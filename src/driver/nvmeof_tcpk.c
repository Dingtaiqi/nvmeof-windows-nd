/* SPDX-FileCopyrightText: 2026 Dingtaiqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * nvmeof_tcpk.c - the NVMe/TCP transport client, in kernel mode, over WSK.
 *
 * Scope, deliberately narrow: open a TCP connection, run the ICReq/ICResp
 * handshake, and issue one NVMe command at a time with in-capsule data.  This
 * file is the transport only.  It does not register with StorPort, it does not
 * present a LUN, and it does not know what a drive letter is; a later step calls
 * NvtcpOpen / NvtcpCommand / NvtcpIdentify from the miniport (src/driver/
 * nvmeofnd.c) at the point where that file currently copies to and from its RAM
 * buffer.
 *
 * Why this file looks the way it does - everything below was learned the
 * expensive way in this project, and the comments name the trap where it bites:
 *
 *   1. WSK, not raw sockets.  WskRegister/WskCaptureProviderNPI are the caller's
 *      job (see src/driver/nvmeofk.c, which is the proven pattern on this
 *      machine); this file takes the captured WSK_PROVIDER_NPI and only creates
 *      sockets from it.
 *
 *   2. Every WSK downcall in this WDK (10.0.26100.0, wsk.h) takes an IRP and
 *      NOTHING else for completion:
 *          PFN_WSK_SOCKET         (Client, AddressFamily, SocketType, Protocol,
 *                                  Flags, SocketContext, Dispatch, OwningProcess,
 *                                  OwningThread, SecurityDescriptor, Irp)
 *          PFN_WSK_BIND           (Socket, LocalAddress, Flags, Irp)
 *          PFN_WSK_CONNECT        (Socket, RemoteAddress, Flags, Irp)
 *          PFN_WSK_SEND           (Socket, Buffer, Flags, Irp)
 *          PFN_WSK_RECEIVE        (Socket, Buffer, Flags, Irp)
 *          PFN_WSK_CLOSE_SOCKET   (Socket, Irp)
 *      There is no "completion routine" argument anywhere in that list, so the
 *      callback WSK invokes is the IRP's own completion routine, attached with
 *      IoSetCompletionRoutine, and its prototype is IO_COMPLETION_ROUTINE:
 *          NTSTATUS (PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
 *      That is the signature the brief calls out, and it is declared below as
 *      "static IO_COMPLETION_ROUTINE NvtcpkIrpComplete;" so the compiler checks
 *      the definition against the type instead of trusting a comment.
 *
 *   3. The event is initialised ONCE per connection and only ever reset
 *      afterwards (StepStart/StepFinish, copied from nvmeofk.c).  Re-running
 *      KeInitializeEvent on a live dispatcher object corrupts a wait list, and
 *      that specific mistake is what took this project's machine down with 0xA.
 *
 *   4. Every wait has a timeout and a timeout is never ignored: it poisons the
 *      connection.  A poisoned connection is never closed and never freed,
 *      because a WSK request that timed out still owns its IRP, its buffer and
 *      its MDL - and wsk.h says it is illegal to call WskCloseSocket while a
 *      WSK call has not returned control.  So the poison path leaks on purpose,
 *      logs the leak, and refuses to touch the connection again.  A leak is
 *      survivable; a use-after-free at DISPATCH_LEVEL is a bugcheck.
 *
 *   5. Every step is appended to a log buffer and written to the registry, so
 *      that if anything goes wrong the evidence is already on disk.  The write
 *      itself is PASSIVE_LEVEL only (ZwSetValueKey), so at DISPATCH_LEVEL the
 *      text is buffered and flushed at the next PASSIVE_LEVEL flush rather than
 *      risking a registry call at the wrong IRQL.
 *
 *   6. Non-paged pool (ExAllocatePool2/ExFreePoolWithTag) only, no malloc, no
 *      CRT, no C++.  This runs at DISPATCH_LEVEL on the miniport's StartIo path.
 *
 *   7. 0xC000020D is STATUS_CONNECTION_RESET, not STATUS_INVALID_BUFFER_SIZE
 *      (that one is 0xC0000206).  A capsule send that completes with it after a
 *      long pause is the peer having closed an idle connection, and the numbers
 *      in the buffer are innocent: this file's test target closes every
 *      connection whose receive sits idle for 60 seconds (SO_RCVTIMEO in
 *      src/nvmeof_tcp.c's targetConnThread) and says nothing about it.  The
 *      repair is a reconnect, not a bigger buffer, and it is safe precisely
 *      because that failure happens before a single byte was transmitted
 *      (IoStatus.Information = 0), so the command was never executed and may be
 *      sent again.  See NvtcpkReconnect below.
 */

/* The public contract, the NVMe opcode values and the WSK headers come from the
 * private header, so the prototypes below cannot drift from what the miniport
 * will compile against. */
#include "nvmeof_tcpk.h"

/* va_list/va_start/va_arg for the log formatter.  This is the compiler's own
 * header (intrinsics, no CRT runtime), which is why it is allowed here while
 * stdio/printf are not.  nvmeofk.c gets the same macros indirectly through
 * <ntstrsafe.h>; this file does not use ntstrsafe, so it says what it needs. */
#include <stdarg.h>

/* The PDU layer, reused verbatim from the user-mode stack: src/nvmeof_tcp.h is
 * header-only and all-static, it only needs stdint types, memset and memcpy -
 * all of which a kernel build has.  It is included rather than copied so there
 * is exactly one definition of the wire format in this project.
 *
 * 4505 = "unreferenced local function removed": the header defines helpers for
 * the target side as well, and this file is the initiator, so some of them are
 * necessarily unused.  That is a property of a header-only library, not a bug in
 * this file, so the warning is disabled around the include only. */
#pragma warning(push)
#pragma warning(disable: 4505)
#include "../nvmeof_tcp.h"
#pragma warning(pop)

/* ------------------------------------------------------------------------- */
/*  Constants                                                                */
/* ------------------------------------------------------------------------- */

/* "ntck" reversed - the same convention as nvmeofk.c's 'kfon'. Every pool tag in
 * a dump should say which file allocated it. */
#define NVMEOF_TCPK_TAG            'kctn'

#define NVMEOF_TCPK_MAGIC          0x4E54434Bu   /* 'NTCK', sanity marker on a conn */

/* Capsule data is still capped at NVMEOF_TCPK_MAX_DATA (4096), but a command may
 * move more than that as data PDUs: a Read's payload arrives as C2HData (handled
 * in the receive loop) and a Write's payload is pushed as H2CData by
 * NvtcpkSendDataPdus.  The R2T exchange stays unimplemented because this transport
 * advertises maxr2t = 0, so a conforming peer never sends one.  This is only the
 * sanity ceiling for a single command. */
#define NVMEOF_TCPK_IO_MAX         (1024u * 1024u)

/* One data PDU carries up to 64 KiB, i.e. a whole command at the default chunk
 * size.  Every PDU costs the host a WSK receive IRP, and 4 KiB PDUs made a 64 KiB
 * read seventeen of them - about 1.7 ms per command and 37 MB/s on Z:.  The peer
 * (nvmeof_tcp.c) sends C2HData in the same size, so one command is two IRPs. */
#define NVMEOF_TCPK_DATA_PDU_MAX   65536u

/* One buffer serves every PDU in both directions.  The slack covers the capsule
 * header (72) plus whatever padding the peer's cpda demands for the data offset
 * (up to 128 is accepted), plus the 4-byte tail pad of a received PDU. */
#define NVMEOF_TCPK_BUF_BYTES      (NVMEOF_TCP_CCMD_HDR_LEN + 256u + NVMEOF_TCPK_DATA_PDU_MAX)

/* IPPROTO_TCP.  The kernel-mode copies of ws2def.h do not define it (it lives in
 * the user-mode winsock headers), so the value is spelled out here. 6 is TCP. */
#define NVMEOF_TCPK_IPPROTO_TCP    6

/* Timeouts, in milliseconds.  Nothing here waits forever: an infinite wait in a
 * kernel driver is a machine that dies slowly with no dump at all. */
#define NVMEOF_TCPK_CONNECT_MS     20000u
#define NVMEOF_TCPK_HS_MS          20000u    /* ICReq send + ICResp receive        */
#define NVMEOF_TCPK_SEND_MS        10000u
#define NVMEOF_TCPK_RECV_MS        30000u    /* one command's response             */
#define NVMEOF_TCPK_CLOSE_MS       10000u

/* A peer asking for a capsule data alignment larger than this is not a peer that
 * this transport can serve out of a 4 KiB in-capsule window, and it is also the
 * sanity bound on the offset arithmetic in NvtcpkCommandEx. */
#define NVMEOF_TCPK_MAX_CPDA       128u

/* NVMe command set.  The opcodes and Identify CNS values are in the header
 * (NVMEOF_TCPK_OPC_*, NVMEOF_TCPK_CNS_*), because the miniport passes them in.
 * The SQE and CQE field offsets below are what src/tcp_identify_selftest.c and
 * the working user-mode initiator (src/nvmeof_tcp.c, hostCmd) both use:
 *   SQE: +0 opcode | +1 flags | +2..3 CID | +4..7 NSID | +8..23 MPTR |
 *        +24..39 DPTR (SGL/PRP) | +40 CDW10 | +44 CDW11 | +48 CDW12 ... +60 CDW15
 *   CQE: +8 DW2 (SQHD | SQID<<16) | +12 DW3 (CID | P<<16 | SC<<17) */
#define NVMEOF_TCPK_SQE_CDW10      40u
#define NVMEOF_TCPK_SQE_CDW11      44u
#define NVMEOF_TCPK_SQE_CDW12      48u
#define NVMEOF_TCPK_CQE_DW3        12u
#define NVMEOF_TCPK_CQE_SC_SHIFT   17u

#define NVMEOF_TCPK_STATUS_UNKNOWN 0xFFu

/* Registry location: the same key every probe and driver in this project writes
 * to, so one reg query shows the whole story. */
#define NVMEOF_TCPK_REG_KEY        L"\\Registry\\Machine\\Software\\NVMeoFProbe"

/* The largest number of PDUs one command may take to answer.  A read answered
 * with C2HData PDUs needs a handful; anything beyond this is a peer that is not
 * speaking the protocol and the connection is dropped rather than followed. */
#define NVMEOF_TCPK_MAX_PDUS_PER_CMD 64u

/* ------------------------------------------------------------------------- */
/*  Logging: a kernel driver's only output is the registry.                   */
/*                                                                           */
/*  This is nvmeofk.c's logf/writeLog, kept in the same shape on purpose: the  */
/*  formatter is hand-rolled because the RtlStringCbVPrintf family and the     */
/*  ANSI printf helpers pull UCRT symbols a driver cannot link.  Supported     */
/*  specifiers: %s %u %d %x %X %p %c %%.                                      */
/* ------------------------------------------------------------------------- */

static char   g_log[8192];
static size_t g_logLen = 0;
static ULONG  g_logDropped = 0;

/*  Never refuse the NEW bytes.  The old behaviour was: once the 8 KiB filled up, every
 *  later line was counted as dropped and thrown away - so a long session lost exactly
 *  the late failures the log exists for.  When there is no room, discard the OLDEST
 *  half (a 4 KiB move, once per ~4 KiB of text) and keep appending; g_logDropped counts
 *  how many times the window rolled, so a reader knows the beginning is gone. */
static void NvtcpkLogRaw(const char* s, size_t n)
{
    if (n + 3 >= sizeof(g_log)) {
        g_logDropped++;                     /* a single line larger than the whole buffer */
        return;
    }
    if (g_logLen + n + 3 >= sizeof(g_log)) {
        size_t keep = sizeof(g_log) / 2;
        if (g_logLen > keep) {
            RtlMoveMemory(g_log, g_log + (g_logLen - keep), keep);
            g_logLen = keep;
        } else {
            g_logLen = 0;
        }
        g_logDropped++;
    }
    RtlCopyMemory(g_log + g_logLen, s, n);
    g_logLen += n;
}

static void NvtcpkLogNum(unsigned long long v, int base, int width, int upper, int padZero)
{
    char tmp[24];
    int i = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[(int)(v % (unsigned)base)];
        v /= (unsigned)base;
    }
    while (i < width && i < (int)sizeof(tmp)) {
        tmp[i++] = padZero ? '0' : ' ';
    }
    while (i > 0) {
        NvtcpkLogRaw(&tmp[--i], 1);
    }
}

static void NvtcpkLogF(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char* p = fmt; *p != 0; p++) {
        int width = 0, padZero = 0, longs = 0;
        if (*p != '%') {
            NvtcpkLogRaw(p, 1);
            continue;
        }
        p++;
        if (*p == '0') { padZero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { longs++; p++; }
        switch (*p) {
        case 'u':
            /* LLP64: `unsigned long` is 32 bits, `unsigned long long` is 64, so
             * only %llu reads a 64-bit argument. Nothing here uses %lu; it is
             * spelled the same way nvmeofk.c does so the two agree. */
            NvtcpkLogNum((longs >= 2) ? va_arg(ap, unsigned long long)
                                      : (unsigned long long)va_arg(ap, unsigned int),
                         10, width, 0, padZero);
            break;
        case 'd': {
            int iv = va_arg(ap, int);
            if (iv < 0) { NvtcpkLogRaw("-", 1); iv = -iv; }
            NvtcpkLogNum((unsigned long long)iv, 10, width, 0, padZero);
            break;
        }
        case 'X':
            NvtcpkLogNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 1, padZero);
            break;
        case 'x':
            NvtcpkLogNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 0, padZero);
            break;
        case 'p':
            NvtcpkLogRaw("0x", 2);
            NvtcpkLogNum((unsigned long long)(ULONG_PTR)va_arg(ap, PVOID), 16, 16, 0, 1);
            break;
        case 's': {
            const char* sv = va_arg(ap, const char*);
            if (sv == NULL) { sv = "(null)"; }
            while (*sv != 0) { NvtcpkLogRaw(sv, 1); sv++; }
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            NvtcpkLogRaw(&cv, 1);
            break;
        }
        case '%':
            NvtcpkLogRaw("%", 1);
            break;
        default:
            NvtcpkLogRaw("%", 1);
            NvtcpkLogRaw(p, 1);
            break;
        }
    }
    va_end(ap);
    NvtcpkLogRaw("\r\n", 2);
}

/* Publish the buffer.  Registry calls are PASSIVE_LEVEL only, so a flush from a
 * DISPATCH_LEVEL command path would be a violation - it is remembered as a count
 * and the text stays buffered until a PASSIVE flush carries it out. */
static void NvtcpkLogFlush(void)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (g_logLen == 0) {
        return;
    }
    RtlInitUnicodeString(&keyName, NVMEOF_TCPK_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    RtlInitUnicodeString(&valueName, L"nvtcpk_log");
    /* The value is a REG_SZ, so it must be NUL terminated.  After a rollover the byte
     * past g_logLen is old text, not zero, so write the terminator explicitly instead
     * of hoping for one.  NvtcpkLogRaw always leaves three bytes of headroom. */
    g_log[g_logLen] = 0;
    ZwSetValueKey(key, &valueName, 0, REG_SZ, g_log, (ULONG)g_logLen + 1);

    RtlInitUnicodeString(&valueName, L"nvtcpk_ok");
    {
        ULONG ok = 1;
        ZwSetValueKey(key, &valueName, 0, REG_DWORD, &ok, sizeof(ok));
    }
    if (g_logDropped != 0) {
        RtlInitUnicodeString(&valueName, L"nvtcpk_logdropped");
        ZwSetValueKey(key, &valueName, 0, REG_DWORD, &g_logDropped, sizeof(g_logDropped));
    }
    ZwClose(key);
}

/* One DWORD per interesting fact, so a human does not have to grep the log. */
static void NvtcpkTraceValue(PCWSTR name, ULONG v)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&keyName, NVMEOF_TCPK_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    RtlInitUnicodeString(&valueName, name);
    ZwSetValueKey(key, &valueName, 0, REG_DWORD, &v, sizeof(v));
    ZwClose(key);
}

/* ------------------------------------------------------------------------- */
/*  Async completion context and the wait helpers                            */
/* ------------------------------------------------------------------------- */

typedef struct _NVMEOF_TCPK_ASYNC {
    KEVENT   done;
    NTSTATUS status;
    ULONG_PTR info;          /* Irp->IoStatus.Information: bytes, or the socket
                              * pointer for WskSocket. ULONG_PTR on purpose: on
                              * x64 truncating that pointer to 32 bits is a
                              * guaranteed crash the moment it is used. */
    LONG     seq;            /* operation number this context was last armed for */
    LONG     doneSeq;        /* operation number of the completion that ended it */
    BOOLEAN  initialized;
    BOOLEAN  dead;           /* a timed-out operation may still complete at any
                              * time; the connection is poisoned when this is set */
} NVMEOF_TCPK_ASYNC;

/* Measurement, not theory.  Three questions that a failure report has to answer
 * and that no amount of reading the code can:
 *
 *   - irpAlloc - irpFree: are this file's IRPs leaking?  Every IRP it allocates
 *     is freed through NvtcpkIrpFree, so the difference is exactly the number
 *     still outstanding.  A leak would fail after a fixed count; the observed
 *     failures did not, but the counter is what says so.
 *   - opsInFlight: can a send and a receive overlap on the one shared completion
 *     context?  The design serialises them (one command at a time, one thread),
 *     so this should never exceed 1.  If it ever reaches 2, the overlap is real.
 *   - staleCompletions: did a wait end on a completion belonging to a different
 *     operation?  seq is stamped when the context is armed and copied into
 *     doneSeq by the completion routine, so a mismatch is the exact corruption a
 *     shared context would produce - counted rather than dismissed.
 *
 * All of it is dumped with a send failure and published as a registry DWORD. */
static LONG g_irpAlloc = 0;
static LONG g_irpFree = 0;
static LONG g_opSeq = 0;
static LONG g_opsInFlight = 0;
static LONG g_maxOpsInFlight = 0;
static LONG g_opCompletions = 0;
static LONG g_staleCompletions = 0;
static LONG g_reconnects = 0;

/* A status the peer's death produces, as opposed to one this file's arithmetic
 * could produce.  Only these are worth re-establishing a connection for; anything
 * else is a bug here and must stay visible. */
static BOOLEAN NvtcpkIsSocketLoss(NTSTATUS st)
{
    return (st == STATUS_CONNECTION_RESET ||
            st == STATUS_CONNECTION_ABORTED ||
            st == STATUS_CONNECTION_DISCONNECTED ||
            st == STATUS_CONNECTION_INVALID ||
            st == STATUS_INVALID_CONNECTION);
}

/* The name of the status, for the two that this project has already misread.
 * A number alone cost a session: 0xC000020D was taken for a buffer-size
 * complaint for a whole day. */
static const char* NvtcpkStatusName(NTSTATUS st)
{
    switch (st) {
    case STATUS_SUCCESS:                   return "STATUS_SUCCESS";
    case STATUS_PENDING:                   return "STATUS_PENDING";
    case STATUS_INVALID_BUFFER_SIZE:       return "STATUS_INVALID_BUFFER_SIZE";
    case STATUS_CONNECTION_RESET:          return "STATUS_CONNECTION_RESET";
    case STATUS_CONNECTION_ABORTED:        return "STATUS_CONNECTION_ABORTED";
    case STATUS_CONNECTION_DISCONNECTED:   return "STATUS_CONNECTION_DISCONNECTED";
    case STATUS_CONNECTION_INVALID:        return "STATUS_CONNECTION_INVALID";
    case STATUS_INVALID_CONNECTION:        return "STATUS_INVALID_CONNECTION";
    case STATUS_IO_TIMEOUT:                return "STATUS_IO_TIMEOUT";
    case STATUS_DEVICE_NOT_READY:          return "STATUS_DEVICE_NOT_READY";
    case STATUS_INVALID_HANDLE:            return "STATUS_INVALID_HANDLE";
    default:                               return "other";
    }
}

/* Declared with the WDK's own function type so the definition below is checked
 * against IO_COMPLETION_ROUTINE instead of a comment in a header. */
static IO_COMPLETION_ROUTINE NvtcpkIrpComplete;

/* Initialise ONCE, then only ever reset.  KeResetEvent changes SignalState and
 * never touches the wait list, which is exactly the difference that matters. */
static void NvtcpkStepStart(NVMEOF_TCPK_ASYNC* a)
{
    LONG now;

    if (!a->initialized) {
        KeInitializeEvent(&a->done, NotificationEvent, FALSE);
        a->initialized = TRUE;
    } else {
        if (a->dead) {
            /* Signalling this event late is harmless; reinitialising it is not,
             * and that no longer happens. */
            NvtcpkLogF("!! async context %p was left dead by a timeout and is being reused", a);
        }
        KeResetEvent(&a->done);
    }
    a->status = STATUS_PENDING;
    a->info = 0;
    a->seq = InterlockedIncrement(&g_opSeq);
    a->doneSeq = 0;
    now = InterlockedIncrement(&g_opsInFlight);
    if (now > g_maxOpsInFlight) {
        g_maxOpsInFlight = now;
    }
}

/* The return value of the WSK downcall and the status the IRP completed with are
 * two different things:
 *   - STATUS_PENDING from the downcall: the IRP will complete later, wait for it.
 *   - anything else: the request either completed inline (its completion routine
 *     has already run, so a->status is authoritative) or never started (the IRP
 *     was not completed and must not be waited on).
 * Getting that distinction wrong is an infinite wait or a freed IRP, so it is
 * spelled out here rather than inlined at six call sites. */
static NTSTATUS NvtcpkStepFinish(NTSTATUS callStatus, NVMEOF_TCPK_ASYNC* a, ULONG waitMs, const char* what)
{
    LARGE_INTEGER t;
    NTSTATUS st;

    st = callStatus;
    if (callStatus == STATUS_PENDING) {
        t.QuadPart = -((LONGLONG)waitMs * 10000);
        if (KeWaitForSingleObject(&a->done, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
            a->dead = TRUE;
            InterlockedDecrement(&g_opsInFlight);
            NvtcpkLogF("!! %s: no completion after %u ms; its IRP and buffers are still owned by WSK,"
                       " so this connection is poisoned and will be leaked rather than freed",
                       what, waitMs);
            return STATUS_IO_TIMEOUT;
        }
        st = a->status;
    } else if (NT_SUCCESS(callStatus)) {
        /* Inline completion: the completion routine already stored the real
         * status.  Defensive fallback if a provider ever completes without
         * calling the routine. */
        st = (a->status != STATUS_PENDING) ? a->status : STATUS_SUCCESS;
    }

    /* The completion that ended the wait must be this operation's own.  If it is
     * not, the shared context really did deliver somebody else's status and that
     * is reported here rather than returned as this operation's result. */
    if (a->doneSeq != 0 && a->doneSeq != a->seq) {
        InterlockedIncrement(&g_staleCompletions);
        NvtcpkLogF("!! %s: the completion that ended this wait belonged to operation %d,"
                   " not to operation %d (status 0x%08X is not this call's)",
                   what, (int)a->doneSeq, (int)a->seq, (unsigned)st);
    }
    InterlockedDecrement(&g_opsInFlight);
    return st;
}

static NTSTATUS NvtcpkIrpComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    NVMEOF_TCPK_ASYNC* a = (NVMEOF_TCPK_ASYNC*)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (a != NULL && Irp != NULL) {
        a->status = Irp->IoStatus.Status;
        a->info   = Irp->IoStatus.Information;
        /* Which operation this completion belongs to, recorded by the completion
         * itself, so NvtcpkStepFinish can tell a late completion from its own. */
        a->doneSeq = a->seq;
        InterlockedIncrement(&g_opCompletions);
        KeSetEvent(&a->done, IO_NO_INCREMENT, FALSE);
    }
    /* Required for a caller-allocated IRP that WSK completes: it tells the I/O
     * manager to stop unwinding so the caller can free the IRP with IoFreeIrp.
     * Anything else here double-frees. */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ------------------------------------------------------------------------- */
/*  Connection object and the global list                                    */
/*                                                                           */
/*  The list is what makes a pointer-based API safe: an entry is reachable     */
/*  exactly while it is in the list, every user takes a reference while it     */
/*  holds the pointer, and the memory is released by the last reference.      */
/*  Without that, "close twice" is a use-after-free dressed up as idempotence. */
/* ------------------------------------------------------------------------- */

typedef struct _NVMEOF_TCPK_CONN {
    ULONG                     magic;
    LIST_ENTRY                link;
    BOOLEAN                   linked;      /* still on g_connList             */
    BOOLEAN                   immortal;    /* leaked on purpose, never freed   */
    BOOLEAN                   poisoned;    /* a wait timed out: hands off      */
    BOOLEAN                   broken;      /* protocol error: stream is garbage */
    BOOLEAN                   needReconnect; /* the peer closed: re-establish  */
    LONG                      refs;        /* 1 = the list's own reference      */
    LONG                      busy;        /* 1 = a command is using the socket */
    LONG                      reconnects;  /* times this connection was rebuilt */
    PWSK_SOCKET               socket;
    const WSK_PROVIDER_DISPATCH* providerDispatch;  /* only WskSocket needs it */
    PVOID                     wskClient;   /* WSK_CLIENT from the provider NPI  */
    SOCKADDR_IN               remote;
    UCHAR                     cpda;        /* peer's capsule data alignment     */
    USHORT                    cidNext;
    ULONG                     commands;
    ULONG                     bytesIn;
    ULONG                     bytesOut;
    NVMEOF_TCPK_ASYNC         async;       /* one event, initialised once       */
    PVOID                     pduBuf;      /* non-paged, outbound PDU complete  */
    PMDL                      pduMdl;
    PVOID                     rspBuf;      /* non-paged, inbound PDU, staged    */
    PMDL                      rspMdl;
} NVMEOF_TCPK_CONN, *PNVMEOF_TCPK_CONN;

/* Defined below their first use (NvtcpOpen's failure path frees through them). */
static void NvtcpkFreeBuffers(PNVMEOF_TCPK_CONN c);
static NTSTATUS NvtcpkCommandEx(PVOID conn, UINT8 opcode, UINT32 nsid, UINT32 cdw10,
                                UINT32 cdw12, PVOID inData, ULONG inBytes,
                                PVOID outData, ULONG outBytes, UINT8* statusOut,
                                ULONG* dataLenOut);

/* Statically a list head that points at itself - a valid address constant, so
 * no one-time init is needed for it and NvtcpOpen is safe to call before any
 * other function in this file. */
static LIST_ENTRY  g_connList = { &g_connList, &g_connList };
static KSPIN_LOCK  g_connLock;
static LONG        g_globalsReady = 0;
static LONG        g_openCount = 0;
static LONG        g_poisonCount = 0;

/* KeInitializeSpinLock writes the statically-zero state, so publishing it behind
 * one interlocked exchange means the loser of a first-call race finds the list
 * already valid and the lock already usable either way. */
static void NvtcpkGlobalsInit(void)
{
    if (InterlockedCompareExchange(&g_globalsReady, 1, 0) == 0) {
        KeInitializeSpinLock(&g_connLock);
    }
}

/* A timeout is the one failure that must never be handled by freeing anything:
 * the WSK request still owns its IRP, and that IRP's completion routine points
 * into this connection.  Marking the connection poisoned makes every later call
 * refuse it and makes NvtcpClose leak it on purpose instead of freeing it. */
static void NvtcpkSetPoisoned(PNVMEOF_TCPK_CONN c)
{
    if (!c->poisoned) {
        c->poisoned = TRUE;
        InterlockedIncrement(&g_poisonCount);
    }
}

/* Find and take a reference in one step, under the list lock. */
static PNVMEOF_TCPK_CONN NvtcpkLookupRef(PVOID conn)
{
    PLIST_ENTRY e;
    KIRQL irql;
    PNVMEOF_TCPK_CONN found = NULL;

    if (conn == NULL) {
        return NULL;
    }
    KeAcquireSpinLock(&g_connLock, &irql);
    for (e = g_connList.Flink; e != &g_connList; e = e->Flink) {
        PNVMEOF_TCPK_CONN c = CONTAINING_RECORD(e, NVMEOF_TCPK_CONN, link);
        if ((PVOID)c == conn) {
            if (c->magic == NVMEOF_TCPK_MAGIC) {
                InterlockedIncrement(&c->refs);
                found = c;
            }
            break;
        }
    }
    KeReleaseSpinLock(&g_connLock, irql);
    return found;
}

static void NvtcpkDestroyConn(PNVMEOF_TCPK_CONN c);

static void NvtcpkDropRef(PNVMEOF_TCPK_CONN c)
{
    if (c == NULL) {
        return;
    }
    if (InterlockedDecrement(&c->refs) == 0) {
        NvtcpkDestroyConn(c);
    }
}

static const WSK_PROVIDER_CONNECTION_DISPATCH* NvtcpkDispatch(PNVMEOF_TCPK_CONN c)
{
    /* WSK_SOCKET.Dispatch is CONST VOID*; the table depends on the socket type,
     * and WSK_FLAG_CONNECTION_SOCKET means the connection table. */
    return (const WSK_PROVIDER_CONNECTION_DISPATCH*)c->socket->Dispatch;
}

/* ------------------------------------------------------------------------- */
/*  IRP plumbing                                                             */
/* ------------------------------------------------------------------------- */

/* WSK completes a caller-allocated IRP; IoFreeIrp is the caller's job.  Each
 * operation gets its own IRP so the lifetime rules stay local to one call -
 * an IRP that outlives its operation is the one that gets freed twice. */
static PIRP NvtcpkIrpNew(PVOID context)
{
    PIRP irp = IoAllocateIrp(1, FALSE);
    if (irp == NULL) {
        return NULL;
    }
    InterlockedIncrement(&g_irpAlloc);
    IoSetCompletionRoutine(irp, NvtcpkIrpComplete, context, TRUE, TRUE, TRUE);
    return irp;
}

/* Every IoFreeIrp in this file goes through here.  One wrapper, not seven hand
 * edits, so the count cannot drift from the frees: alloc - free is the number of
 * IRPs still outstanding, and a send failure that reports a non-zero figure has
 * found a leak instead of a provider fault. */
static void NvtcpkIrpFree(PIRP Irp)
{
    if (Irp == NULL) {
        return;
    }
    InterlockedIncrement(&g_irpFree);
    IoFreeIrp(Irp);
}

/* ------------------------------------------------------------------------- */
/*  Socket downcalls                                                         */
/* ------------------------------------------------------------------------- */

/* A socket dispatch table is handed to WskSocket even though no event callback
 * is ever enabled (SO_WSK_EVENT_CALLBACK is never issued): wsk.h allows NULL
 * here, and a zeroed table is the same thing to a provider that copies it. */
static const WSK_CLIENT_CONNECTION_DISPATCH g_socketDispatch = { NULL, NULL, NULL };

static NTSTATUS NvtcpkSocketCreate(PNVMEOF_TCPK_CONN c, PWSK_CLIENT client)
{
    NTSTATUS st;
    PIRP irp = NvtcpkIrpNew(&c->async);
    const WSK_PROVIDER_DISPATCH* d;

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    d = (const WSK_PROVIDER_DISPATCH*)c->providerDispatch;
    NvtcpkStepStart(&c->async);
    st = d->WskSocket(client, AF_INET, SOCK_STREAM, NVMEOF_TCPK_IPPROTO_TCP,
                      WSK_FLAG_CONNECTION_SOCKET, c, &g_socketDispatch,
                      NULL, NULL, NULL, irp);
    st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_HS_MS, "WskSocket");
    if (st == STATUS_IO_TIMEOUT) {
        NvtcpkSetPoisoned(c);
        return st;                      /* IRP still owned by WSK: leak it */
    }
    NvtcpkIrpFree(irp);
    if (NT_SUCCESS(st)) {
        /* PFN_WSK_SOCKET returns the new socket in Irp->IoStatus.Information. */
        c->socket = (PWSK_SOCKET)c->async.info;
        if (c->socket == NULL || c->socket->Dispatch == NULL) {
            return STATUS_UNSUCCESSFUL;
        }
    }
    return st;
}

static NTSTATUS NvtcpkBind(PNVMEOF_TCPK_CONN c)
{
    NTSTATUS st;
    PIRP irp;
    SOCKADDR_IN local;

    /* A wildcard bind asks the transport to pick the source address.  It does not
     * touch any adapter, route or DNS setting; it is what every client socket
     * does before connect.  Failure is not fatal: the connect below is what
     * matters and the provider may auto-bind. */
    RtlZeroMemory(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.S_un.S_addr = 0;

    irp = NvtcpkIrpNew(&c->async);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NvtcpkStepStart(&c->async);
    st = NvtcpkDispatch(c)->WskBind(c->socket, (PSOCKADDR)&local, 0, irp);
    st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_HS_MS, "WskBind");
    if (st == STATUS_IO_TIMEOUT) {
        NvtcpkSetPoisoned(c);
        return st;
    }
    NvtcpkIrpFree(irp);
    return st;
}

static NTSTATUS NvtcpkConnect(PNVMEOF_TCPK_CONN c)
{
    NTSTATUS st;
    PIRP irp = NvtcpkIrpNew(&c->async);

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NvtcpkStepStart(&c->async);
    st = NvtcpkDispatch(c)->WskConnect(c->socket, (PSOCKADDR)&c->remote, 0, irp);
    st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_CONNECT_MS, "WskConnect");
    if (st == STATUS_IO_TIMEOUT) {
        NvtcpkSetPoisoned(c);
        return st;
    }
    NvtcpkIrpFree(irp);
    return st;
}

/* Close and wait, with a timeout.  wsk.h: WskCloseSocket may only be called when
 * no other WSK call on the socket has yet returned control, which is why every
 * caller of this function has already established that nothing is in flight. */
static NTSTATUS NvtcpkSocketClose(PNVMEOF_TCPK_CONN c)
{
    NTSTATUS st;
    PIRP irp;

    if (c->socket == NULL) {
        return STATUS_SUCCESS;
    }
    irp = NvtcpkIrpNew(&c->async);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NvtcpkStepStart(&c->async);
    st = NvtcpkDispatch(c)->WskCloseSocket(c->socket, irp);
    st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_CLOSE_MS, "WskCloseSocket");
    if (st == STATUS_IO_TIMEOUT) {
        NvtcpkSetPoisoned(c);
        return st;
    }
    NvtcpkIrpFree(irp);
    if (NT_SUCCESS(st)) {
        c->socket = NULL;
    }
    return st;
}

/* ------------------------------------------------------------------------- */
/*  Diagnostics and the IRP/operation counters                               */
/*                                                                           */
/*  A send that fails is not automatically a bug here: the status says which  */
/*  side gave up.  The numbers below are what distinguishes "this file leaked  */
/*  or mis-sized something" from "the peer closed the connection": IRP         */
/*  accounting, the number of operations ever in flight at once, and whether   */
/*  any completion was delivered to a wait that did not own it.               */
/* ------------------------------------------------------------------------- */

typedef struct _NVMEOF_TCPK_DIAG {
    USHORT   cid;
    UCHAR    opcode;
    ULONG    inBytes;
    ULONG    outBytes;
    ULONG    total;
    ULONG    mdlBytes;
    NTSTATUS callSt;
    NTSTATUS irpSt;
} NVMEOF_TCPK_DIAG;

static NVMEOF_TCPK_DIAG  g_diagRing[8];
static ULONG             g_diagAt = 0;
static ULONG             g_diagSeen = 0;
static ULONG             g_sendSeen = 0;
static ULONG             g_rspSeen = 0;
static NVMEOF_TCPK_DIAG* g_diagCur = NULL;

/* Milliseconds since boot.  KeQueryInterruptTime is 100 ns units and callable at
 * any IRQL, so a log line can show the gap between two commands - which is what
 * a peer's idle timeout turns on and what a command count cannot show. */
static ULONG NvtcpkNowMs(void)
{
    return (ULONG)(KeQueryInterruptTime() / 10000u);
}

static void NvtcpkDiagDump(PNVMEOF_TCPK_CONN c, const char* why)
{
    ULONG n = (g_diagSeen < 8) ? g_diagSeen : 8;
    ULONG i;

    NvtcpkLogF("--- %s after %u commands: conn %p cidNext=%u ok=%u in=%u out=%u"
               " broken=%u poisoned=%u needReconnect=%u reconnects=%u",
               why, g_diagSeen, c, (unsigned)c->cidNext, c->commands, c->bytesIn,
               c->bytesOut, (unsigned)c->broken, (unsigned)c->poisoned,
               (unsigned)c->needReconnect, (unsigned)c->reconnects);
    NvtcpkLogF("    irps: alloc=%u free=%u outstanding=%u | ops: issued=%u inFlight=%u"
               " maxInFlight=%u completions=%u staleCompletions=%u",
               (unsigned)g_irpAlloc, (unsigned)g_irpFree,
               (unsigned)(g_irpAlloc - g_irpFree), (unsigned)g_opSeq,
               (unsigned)g_opsInFlight, (unsigned)g_maxOpsInFlight,
               (unsigned)g_opCompletions, (unsigned)g_staleCompletions);
    NvtcpkLogF("    failing status: 0x%08X %s info=%u (0xC000020D is"
               " STATUS_CONNECTION_RESET; STATUS_INVALID_BUFFER_SIZE is 0xC0000206)",
               (unsigned)c->async.status, NvtcpkStatusName(c->async.status),
               (unsigned)c->async.info);
    NvtcpkLogF("    buffers: pduBuf=%p pduMdl=%p rspBuf=%p rspMdl=%p",
               c->pduBuf, c->pduMdl, c->rspBuf, c->rspMdl);
    {
        NVMEOF_TCP_HDR ph;
        nvmeofTcpReadHdr((const uint8_t*)c->pduBuf, &ph);
        NvtcpkLogF("    capsule still in pduBuf: type=0x%02X flags=0x%02X hlen=%u pdo=%u plen=%u",
                   (unsigned)ph.type, (unsigned)ph.flags, (unsigned)ph.hlen,
                   (unsigned)ph.pdo, (unsigned)ph.plen);
    }
    for (i = 0; i < n; i++) {
        NVMEOF_TCPK_DIAG* d = &g_diagRing[(g_diagAt + 8 - n + i) & 7u];
        NvtcpkLogF("    cid=%u op=0x%02X in=%u out=%u total=%u mdl=%u callSt=0x%08X irpSt=0x%08X",
                   (unsigned)d->cid, (unsigned)d->opcode, d->inBytes, d->outBytes,
                   d->total, d->mdlBytes, (unsigned)d->callSt, (unsigned)d->irpSt);
    }
}

/* Send exactly `len` bytes of c->pduBuf.  A connection-oriented WSK send takes
 * the whole buffer, so one call is one PDU - no partial-send loop, because a
 * second copy of bytes the peer already has is silent data corruption. */
static NTSTATUS NvtcpkSend(PNVMEOF_TCPK_CONN c, ULONG len)
{
    NTSTATUS st;
    NTSTATUS callSt;
    PIRP irp;
    WSK_BUF buf;
    ULONG mdlBytes;
    const WSK_PROVIDER_CONNECTION_DISPATCH* d;

    if (len == 0 || len > NVMEOF_TCPK_BUF_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }
    /* The MDL handed to WskSend must describe the buffer this file allocated and
     * must cover at least `len` bytes of it.  Checked on every send: if the
     * provider really is refusing the buffer, these three numbers say whether it
     * is right to. */
    mdlBytes = (c->pduMdl != NULL) ? MmGetMdlByteCount(c->pduMdl) : 0;
    if (c->pduMdl == NULL || mdlBytes < len ||
        MmGetMdlVirtualAddress(c->pduMdl) != c->pduBuf) {
        NvtcpkLogF("!! send: the PDU MDL does not describe pduBuf (mdl=%p bytes=%u va=%p"
                   " buf=%p len=%u); rebuilding it",
                   c->pduMdl, mdlBytes,
                   (c->pduMdl != NULL) ? MmGetMdlVirtualAddress(c->pduMdl) : NULL,
                   c->pduBuf, len);
        if (c->pduMdl != NULL) {
            IoFreeMdl(c->pduMdl);
            c->pduMdl = NULL;
        }
        c->pduMdl = IoAllocateMdl(c->pduBuf, NVMEOF_TCPK_BUF_BYTES, FALSE, FALSE, NULL);
        if (c->pduMdl == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(c->pduMdl);
        mdlBytes = MmGetMdlByteCount(c->pduMdl);
    }
    irp = NvtcpkIrpNew(&c->async);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    d = NvtcpkDispatch(c);
    buf.Mdl = c->pduMdl;
    buf.Offset = 0;
    buf.Length = len;
    NvtcpkStepStart(&c->async);
    st = d->WskSend(c->socket, &buf, WSK_FLAG_NODELAY, irp);
    callSt = st;
    st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_SEND_MS, "WskSend");
    if (g_diagCur != NULL) {
        g_diagCur->callSt = callSt;
        g_diagCur->irpSt = c->async.status;
    }
    if (g_sendSeen < 8 || !NT_SUCCESS(st)) {
        ULONG allocNow = (ULONG)InterlockedCompareExchange(&g_irpAlloc, 0, 0);
        ULONG freeNow  = (ULONG)InterlockedCompareExchange(&g_irpFree, 0, 0);
        NvtcpkLogF("  send #%u len=%u off=%u mdlBytes=%u bufferBytes=%u irql=%u t=%ums"
                   " callSt=0x%08X irpSt=0x%08X info=%u irpOut=%u",
                   g_sendSeen, len, (unsigned)buf.Offset, mdlBytes,
                   (unsigned)NVMEOF_TCPK_BUF_BYTES, (unsigned)KeGetCurrentIrql(),
                   NvtcpkNowMs(), (unsigned)callSt, (unsigned)c->async.status,
                   (unsigned)c->async.info, (unsigned)(allocNow - freeNow));
    }
    g_sendSeen++;
    if (st == STATUS_IO_TIMEOUT) {
        NvtcpkSetPoisoned(c);
        return st;                      /* IRP, MDL and buffer stay with WSK */
    }
    NvtcpkIrpFree(irp);
    if (NT_SUCCESS(st)) {
        c->bytesOut += len;
    } else if (NvtcpkIsSocketLoss(st)) {
        /* Nothing reached the wire: WSK reports Information = 0 when it refuses a
         * send, so the capsule is still whole in pduBuf and the command was never
         * executed.  That is exactly what makes reconnect-and-resend safe here;
         * a failure after bytes went out would not be. */
        c->needReconnect = TRUE;
        NvtcpkLogF("  send: the socket is gone (0x%08X %s, info=%u); the capsule was not"
                   " transmitted, so the connection is re-established and it is sent again",
                   (unsigned)st, NvtcpkStatusName(st), (unsigned)c->async.info);
    } else {
        /* A failed send leaves the peer holding a partial PDU, so the stream is
         * out of step even though nothing here is unsafe to touch. */
        c->broken = TRUE;
    }
    return st;
}

/* Receive exactly `want` bytes into c->rspBuf + off.  TCP splits where it likes,
 * so this loops; WSK_FLAG_WAITALL asks the provider to hold the completion until
 * the buffer is full, and the loop is still there for the case where it does not. */
static NTSTATUS NvtcpkRecv(PNVMEOF_TCPK_CONN c, ULONG off, ULONG want)
{
    ULONG got = 0;
    const WSK_PROVIDER_CONNECTION_DISPATCH* d = NvtcpkDispatch(c);

    if (want == 0) {
        return STATUS_SUCCESS;
    }
    if (off > NVMEOF_TCPK_BUF_BYTES || want > NVMEOF_TCPK_BUF_BYTES - off) {
        /* Asking for more than the buffer holds means the caller's PDU arithmetic
         * is wrong, and by this point part of that PDU may already have been
         * read: the stream can no longer be trusted to be in step. */
        c->broken = TRUE;
        NvtcpkLogF("!! receive of %u bytes at offset %u does not fit the %u-byte PDU buffer",
                   want, off, (unsigned)NVMEOF_TCPK_BUF_BYTES);
        return STATUS_INVALID_PARAMETER;
    }
    while (got < want) {
        NTSTATUS st;
        PIRP irp;
        WSK_BUF buf;
        ULONG n;

        irp = NvtcpkIrpNew(&c->async);
        if (irp == NULL) {
            c->broken = TRUE;       /* part of the PDU is already consumed */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        buf.Mdl = c->rspMdl;
        buf.Offset = off + got;
        buf.Length = want - got;
        NvtcpkStepStart(&c->async);
        st = d->WskReceive(c->socket, &buf, WSK_FLAG_WAITALL, irp);
        st = NvtcpkStepFinish(st, &c->async, NVMEOF_TCPK_RECV_MS, "WskReceive");
        if (st == STATUS_IO_TIMEOUT) {
            NvtcpkSetPoisoned(c);
            return st;                  /* IRP, MDL and buffer stay with WSK */
        }
        if (!NT_SUCCESS(st)) {
            NvtcpkIrpFree(irp);
            if (NvtcpkIsSocketLoss(st)) {
                /* The socket itself is gone, so there is no stream left to be out
                 * of step: the next command on a fresh connection starts clean.
                 * The command being read, however, is lost - its bytes are not
                 * coming back - so this call still fails. */
                c->needReconnect = TRUE;
            } else {
                c->broken = TRUE;
            }
            NvtcpkLogF("  recv off=%u want=%u st=0x%08X %s info=%u rspMdl=%p mdlBytes=%u",
                       off, want, (unsigned)st, NvtcpkStatusName(st),
                       (unsigned)c->async.info, c->rspMdl,
                       (c->rspMdl != NULL) ? MmGetMdlByteCount(c->rspMdl) : 0);
            return st;
        }
        n = (ULONG)c->async.info;
        NvtcpkIrpFree(irp);
        if (n == 0) {
            /* A zero-byte completion on a stream socket means the peer closed its
             * side.  A fresh connection is in step again; this command is not. */
            c->needReconnect = TRUE;
            NvtcpkLogF("  recv: the peer closed the connection (0 bytes) after %u of %u"
                       " wanted; the connection will be re-established", got, want);
            return STATUS_CONNECTION_DISCONNECTED;
        }
        if (n > want - got) {
            c->broken = TRUE;
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
        got += n;
        c->bytesIn += n;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  NVMe pieces                                                              */
/* ------------------------------------------------------------------------- */

static void NvtcpkBuildSqe(PUCHAR sqe, UCHAR opcode, USHORT cid, UINT32 nsid,
                           UINT32 cdw10, UINT32 cdw12)
{
    RtlZeroMemory(sqe, NVMEOF_TCP_SQE_LEN);
    sqe[0] = opcode;
    /* +1 flags stays 0: no fused operation, and PSDT = 0 means "no SGL/PRP",
     * which is what in-capsule data uses - TP 8000 locates the payload through
     * the PDU (hlen/plen/pdo), not through the command's DPTR.  The working
     * user-mode initiator in src/nvmeof_tcp.c sends zeros here too. */
    sqe[2] = (UCHAR)(cid & 0xFF);
    sqe[3] = (UCHAR)((cid >> 8) & 0xFF);
    nvmeofTcpPut32(sqe, 4, nsid);
    /* +8..+23 MPTR, +24..+39 DPTR: zero */
    nvmeofTcpPut32(sqe, NVMEOF_TCPK_SQE_CDW10, cdw10);
    nvmeofTcpPut32(sqe, NVMEOF_TCPK_SQE_CDW11, 0);
    nvmeofTcpPut32(sqe, NVMEOF_TCPK_SQE_CDW12, cdw12);
    /* +52..+63 CDW13..CDW15: zero */
}

/* CQE status: DW3 bits 31:17 hold P | SC (bit 16 is the phase).  The project's
 * self-test and its RDMA status reader agree on this, so SC comes back as the
 * low 8 bits of (DW3 >> 17). */
static UCHAR NvtcpkCqeStatus(const UCHAR* cqe)
{
    return (UCHAR)((nvmeofTcpGet32(cqe, NVMEOF_TCPK_CQE_DW3) >> NVMEOF_TCPK_CQE_SC_SHIFT) & 0xFF);
}

static USHORT NvtcpkCqeCid(const UCHAR* cqe)
{
    return (USHORT)(nvmeofTcpGet32(cqe, NVMEOF_TCPK_CQE_DW3) & 0xFFFF);
}

/* The total wire length of a PDU - header + payload rounded up to the peer's
 * capsule data alignment - is nvmeofTcpPduTotal from the included PDU layer;
 * there is deliberately no second definition of that arithmetic here. */

/* ------------------------------------------------------------------------- */
/*  Handshake: ICReq out, ICResp in                                          */
/* ------------------------------------------------------------------------- */

static NTSTATUS NvtcpkHandshake(PNVMEOF_TCPK_CONN c)
{
    NTSTATUS st;
    NVMEOF_TCP_HDR hdr;
    NVMEOF_TCP_IC ic;
    ULONG pad;

    /* The body is written by the header's builder, so the 128-byte layout, the
     * pfv/cpda/dgst placement and the reserved zeros are the ones the self-tests
     * already prove. */
    RtlZeroMemory(&ic, sizeof(ic));
    ic.pfv = 0;
    ic.cpda = (UCHAR)NVMEOF_TCP_PDU_ALIGN;   /* 4: the alignment we can serve */
    ic.dgst = 0;                             /* no digests */
    ic.maxr2t = 0;                           /* no R2T: all data in-capsule */
    nvmeofTcpBuildIc((PUCHAR)c->pduBuf, NVMEOF_TCP_ICREQ, &ic);

    st = NvtcpkSend(c, NVMEOF_TCP_ICREQ_LEN);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("handshake: could not send ICReq, 0x%08X", st);
        return st;
    }
    NvtcpkLogF("handshake: ICReq sent (%u bytes, cpda=%u dgst=0 maxr2t=0)",
               (unsigned)NVMEOF_TCP_ICREQ_LEN, (unsigned)ic.cpda);

    st = NvtcpkRecv(c, 0, NVMEOF_TCP_HDR_LEN);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("handshake: no ICResp header, 0x%08X", st);
        return st;
    }
    nvmeofTcpReadHdr((const uint8_t*)c->rspBuf, &hdr);
    if (hdr.type != NVMEOF_TCP_ICRESP) {
        NvtcpkLogF("handshake: expected ICResp (0x%02X), got 0x%02X - peer is not speaking NVMe/TCP",
                   (unsigned)NVMEOF_TCP_ICRESP, (unsigned)hdr.type);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    if (hdr.hlen != NVMEOF_TCP_ICREQ_LEN) {
        NvtcpkLogF("handshake: ICResp hlen=%u, expected %u",
                   (unsigned)hdr.hlen, (unsigned)NVMEOF_TCP_ICREQ_LEN);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    /* Header first, then the remaining body, so the parse below sees the whole
     * 128 bytes in one place. */
    st = NvtcpkRecv(c, NVMEOF_TCP_HDR_LEN, NVMEOF_TCP_IC_BODY_LEN);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("handshake: ICResp body truncated, 0x%08X", st);
        return st;
    }
    if (nvmeofTcpParseIc((const uint8_t*)c->rspBuf, NVMEOF_TCP_ICREQ_LEN, &ic) != 0) {
        NvtcpkLogF("handshake: ICResp rejected by nvmeofTcpParseIc (pfv=%u cpda=%u)",
                   (unsigned)ic.pfv, (unsigned)ic.cpda);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    /* Digest negotiation: the ICReq asked for none, so a peer that answers with
     * digests on is a peer whose every subsequent PDU would carry 4 trailing
     * bytes this transport does not compute. Refuse now, with the reason logged,
     * instead of failing on the first capsule. */
    if (ic.dgst != 0 || ic.hdgst != 0 || ic.ddgst != 0) {
        NvtcpkLogF("handshake: peer requires digests (dgst=%u hdgst=%u ddgst=%u); this transport"
                   " implements none and refuses rather than sending undigested PDUs",
                   (unsigned)ic.dgst, (unsigned)ic.hdgst, (unsigned)ic.ddgst);
        return STATUS_NOT_SUPPORTED;
    }
    if (ic.cpda < NVMEOF_TCP_PDU_ALIGN || ic.cpda > NVMEOF_TCPK_MAX_CPDA ||
        (ic.cpda & (ic.cpda - 1)) != 0) {
        NvtcpkLogF("handshake: peer cpda=%u is not a power of two in [%u,%u]",
                   (unsigned)ic.cpda, (unsigned)NVMEOF_TCP_PDU_ALIGN,
                   (unsigned)NVMEOF_TCPK_MAX_CPDA);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    c->cpda = ic.cpda;
    /* TP 8000 allows the receiver to pad a PDU up to its alignment.  Ours is 4
     * and every capsule this file builds is already a multiple of 4, so the
     * trailing-pad read below is normally zero bytes - but it is still read,
     * because a peer that pads and a host that does not read the pad are a
     * permanently desynchronised stream. */
    pad = nvmeofTcpPduTotal(NVMEOF_TCP_ICREQ_LEN, 0, c->cpda) - NVMEOF_TCP_ICREQ_LEN;
    if (pad != 0) {
        st = NvtcpkRecv(c, NVMEOF_TCP_ICREQ_LEN, pad);
        if (!NT_SUCCESS(st)) {
            NvtcpkLogF("handshake: ICResp padding (%u bytes) not received, 0x%08X", pad, st);
            return st;
        }
    }

    NvtcpkLogF("handshake OK: peer pfv=%u cpda=%u dgst=%u maxr2t=%u",
               (unsigned)ic.pfv, (unsigned)ic.cpda, (unsigned)ic.dgst,
               (unsigned)ic.maxr2t);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Reconnect: the peer closed the connection, not this file's buffers        */
/*                                                                           */
/*  A transport that treats "the peer went away" as a permanently broken      */
/*  connection serves one diskpart run and then answers STATUS_DEVICE_NOT_READY */
/*  until the machine reboots.  The socket is gone, but the caller's handle,   */
/*  the namespace geometry and the buffers are all still valid, so the repair  */
/*  is a new socket, a new bind/connect/handshake, and then the work goes on.  */
/*  This is not a retry of an ambiguous operation: the only caller that ever   */
/*  resends (NvtcpkTransaction) does so when the capsule send completed with   */
/*  Information = 0, which means the command never reached the peer.          */
/* ------------------------------------------------------------------------- */
static NTSTATUS NvtcpkReconnect(PNVMEOF_TCPK_CONN c, const char* why)
{
    NTSTATUS st;

    if (c->poisoned) {
        /* A timed-out WSK request still owns its IRP, its buffer and its socket:
         * that connection must never be touched again, not even to replace it. */
        NvtcpkLogF("!! reconnect (%s): connection %p was poisoned by a timeout; not touching it",
                   why, c);
        return STATUS_DEVICE_NOT_READY;
    }
    InterlockedIncrement(&g_reconnects);
    InterlockedIncrement(&c->reconnects);
    NvtcpkLogF("=== reconnect (%s): rebuilding connection %p to %u.%u.%u.%u:%u, old socket=%p ===",
               why, c,
               (unsigned)(c->remote.sin_addr.S_un.S_addr & 0xFF),
               (unsigned)((c->remote.sin_addr.S_un.S_addr >> 8) & 0xFF),
               (unsigned)((c->remote.sin_addr.S_un.S_addr >> 16) & 0xFF),
               (unsigned)((c->remote.sin_addr.S_un.S_addr >> 24) & 0xFF),
               (unsigned)RtlUshortByteSwap(c->remote.sin_port), c->socket);
    NvtcpkLogFlush();

    /* The old socket is dead, but it is still an object WSK owns; closing it
     * first is what keeps the socket count from growing by one per reconnect. */
    st = NvtcpkSocketClose(c);
    if (c->poisoned) {
        return STATUS_DEVICE_NOT_READY;    /* the close itself timed out: hands off */
    }
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("!! reconnect: WskCloseSocket on the dead socket returned 0x%08X %s;"
                   " abandoning that socket object and building a new one",
                   (unsigned)st, NvtcpkStatusName(st));
        c->socket = NULL;                  /* never touch the old handle again */
    }

    st = NvtcpkSocketCreate(c, (PWSK_CLIENT)c->wskClient);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("!! reconnect: WskSocket = 0x%08X %s", (unsigned)st, NvtcpkStatusName(st));
        c->broken = TRUE;
        return st;
    }
    st = NvtcpkBind(c);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("reconnect: WskBind(wildcard) = 0x%08X (continuing to connect unbound)", st);
    }
    st = NvtcpkConnect(c);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("!! reconnect: WskConnect = 0x%08X %s", (unsigned)st, NvtcpkStatusName(st));
        c->broken = TRUE;
        return st;
    }
    st = NvtcpkHandshake(c);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("!! reconnect: handshake failed 0x%08X %s", (unsigned)st, NvtcpkStatusName(st));
        c->broken = TRUE;
        return st;
    }

    c->needReconnect = FALSE;
    c->broken = FALSE;                     /* a fresh stream is in step again */
    NvtcpkLogF("reconnect: connection %p is up again (%u reconnects on this connection)",
               c, (unsigned)c->reconnects);
    NvtcpkLogFlush();
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Public: open                                                             */
/* ------------------------------------------------------------------------- */

NTSTATUS NvtcpOpen(_In_ WSK_REGISTRATION* reg, _In_ WSK_PROVIDER_NPI* provider,
                   _In_ ULONG ipv4NetworkOrder, _In_ USHORT port,
                   _Out_ PVOID* conn)
{
    PNVMEOF_TCPK_CONN c;
    NTSTATUS st;
    NTSTATUS st2;
    KIRQL irql;

    NvtcpkGlobalsInit();

    if (conn == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *conn = NULL;
    if (reg == NULL || provider == NULL || provider->Client == NULL ||
        provider->Dispatch == NULL || provider->Dispatch->WskSocket == NULL) {
        NvtcpkLogF("NvtcpOpen: bad WSK registration/provider NPI (reg=%p provider=%p)",
                   reg, provider);
        NvtcpkLogFlush();
        return STATUS_INVALID_PARAMETER;
    }

    NvtcpkLogF("=== NvtcpOpen: NVMe/TCP connect to %u.%u.%u.%u:%u ===",
               (unsigned)((ipv4NetworkOrder >> 0) & 0xFF),
               (unsigned)((ipv4NetworkOrder >> 8) & 0xFF),
               (unsigned)((ipv4NetworkOrder >> 16) & 0xFF),
               (unsigned)((ipv4NetworkOrder >> 24) & 0xFF),
               (unsigned)port);
    NvtcpkLogFlush();

    c = (PNVMEOF_TCPK_CONN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), NVMEOF_TCPK_TAG);
    if (c == NULL) {
        NvtcpkLogF("NvtcpOpen: no non-paged pool for the connection");
        NvtcpkLogFlush();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(c, sizeof(*c));
    c->magic = NVMEOF_TCPK_MAGIC;
    c->refs = 1;                 /* the reference the list will own */
    c->cidNext = 1;
    c->cpda = (UCHAR)NVMEOF_TCP_PDU_ALIGN;
    c->providerDispatch = provider->Dispatch;
    /* Kept so a reconnect can create a socket without the caller's NPI: the
     * client half of the captured NPI lives as long as the registration. */
    c->wskClient = provider->Client;
    c->remote.sin_family = AF_INET;
    /* `port` is host order (NVMEOF_TCP_DEFAULT_PORT is 4420, not 0x4211) and the
     * address is already network order, so only the port is swapped. */
    c->remote.sin_port = RtlUshortByteSwap(port);
    c->remote.sin_addr.S_un.S_addr = ipv4NetworkOrder;

    c->pduBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, NVMEOF_TCPK_BUF_BYTES, NVMEOF_TCPK_TAG);
    c->rspBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, NVMEOF_TCPK_BUF_BYTES, NVMEOF_TCPK_TAG);
    if (c->pduBuf == NULL || c->rspBuf == NULL) {
        NvtcpkLogF("NvtcpOpen: no non-paged pool for the PDU buffers");
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto fail_free;
    }
    RtlZeroMemory(c->pduBuf, NVMEOF_TCPK_BUF_BYTES);
    RtlZeroMemory(c->rspBuf, NVMEOF_TCPK_BUF_BYTES);

    /* Both buffers are non-paged pool, so MmBuildMdlForNonPagedPool is the
     * documented way to describe them and the pages are already resident: no
     * MmProbeAndLockPages and no __try needed, which also matters because this
     * code can run at DISPATCH_LEVEL where probing is not allowed. */
    c->pduMdl = IoAllocateMdl(c->pduBuf, NVMEOF_TCPK_BUF_BYTES, FALSE, FALSE, NULL);
    c->rspMdl = IoAllocateMdl(c->rspBuf, NVMEOF_TCPK_BUF_BYTES, FALSE, FALSE, NULL);
    if (c->pduMdl == NULL || c->rspMdl == NULL) {
        NvtcpkLogF("NvtcpOpen: IoAllocateMdl failed");
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto fail_free;
    }
    MmBuildMdlForNonPagedPool(c->pduMdl);
    MmBuildMdlForNonPagedPool(c->rspMdl);

    st = NvtcpkSocketCreate(c, provider->Client);
    NvtcpkLogF("WskSocket = 0x%08X socket=%p", st, c->socket);
    NvtcpkLogFlush();
    if (!NT_SUCCESS(st)) {
        goto fail_close;
    }

    /* Bind is best effort; a failure here is logged and connect is tried anyway. */
    st = NvtcpkBind(c);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("WskBind(wildcard) = 0x%08X (continuing to connect unbound)", st);
    }
    NvtcpkLogFlush();

    st = NvtcpkConnect(c);
    NvtcpkLogF("WskConnect = 0x%08X (0xC0000236 = connection refused, 0xC000023C = unreachable)",
               st);
    NvtcpkLogFlush();
    if (!NT_SUCCESS(st)) {
        goto fail_close;
    }

    st = NvtcpkHandshake(c);
    NvtcpkLogFlush();
    if (!NT_SUCCESS(st)) {
        goto fail_close;
    }

    /* Publish only now: from here on NvtcpClose can find it, and the reference
     * taken at allocation time becomes the list's. */
    KeAcquireSpinLock(&g_connLock, &irql);
    InsertTailList(&g_connList, &c->link);
    c->linked = TRUE;
    KeReleaseSpinLock(&g_connLock, irql);

    InterlockedIncrement(&g_openCount);
    NvtcpkTraceValue(L"nvtcpk_opens", (ULONG)InterlockedCompareExchange(&g_openCount, 0, 0));
    NvtcpkLogF("NvtcpOpen: connection %p is open (cpda=%u, one command at a time)", c, (unsigned)c->cpda);
    NvtcpkLogFlush();

    *conn = c;
    return STATUS_SUCCESS;

fail_close:
    /* A failed bring-up must not leave a socket behind: that is exactly the
     * dangling state that cost this project delayed bugchecks. */
    if (c->poisoned) {
        NvtcpkLogF("NvtcpOpen failed 0x%08X AND the connection is poisoned, so its socket and"
                   " buffers are leaked instead of freed", st);
        c->immortal = TRUE;
        NvtcpkLogFlush();
        return st;
    }
    st2 = NvtcpkSocketClose(c);
    if (c->poisoned) {
        /* The close itself timed out, so the socket is still owned by WSK and the
         * buffers describe pages it may still write into. */
        c->immortal = TRUE;
        NvtcpkLogF("!! NvtcpOpen failed 0x%08X and closing the socket timed out; the connection"
                   " is leaked rather than freed", st);
        NvtcpkLogFlush();
        return st;
    }
    if (!NT_SUCCESS(st2)) {
        /* No operation was ever left pending, so the provider cannot be holding
         * these MDLs; the buffers are released.  The socket object itself is not
         * released though, and that is said out loud rather than glossed over. */
        NvtcpkLogF("!! NvtcpOpen failed 0x%08X and WskCloseSocket returned 0x%08X; the socket"
                   " object is leaked, the buffers are released", st, st2);
    } else {
        NvtcpkLogF("NvtcpOpen failed 0x%08X; socket closed and everything released", st);
    }
    NvtcpkLogFlush();
fail_free:
    NvtcpkFreeBuffers(c);
    c->magic = 0;
    ExFreePoolWithTag(c, NVMEOF_TCPK_TAG);
    return st;
}

/* ------------------------------------------------------------------------- */
/*  Teardown: the only place that releases a socket                          */
/*                                                                           */
/*  Order matters and is the reverse of creation: socket, then the MDLs, then  */
/*  the buffers, then the object.  A WSK_BUF that outlives its MDL is a        */
/*  provider writing into freed pool.                                         */
/* ------------------------------------------------------------------------- */

static void NvtcpkFreeBuffers(PNVMEOF_TCPK_CONN c)
{
    if (c->pduMdl != NULL) { IoFreeMdl(c->pduMdl); c->pduMdl = NULL; }
    if (c->rspMdl != NULL) { IoFreeMdl(c->rspMdl); c->rspMdl = NULL; }
    if (c->pduBuf != NULL) { ExFreePoolWithTag(c->pduBuf, NVMEOF_TCPK_TAG); c->pduBuf = NULL; }
    if (c->rspBuf != NULL) { ExFreePoolWithTag(c->rspBuf, NVMEOF_TCPK_TAG); c->rspBuf = NULL; }
}

static void NvtcpkDestroyConn(PNVMEOF_TCPK_CONN c)
{
    if (c == NULL) {
        return;
    }
    if (c->immortal) {
        /* Never freed on purpose: either a WSK request that timed out still owns
         * the IRP whose completion routine points into this object, or closing
         * the socket failed.  Freeing it would be the use-after-free that turns
         * a leaked socket into a bugcheck. */
        NvtcpkLogF("!! connection %p is immortal (leaked on purpose); it is NOT freed", c);
        NvtcpkLogFlush();
        return;
    }
    NvtcpkFreeBuffers(c);
    c->magic = 0;
    ExFreePoolWithTag(c, NVMEOF_TCPK_TAG);
}

static NTSTATUS NvtcpkTeardown(PNVMEOF_TCPK_CONN c)
{
    NTSTATUS st = NvtcpkSocketClose(c);

    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("!! teardown: WskCloseSocket = 0x%08X, so the socket may still exist and its"
                   " MDLs and buffers are left allocated on purpose", st);
        return st;
    }
    NvtcpkFreeBuffers(c);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Public: close                                                            */
/* ------------------------------------------------------------------------- */

NTSTATUS NvtcpClose(_In_ PVOID conn)
{
    PNVMEOF_TCPK_CONN c;
    NTSTATUS st;
    KIRQL irql;

    NvtcpkGlobalsInit();
    if (conn == NULL) {
        NvtcpkLogF("NvtcpClose(NULL): nothing to do");
        return STATUS_INVALID_PARAMETER;
    }

    c = NvtcpkLookupRef(conn);
    if (c == NULL) {
        /* Double close, or a pointer that was never an NvtcpOpen result.  It is
         * deliberately not an error: "safe to call twice" means the second call
         * has nothing to do, and a caller that has to special-case it is a caller
         * that will get it wrong. */
        NvtcpkLogF("NvtcpClose: %p is not an open connection (already closed, or never opened)"
                   "; nothing to do", conn);
        NvtcpkLogFlush();
        return STATUS_SUCCESS;
    }

    /* One closer at a time, and never while a command is inside the socket. */
    if (InterlockedCompareExchange(&c->busy, 1, 0) != 0) {
        NvtcpkLogF("NvtcpClose: a command is still running on connection %p; refusing to close it"
                   " (it is left open so nothing is freed under a live request)", c);
        NvtcpkLogFlush();
        NvtcpkDropRef(c);
        return STATUS_DEVICE_BUSY;
    }

    /* Unlink first: from here no new caller can find it, and the busy flag above
     * guarantees nobody is inside it. */
    KeAcquireSpinLock(&g_connLock, &irql);
    if (c->linked) {
        RemoveEntryList(&c->link);
        c->linked = FALSE;
    }
    KeReleaseSpinLock(&g_connLock, irql);

    if (c->poisoned) {
        /* A timed-out request still owns its IRP and buffer.  wsk.h forbids
         * WskCloseSocket while a WSK call has not returned control, so the whole
         * connection is abandoned here - logged, counted, and never touched again. */
        c->immortal = TRUE;
        NvtcpkLogF("!! NvtcpClose: connection %p was poisoned by a timeout; its socket and"
                   " buffers are deliberately leaked (%u bytes held). Reboot or let the peer"
                   " time out: do not free it", c, (unsigned)(2 * NVMEOF_TCPK_BUF_BYTES));
        NvtcpkLogFlush();
        NvtcpkTraceValue(L"nvtcpk_poisoned", (ULONG)InterlockedCompareExchange(&g_poisonCount, 0, 0));
        NvtcpkDropRef(c);
        return STATUS_SUCCESS;
    }

    st = NvtcpkTeardown(c);
    if (!NT_SUCCESS(st)) {
        c->immortal = TRUE;
        NvtcpkLogF("!! NvtcpClose: teardown of %p failed 0x%08X; the connection is leaked rather"
                   " than freed so nothing can be released twice", c, st);
        NvtcpkLogFlush();
        NvtcpkDropRef(c);
        return st;
    }

    NvtcpkLogF("NvtcpClose: connection %p closed cleanly (%u commands, %u bytes in, %u bytes out)",
               c, c->commands, c->bytesIn, c->bytesOut);
    NvtcpkTraceValue(L"nvtcpk_closes", 1);
    NvtcpkLogFlush();

    /* Two references go: the list's, which this call just removed, and the one
     * the lookup above took.  The last reference frees the object, so no other
     * thread can be left holding a pointer into it. */
    NvtcpkDropRef(c);
    NvtcpkDropRef(c);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  One command, with in-capsule data                                        */
/*                                                                           */
/*  Wire shape, exactly what src/nvmeof_tcp.c's hostCmd sends and its target */
/*  accepts, plus the two robustness points TP 8000 asks for:                 */
/*                                                                           */
/*    CapsuleCmd PDU : 8-byte header (type 0x04, hlen 72, pdo = data offset,  */
/*                     plen = data length) | 64-byte SQE | [padding] | data   */
/*    CapsuleResp PDU: 8-byte header | 16-byte CQE | [padding] | data         */
/*                                                                           */
/*  The data offset comes from the PDU's own pdo field rather than being      */
/*  assumed equal to hlen, and the trailing pad up to the peer's cpda is      */
/*  consumed, because a stream that is one short read out of step stays out   */
/*  of step for every command that follows.                                   */
/* ------------------------------------------------------------------------- */

/* Build the whole CapsuleCmd PDU in c->pduBuf: header, SQE, padding to the
 * peer's data offset, then the in-capsule payload.  Split out of the transaction
 * because a reconnect has to build it twice: the ICReq of the new connection
 * handshake is written into the same pduBuf and would otherwise overwrite the
 * capsule that still has to be sent. */
static void NvtcpkBuildCapsule(PNVMEOF_TCPK_CONN c, const UCHAR* sqe, ULONG pdo,
                               PVOID inData, ULONG inBytes)
{
    (void)nvmeofTcpBuildCapsuleCmd((PUCHAR)c->pduBuf, sqe, inBytes, pdo);
    if (pdo > NVMEOF_TCP_CCMD_HDR_LEN) {
        RtlZeroMemory((PUCHAR)c->pduBuf + NVMEOF_TCP_CCMD_HDR_LEN, pdo - NVMEOF_TCP_CCMD_HDR_LEN);
    }
    if (inBytes != 0 && inData != NULL) {
        RtlCopyMemory((PUCHAR)c->pduBuf + pdo, inData, inBytes);
    }
}

/* Push a Write's payload as H2CData PDUs.  The capsule that precedes them carries
 * plen = 0, and the target reassembles by datao and completes only after it has
 * every byte.  One PDU per 4096 bytes fits the existing send buffer, so nothing
 * new is allocated and the per-PDU padding comes from the negotiated cpda. */
static NTSTATUS NvtcpkSendDataPdus(PNVMEOF_TCPK_CONN c, USHORT cid, PVOID data, ULONG bytes)
{
    ULONG off = 0;
    PUCHAR buf = (PUCHAR)c->pduBuf;

    while (off < bytes) {
        NVMEOF_TCP_DATA d;
        ULONG chunk = bytes - off;
        ULONG used, total;
        NTSTATUS st;

        if (chunk > NVMEOF_TCPK_DATA_PDU_MAX) { chunk = NVMEOF_TCPK_DATA_PDU_MAX; }
        RtlZeroMemory(&d, sizeof(d));
        d.cccid = cid;
        d.ttag = 0;
        d.datao = off;
        d.datal = chunk;
        nvmeofTcpBuildData(buf, NVMEOF_TCP_H2CDATA,
                           (off + chunk == bytes) ? NVMEOF_TCP_F_LAST : 0, &d);
        RtlCopyMemory(buf + NVMEOF_TCP_DATA_HDR_LEN, (PUCHAR)data + off, chunk);
        used  = NVMEOF_TCP_DATA_HDR_LEN + chunk;
        total = nvmeofTcpPduTotal(used, 0, c->cpda);
        if (total > NVMEOF_TCPK_BUF_BYTES) {
            return STATUS_INVALID_BUFFER_SIZE;
        }
        if (total > used) {
            RtlZeroMemory(buf + used, total - used);
        }
        st = NvtcpkSend(c, total);
        if (!NT_SUCCESS(st)) {
            NvtcpkLogF("command cid=%u: H2CData at offset %u/%u failed 0x%08X %s",
                       (unsigned)cid, off, bytes, (unsigned)st, NvtcpkStatusName(st));
            return st;
        }
        off += chunk;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NvtcpkTransaction(PNVMEOF_TCPK_CONN c, UINT8 opcode, UINT32 nsid,
                                  UINT32 cdw10, UINT32 cdw12,
                                  PVOID inData, ULONG inBytes,
                                  PVOID outData, ULONG outBytes,
                                  UINT8* statusOut, ULONG* dataLenOut)
{
    NTSTATUS st;
    NVMEOF_TCP_HDR hdr;
    UCHAR sqe[NVMEOF_TCP_SQE_LEN];
    ULONG pdo, total, plen = 0, pdus;
    ULONG capsuleBytes;
    ULONG dataSeen = 0;
    BOOLEAN expectTail = FALSE;
    USHORT cid;
    BOOLEAN gotResponse = FALSE;
    BOOLEAN bigWrite;

    if (dataLenOut != NULL) {
        *dataLenOut = 0;
    }

    /* Anything over one capsule payload travels as data PDUs: 0 here means the
     * capsule announces no in-line data and an H2CData train follows it. */
    bigWrite = (inBytes > NVMEOF_TCPK_MAX_DATA) ? TRUE : FALSE;
    capsuleBytes = bigWrite ? 0 : inBytes;

    cid = c->cidNext++;
    NvtcpkBuildSqe(sqe, opcode, cid, nsid, cdw10, cdw12);

    /* In-capsule data starts after the SQE, at the peer's alignment.  With the
     * cpda every implementation in practice negotiates (4) this is 72 and the
     * PDU is byte-identical to the user-mode initiator's. */
    pdo = nvmeofTcpPduTotal(NVMEOF_TCP_CCMD_HDR_LEN, 0, c->cpda);
    if (pdo + capsuleBytes > NVMEOF_TCPK_BUF_BYTES) {
        NvtcpkLogF("command 0x%02X: capsule %u + %u bytes does not fit the PDU buffer",
                   (unsigned)opcode, (unsigned)pdo, (unsigned)capsuleBytes);
        return STATUS_INVALID_BUFFER_SIZE;
    }
    NvtcpkBuildCapsule(c, sqe, pdo, inData, capsuleBytes);
    total = pdo + capsuleBytes;

    {   /* TEMPORARY: remember this command for the failure dump. */
        NVMEOF_TCPK_DIAG* dg = &g_diagRing[g_diagAt & 7u];
        dg->cid      = cid;
        dg->opcode   = opcode;
        dg->inBytes  = inBytes;
        dg->outBytes = outBytes;
        dg->total    = total;
        dg->mdlBytes = (c->pduMdl != NULL) ? MmGetMdlByteCount(c->pduMdl) : 0;
        dg->callSt   = 0;
        dg->irpSt    = 0;
        g_diagCur    = dg;
        g_diagAt++;
        g_diagSeen++;
    }

    st = NvtcpkSend(c, total);
    if (!NT_SUCCESS(st) && NvtcpkIsSocketLoss(st) && !c->poisoned) {
        /* The peer closed the connection and this capsule never left (the send
         * completed with Information = 0), so the command has not run and may be
         * sent again.  One reconnect, one resend: no loop, no backoff, and no
         * second attempt if the first one fails. */
        NvtcpkLogF("command 0x%02X cid=%u: the capsule send hit a dead connection (0x%08X %s);"
                   " reconnecting and resending it", (unsigned)opcode, (unsigned)cid,
                   (unsigned)st, NvtcpkStatusName(st));
        st = NvtcpkReconnect(c, "dead connection before a capsule");
        if (NT_SUCCESS(st)) {
            /* The new handshake may have negotiated a different capsule data offset,
             * so pdo and total are recomputed rather than carried over. */
            pdo = nvmeofTcpPduTotal(NVMEOF_TCP_CCMD_HDR_LEN, 0, c->cpda);
            total = pdo + capsuleBytes;
            if (pdo + capsuleBytes > NVMEOF_TCPK_BUF_BYTES) {
                st = STATUS_INVALID_BUFFER_SIZE;
            } else {
                NvtcpkBuildCapsule(c, sqe, pdo, inData, capsuleBytes);
                st = NvtcpkSend(c, total);
            }
            if (!NT_SUCCESS(st)) {
                NvtcpkLogF("command 0x%02X cid=%u: resend after the reconnect also failed 0x%08X %s",
                           (unsigned)opcode, (unsigned)cid, (unsigned)st, NvtcpkStatusName(st));
            }
        }
    }
    if (NT_SUCCESS(st) && bigWrite) {
        st = NvtcpkSendDataPdus(c, cid, inData, inBytes);
        if (!NT_SUCCESS(st)) {
            /* The capsule is already on the wire and the target is waiting for the
             * rest of the payload, so this stream can never be back in step.  A
             * reconnect (which replaces the socket) is the only repair. */
            c->broken = TRUE;
        }
    }
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("command 0x%02X cid=%u: send of the capsule failed 0x%08X %s",
                   (unsigned)opcode, (unsigned)cid, (unsigned)st, NvtcpkStatusName(st));
        NvtcpkDiagDump(c, "send failed");
        return st;
    }

    for (pdus = 0; pdus < NVMEOF_TCPK_MAX_PDUS_PER_CMD && !gotResponse; pdus++) {
        ULONG hlen, body, dataOff, pad;

        st = NvtcpkRecv(c, 0, NVMEOF_TCP_HDR_LEN);
        if (!NT_SUCCESS(st)) {
            NvtcpkLogF("command 0x%02X cid=%u: no response header, 0x%08X",
                       (unsigned)opcode, (unsigned)cid, st);
            return st;
        }
        nvmeofTcpReadHdr((const uint8_t*)c->rspBuf, &hdr);
        if (g_rspSeen < 8) {
            NvtcpkLogF("  resp #%u cid=%u type=0x%02X flags=0x%02X hlen=%u pdo=%u plen=%u",
                       g_rspSeen, (unsigned)cid, (unsigned)hdr.type, (unsigned)hdr.flags,
                       (unsigned)hdr.hlen, (unsigned)hdr.pdo, (unsigned)hdr.plen);
        }
        g_rspSeen++;
        hlen = hdr.hlen;
        if (hlen < NVMEOF_TCP_HDR_LEN || hlen > NVMEOF_TCPK_BUF_BYTES) {
            c->broken = TRUE;
            NvtcpkLogF("command 0x%02X: PDU type 0x%02X announces hlen=%u, which cannot be right",
                       (unsigned)opcode, (unsigned)hdr.type, hlen);
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
        if ((hdr.flags & (NVMEOF_TCP_F_HDGST | NVMEOF_TCP_F_DDGST)) != 0) {
            c->broken = TRUE;
            NvtcpkLogF("command 0x%02X: PDU type 0x%02X carries digests, which this transport"
                       " never negotiated", (unsigned)opcode, (unsigned)hdr.type);
            return STATUS_NOT_SUPPORTED;
        }
        body = hlen - NVMEOF_TCP_HDR_LEN;
        if (body != 0) {
            st = NvtcpkRecv(c, NVMEOF_TCP_HDR_LEN, body);
            if (!NT_SUCCESS(st)) {
                NvtcpkLogF("command 0x%02X: PDU type 0x%02X body truncated, 0x%08X",
                           (unsigned)opcode, (unsigned)hdr.type, st);
                return st;
            }
        }

        if (hdr.type == NVMEOF_TCP_CAPSULERESP) {
            if (hlen < NVMEOF_TCP_CRSP_HDR_LEN) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: CapsuleResp hlen=%u has no room for a completion",
                           (unsigned)opcode, hlen);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            /* The completion lives at rspBuf + 8; the data, if any, at the PDU's
             * own data offset. */
            dataOff = ((ULONG)hdr.pdo >= hlen) ? (ULONG)hdr.pdo : hlen;
            if (dataOff > hlen + 64) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: CapsuleResp pdo=%u is %u bytes past hlen=%u",
                           (unsigned)opcode, (unsigned)hdr.pdo, dataOff - hlen, hlen);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            if (dataOff > hlen) {
                st = NvtcpkRecv(c, hlen, dataOff - hlen);
                if (!NT_SUCCESS(st)) {
                    return st;
                }
            }
            plen = hdr.plen;
            if (plen > NVMEOF_TCPK_MAX_DATA || dataOff + plen > NVMEOF_TCPK_BUF_BYTES) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: CapsuleResp announces plen=%u at offset %u",
                           (unsigned)opcode, plen, dataOff);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            if (plen != 0) {
                st = NvtcpkRecv(c, dataOff, plen);
                if (!NT_SUCCESS(st)) {
                    NvtcpkLogF("command 0x%02X: response data truncated, 0x%08X",
                               (unsigned)opcode, st);
                    return st;
                }
                pad = nvmeofTcpPduTotal(dataOff + plen, 0, c->cpda) - (dataOff + plen);
                if (pad != 0) {
                    st = NvtcpkRecv(c, dataOff + plen, pad);
                    if (!NT_SUCCESS(st)) {
                        return st;
                    }
                }
                if (outData == NULL) {
                    /* A completion that carries data the caller did not ask for:
                     * the bytes are consumed (the stream stays in step) and said
                     * so, rather than being silently dropped. */
                    NvtcpkLogF("command 0x%02X: %u bytes arrived but the caller supplied no"
                               " buffer; consumed and dropped", (unsigned)opcode, plen);
                } else if (plen > outBytes) {
                    c->broken = TRUE;
                    NvtcpkLogF("command 0x%02X: response carries %u bytes, caller's buffer is %u",
                               (unsigned)opcode, plen, outBytes);
                    return STATUS_BUFFER_TOO_SMALL;
                } else {
                    RtlCopyMemory(outData, (PUCHAR)c->rspBuf + dataOff, plen);
                }
                if (dataLenOut != NULL) {
                    *dataLenOut = plen;
                }
            }
            *statusOut = NvtcpkCqeStatus((const UCHAR*)c->rspBuf + NVMEOF_TCP_HDR_LEN);
            if (NvtcpkCqeCid((const UCHAR*)c->rspBuf + NVMEOF_TCP_HDR_LEN) != cid) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: completion is for cid=%u, not the cid=%u this command"
                           " was given - the queue is out of step",
                           (unsigned)opcode,
                           (unsigned)NvtcpkCqeCid((const UCHAR*)c->rspBuf + NVMEOF_TCP_HDR_LEN),
                           (unsigned)cid);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            gotResponse = TRUE;
            if (plen == 0 && outBytes > NVMEOF_TCPK_MAX_DATA && *statusOut == 0 &&
                dataSeen < outBytes) {
                /* The completion arrived before the data PDUs - the order this project's
                 * target used until 2026-10-18.  Stay in the loop and drain the C2HData
                 * tail: returning here leaves it in the socket, the next command reads it
                 * as its own response, and the stream is out of step for good.  That is
                 * exactly how the first 64 KiB read damaged Z:. */
                gotResponse = FALSE;
                expectTail = TRUE;
                NvtcpkLogF("command 0x%02X: completion before %u bytes of C2HData; draining"
                           " the tail", (unsigned)opcode, outBytes);
            }
        } else if (hdr.type == NVMEOF_TCP_C2HDATA) {
            /* A read answered out-of-capsule.  Legal, and supported: the payload
             * lands at its own datao inside the caller's buffer.  R2T-paced
             * writing is not implemented, so nothing here ever needs to ask. */
            NVMEOF_TCP_DATA d;

            if (hlen < NVMEOF_TCP_DATA_HDR_LEN) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: C2HData hlen=%u is too small", (unsigned)opcode, hlen);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            nvmeofTcpReadData((const uint8_t*)c->rspBuf, &d);
            if (d.datal > NVMEOF_TCPK_DATA_PDU_MAX || hlen + d.datal > NVMEOF_TCPK_BUF_BYTES) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: C2HData datal=%u", (unsigned)opcode, d.datal);
                return STATUS_INVALID_NETWORK_RESPONSE;
            }
            st = NvtcpkRecv(c, hlen, d.datal);
            if (!NT_SUCCESS(st)) {
                NvtcpkLogF("command 0x%02X: C2HData payload truncated, 0x%08X",
                           (unsigned)opcode, st);
                return st;
            }
            pad = nvmeofTcpPduTotal(hlen + d.datal, 0, c->cpda) - (hlen + d.datal);
            if (pad != 0) {
                st = NvtcpkRecv(c, hlen + d.datal, pad);
                if (!NT_SUCCESS(st)) {
                    return st;
                }
            }
            if (outData == NULL || (ULONGLONG)d.datao + d.datal > (ULONGLONG)outBytes) {
                c->broken = TRUE;
                NvtcpkLogF("command 0x%02X: C2HData range %u+%u does not fit the caller's %u-byte"
                           " buffer", (unsigned)opcode, d.datao, d.datal, outBytes);
                return STATUS_BUFFER_TOO_SMALL;
            }
            RtlCopyMemory((PUCHAR)outData + d.datao, (PUCHAR)c->rspBuf + hlen, d.datal);
            dataSeen += d.datal;
            if (dataLenOut != NULL && d.datao + d.datal > *dataLenOut) {
                *dataLenOut = d.datao + d.datal;
            }
            if (expectTail && dataSeen >= outBytes) {
                /* The completion already arrived; the data is complete now. */
                gotResponse = TRUE;
            }
        } else if (hdr.type == NVMEOF_TCP_R2T) {
            c->broken = TRUE;
            NvtcpkLogF("command 0x%02X: peer sent an R2T although the ICReq advertised maxr2t=0;"
                       " this transport only does in-capsule data", (unsigned)opcode);
            return STATUS_NOT_SUPPORTED;
        } else {
            c->broken = TRUE;
            NvtcpkLogF("command 0x%02X: unexpected PDU type 0x%02X", (unsigned)opcode,
                       (unsigned)hdr.type);
            return STATUS_INVALID_NETWORK_RESPONSE;
        }
    }

    if (!gotResponse) {
        c->broken = TRUE;
        NvtcpkLogF("command 0x%02X cid=%u: no CapsuleResp within %u PDUs",
                   (unsigned)opcode, (unsigned)cid, (unsigned)NVMEOF_TCPK_MAX_PDUS_PER_CMD);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    if (outData != NULL && outBytes > NVMEOF_TCPK_MAX_DATA && *statusOut == 0 &&
        dataSeen < outBytes) {
        c->broken = TRUE;
        NvtcpkLogF("command 0x%02X: only %u of %u bytes of C2HData arrived",
                   (unsigned)opcode, dataSeen, outBytes);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    c->commands++;
    return STATUS_SUCCESS;
}

static NTSTATUS NvtcpkCommandEx(PVOID conn, UINT8 opcode, UINT32 nsid, UINT32 cdw10,
                                UINT32 cdw12, PVOID inData, ULONG inBytes,
                                PVOID outData, ULONG outBytes, UINT8* statusOut,
                                ULONG* dataLenOut)
{
    PNVMEOF_TCPK_CONN c;
    NTSTATUS st;
    NTSTATUS rst;
    UCHAR localStatus = NVMEOF_TCPK_STATUS_UNKNOWN;

    if (statusOut == NULL) {
        statusOut = &localStatus;
    }
    *statusOut = NVMEOF_TCPK_STATUS_UNKNOWN;
    if (dataLenOut != NULL) {
        *dataLenOut = 0;
    }

    /* Up to one capsule payload rides in the capsule; more moves as C2HData/H2CData
     * PDUs.  R2T pacing is not implemented (the ICReq advertises maxr2t = 0). */
    if (inBytes > NVMEOF_TCPK_IO_MAX || outBytes > NVMEOF_TCPK_IO_MAX) {
        NvtcpkLogF("command 0x%02X: %u in / %u out exceeds the %u-byte per-command limit",
                   (unsigned)opcode, inBytes, outBytes, (unsigned)NVMEOF_TCPK_IO_MAX);
        return STATUS_INVALID_BUFFER_SIZE;
    }
    if ((inBytes != 0 && inData == NULL) || (outBytes != 0 && outData == NULL)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (inBytes != 0 && outBytes != 0) {
        /* No NVMe command in this transport's set moves data both ways at once. */
        NvtcpkLogF("command 0x%02X: both an input and an output buffer were given",
                   (unsigned)opcode);
        return STATUS_INVALID_PARAMETER;
    }

    NvtcpkGlobalsInit();
    c = NvtcpkLookupRef(conn);
    if (c == NULL) {
        NvtcpkLogF("command 0x%02X: %p is not an open connection", (unsigned)opcode, conn);
        return STATUS_INVALID_HANDLE;
    }
    if (c->poisoned) {
        NvtcpkLogF("command 0x%02X: connection %p was poisoned by a timeout; refusing",
                   (unsigned)opcode, c);
        NvtcpkDropRef(c);
        return STATUS_DEVICE_NOT_READY;
    }
    if (c->broken) {
        /* A protocol error left the byte stream out of step.  A TCP stream cannot be
         * resynchronised, so the only repair is a NEW socket - and the old code did
         * not do that: it refused every command until the machine rebooted, which is
         * the one-way latch this replaces.  Setting needReconnect routes this command
         * through NvtcpkReconnect, which closes the out-of-step socket, builds a new
         * one, redoes the handshake and clears broken.  If the rebuild fails, broken
         * stays set and the next command tries again. */
        NvtcpkLogF("command 0x%02X: connection %p is out of step after a protocol error;"
                   " rebuilding the connection instead of refusing until reboot",
                   (unsigned)opcode, c);
        c->needReconnect = TRUE;
    }
    if (InterlockedCompareExchange(&c->busy, 1, 0) != 0) {
        /* One command at a time per connection is the whole concurrency model:
         * the capsule buffer and the completion event are per connection, and
         * two commands sharing them is silent protocol corruption. */
        NvtcpkLogF("command 0x%02X: connection %p is busy with another command",
                   (unsigned)opcode, c);
        NvtcpkDropRef(c);
        return STATUS_DEVICE_BUSY;
    }

    if (c->needReconnect) {
        /* The socket was lost on an earlier command - the peer closed it, or a
         * receive died with the connection.  Re-establish it before this command
         * instead of refusing every command from here to the next reboot: the
         * caller's handle, the geometry and the buffers are all still valid, and
         * the fresh socket starts in step. */
        rst = NvtcpkReconnect(c, "socket lost on an earlier command");
        if (!NT_SUCCESS(rst)) {
            InterlockedExchange(&c->busy, 0);
            NvtcpkDropRef(c);
            NvtcpkLogFlush();
            return rst;
        }
    }

    st = NvtcpkTransaction(c, opcode, nsid, cdw10, cdw12, inData, inBytes,
                           outData, outBytes, statusOut, dataLenOut);

    InterlockedExchange(&c->busy, 0);
    NvtcpkDropRef(c);
    NvtcpkLogFlush();
    return st;
}

NTSTATUS NvtcpCommand(_In_ PVOID conn, _In_ UINT8 opcode, _In_ UINT32 nsid,
                      _In_ UINT32 cdw10, _In_ UINT32 cdw12,
                      _In_reads_bytes_opt_(inBytes) PVOID inData, _In_ ULONG inBytes,
                      _Out_writes_bytes_opt_(outBytes) PVOID outData, _In_ ULONG outBytes,
                      _Out_ UINT8* status)
{
    return NvtcpkCommandEx(conn, opcode, nsid, cdw10, cdw12, inData, inBytes,
                           outData, outBytes, status, NULL);
}

/* ------------------------------------------------------------------------- */
/*  Public: identify                                                         */
/* ------------------------------------------------------------------------- */

NTSTATUS NvtcpIdentify(_In_ PVOID conn, _In_ UINT8 cns, _In_ UINT32 nsid,
                       _Out_writes_bytes_(4096) PVOID out4096)
{
    NTSTATUS st;
    UCHAR nvmeStatus = NVMEOF_TCPK_STATUS_UNKNOWN;
    ULONG got = 0;

    if (out4096 == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Zeroed first: a short answer then looks like zeros in the unused half
     * rather than whatever the caller's buffer held before. */
    RtlZeroMemory(out4096, NVMEOF_TCPK_MAX_DATA);

    /* Identify: opcode 0x06, CNS in CDW10 bits 7:0, CNTID in CDW11 (zero = the
     * controller this connection is attached to).  Both the self-test and the
     * working user-mode initiator build it exactly this way. */
    st = NvtcpkCommandEx(conn, NVMEOF_TCPK_OPC_IDENTIFY, nsid, (UINT32)cns, 0,
                         NULL, 0, out4096, NVMEOF_TCPK_MAX_DATA, &nvmeStatus, &got);
    if (!NT_SUCCESS(st)) {
        NvtcpkLogF("Identify cns=%u nsid=%u: transport failed 0x%08X",
                   (unsigned)cns, nsid, st);
        return st;
    }
    if (nvmeStatus != 0) {
        NvtcpkLogF("Identify cns=%u nsid=%u: NVMe status 0x%02X", (unsigned)cns, nsid,
                   (unsigned)nvmeStatus);
        NvtcpkTraceValue(L"nvtcpk_identify_status", (ULONG)nvmeStatus);
        return STATUS_IO_DEVICE_ERROR;
    }
    if (got != NVMEOF_TCPK_MAX_DATA) {
        NvtcpkLogF("Identify cns=%u nsid=%u: %u bytes came back, expected %u",
                   (unsigned)cns, nsid, got, (unsigned)NVMEOF_TCPK_MAX_DATA);
        return STATUS_INVALID_NETWORK_RESPONSE;
    }
    NvtcpkLogF("Identify cns=%u nsid=%u: 4096 bytes, VID 0x%04X",
               (unsigned)cns, nsid,
               (unsigned)(((const UCHAR*)out4096)[0] | ((unsigned)((const UCHAR*)out4096)[1] << 8)));
    NvtcpkLogFlush();
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/*  Driver skeleton: entry, unload                                           */
/*                                                                           */
/*  DriverEntry opens nothing.  It does not call WskRegister, it does not      */
/*  capture a provider, and it does not touch the network: this file is a      */
/*  library that the miniport will call into, and its DriverEntry exists only  */
/*  so the object links into a complete driver and the log proves the build    */
/*  was loaded.  Signing and installing are deliberately not part of this step.*/
/* ------------------------------------------------------------------------- */

static VOID NvtcpkCloseAll(void)
{
    ULONG round;

    /* Bounded on purpose: a connection whose command is still in flight is
     * refused by NvtcpClose, and retrying it more than a few times cannot help. */
    for (round = 0; round < 4; round++) {
        PNVMEOF_TCPK_CONN batch[8];
        ULONG n = 0, i;
        KIRQL irql;
        PLIST_ENTRY e;

        KeAcquireSpinLock(&g_connLock, &irql);
        for (e = g_connList.Flink; e != &g_connList && n < 8; e = e->Flink) {
            PNVMEOF_TCPK_CONN p = CONTAINING_RECORD(e, NVMEOF_TCPK_CONN, link);
            if (p->immortal) {
                continue;
            }
            InterlockedIncrement(&p->refs);
            batch[n++] = p;
        }
        KeReleaseSpinLock(&g_connLock, irql);
        if (n == 0) {
            return;
        }
        for (i = 0; i < n; i++) {
            (void)NvtcpClose((PVOID)batch[i]);
            NvtcpkDropRef(batch[i]);
        }
    }
    NvtcpkLogF("!! close-all stopped after 4 rounds: a connection is still open or busy");
}

static VOID NvtcpkUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    NvtcpkLogF("=== nvmeof_tcpk unload: releasing every connection this file still owns ===");
    /* No registry I/O on unload: it wedged this machine once already.  The ring is
     * dropped with the driver; anything worth keeping was written during the run. */
    /* NvtcpkLogFlush(); */
    NvtcpkCloseAll();
    NvtcpkLogF("unload complete");
    /* No registry I/O on unload: it wedged this machine once already.  The ring is
     * dropped with the driver; anything worth keeping was written during the run. */
    /* NvtcpkLogFlush(); */
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = NvtcpkUnload;

    NvtcpkGlobalsInit();
    NvtcpkLogF("=== nvmeof_tcpk: NVMe/TCP transport client, loaded as a library (no socket,"
               " no adapter, no StorPort registration) ===");
    NvtcpkLogFlush();
    NvtcpkTraceValue(L"nvtcpk_loaded", 1);
    return STATUS_SUCCESS;
}


