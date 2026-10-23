// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
//
// ===========================================================================
//  nvmeofnd.sys - D1.2: the StorPort VIRTUAL MINIPORT now serves a REMOTE
//  NVMe/TCP namespace instead of a RAM buffer.
//
//  D1.1 (the previous revision of this file) presented a 48 MiB RAM LUN and
//  answered every SCSI command out of that buffer.  It was installed and used on
//  this machine, which proved the StorPort half of the design: Windows scanned
//  the adapter, sent INQUIRY / READ CAPACITY / READ / WRITE, partitioned the
//  disk, formatted it and put a drive letter on it.
//
//  D1.2 keeps every one of those SCSI answers and changes only WHERE THE BYTES
//  COME FROM.  READ and WRITE are translated into NVMe commands and carried to
//  a target over NVMe/TCP by nvmeof_tcpk.c (WSK), the transport whose IRP
//  completion pattern was validated by an actual load on this machine (see
//  src/driver/nvmeof_wsktest.c and evidence/wsktest_incident.txt).
//
//  ---------------------------------------------------------------------------
//  THE ONE STRUCTURAL CONSTRAINT: IRQL
//  ---------------------------------------------------------------------------
//  StorPort calls HwStartIo at DISPATCH_LEVEL.  NvtcpOpen blocks for about two
//  seconds when it fails and NvtcpCommand blocks until the target answers (30 s
//  response timeout).  Blocking at DISPATCH_LEVEL while StorPort holds its
//  spinlock is a machine that looks frozen, so THIS DRIVER NEVER CALLS THE
//  TRANSPORT FROM HwStartIo.  The chosen shape is:
//
//    HwFindAdapter  PASSIVE  declares the adapter shape (unchanged from D1.1)
//    HwInitialize   PASSIVE  WskCaptureProviderNPI (bounded), NvtcpOpen,
//                            NvtcpIdentify CNS=1 then CNS=0, then starts ONE
//                            PASSIVE system thread.  Because this is already
//                            PASSIVE_LEVEL, the connection is open BEFORE the
//                            LUN is announced, so Windows' first READ CAPACITY
//                            already sees the real remote geometry.  (The other
//                            allowed shape - open from a worker and fail I/O
//                            until it is up - was rejected: Windows probes
//                            READ CAPACITY immediately after the bus change
//                            notification, and a "not ready yet" answer there
//                            risks the disk being recorded as zero-sized.)
//    HwStartIo      DISPATCH enqueues the SRB into a fixed 64-slot in-driver
//                            queue and returns TRUE without completing it.
//    worker         PASSIVE  dequeues one SRB at a time, runs the SCSI command
//                            (transport calls included) and completes it with
//                            StorPortNotification(RequestComplete, ...).
//
//  So every transport call - and every registry write - happens at
//  PASSIVE_LEVEL, and HwStartIo does nothing but take a spinlock, copy 16 bytes
//  and set an event.  One command at a time is also what the transport demands:
//  NvtcpCommand refuses a second concurrent caller with STATUS_DEVICE_BUSY
//  rather than corrupting its shared capsule buffer.
//
//  ---------------------------------------------------------------------------
//  THE OTHER CONSTRAINT: THIS DRIVER IS LOADED ONCE AND NEVER UNLOADED
//  ---------------------------------------------------------------------------
//  driver:
//    - There is NO DriverUnload routine and none is added.  StorPort owns the
//      unload path; nothing here needs to run at unload.
//    - The engine (queue, event, spinlock, connection, identify buffer) lives in
//      non-paged pool and is deliberately NEVER freed, so no code path can ever
//      touch freed memory after the adapter goes away.  The worker thread parks
//      on its own event and touches nothing else while idle.
//    - DriverEntry's failure paths DO clean up (WskDeregister), because
//      DriverEntry runs in a normal system-thread context and never at unload.
//    - ScsiStopAdapter is NOT claimed in the supported-control-types list, so
//      StorPort never calls back into this driver while tearing the adapter
//      down.  Nothing to wedge, nothing to free twice.
//
//  Logging is incremental and goes to HKLM\Software\NVMeoFProbe exactly like
//  every other probe in this project, so a crash still leaves the trail on disk.
//  Registry I/O is PASSIVE_LEVEL only; every call site is guarded by an IRQL
//  check so it can never be attempted at DISPATCH_LEVEL.
// ===========================================================================

#include <ntddk.h>
#include <storport.h>
#include <stdarg.h>

// The transport's public contract, its NVMe opcodes / Identify CNS values and
// the WSK headers.  nvmeof_tcpk.h takes care of <ntddk.h> and <wsk.h>; including
// it here is what makes the prototypes below checked against the transport
// instead of hand-copied.
#include "nvmeof_tcpk.h"

// SCSI opcodes and status codes are defined here rather than pulled from scsi.h, because including
// both storport.h and scsi.h makes srb.h define its structures twice (measured: a wall of C2011
// "struct type redefinition").  The values are the standard ones.
#define NVMEOFND_OPC_TEST_UNIT_READY  0x00
#define NVMEOFND_OPC_REQUEST_SENSE    0x03
#define NVMEOFND_OPC_INQUIRY          0x12
#define NVMEOFND_OPC_READ_CAPACITY10  0x25
#define NVMEOFND_OPC_READ10           0x28
#define NVMEOFND_OPC_WRITE10          0x2A
#define NVMEOFND_OPC_SYNC_CACHE10     0x35
#define NVMEOFND_OPC_READ16           0x88
#define NVMEOFND_OPC_WRITE16          0x8A
#define NVMEOFND_OPC_READ6            0x08
#define NVMEOFND_OPC_WRITE6           0x0A
#define NVMEOFND_OPC_REPORT_LUNS      0xA0
#define NVMEOFND_OPC_MODE_SENSE6     0x1A
#define NVMEOFND_OPC_START_STOP_UNIT 0x1B
#define NVMEOFND_OPC_VERIFY10        0x2F
#define NVMEOFND_OPC_MODE_SENSE10    0x5A
#define NVMEOFND_OPC_SYNC_CACHE16    0x91
#define NVMEOFND_OPC_SERVICE_IN16    0x9E
#define NVMEOFND_STAT_GOOD            0x00
#define NVMEOFND_STAT_CHECK_COND      0x02
#define NVMEOFND_SENSE_ILLEGAL_REQ    0x05
#define NVMEOFND_SENSE_NOT_READY      0x02
#define NVMEOFND_SENSE_MEDIUM_ERROR   0x03

#define NVMEOFND_TAG          'dnfo'

// The block size is NOT taken from Identify: this target fills its LBAF table
// with an off-by-four (nvmeof_tcp.c writes d[130..133] where the spec puts
// LBAF0 at d[128..131]), so its LBADS byte reads back as 0.  NSZE is correct
// there and IS used; the block size is 512 because that is what the target's
// nsRead/nsWrite use, and the READ/WRITE path proves it (see the report).
#define NVMEOFND_BLOCK_BYTES  512u

// 127.0.0.1 in network byte order: the bytes in memory are 7F 00 00 01, so the
// 32-bit value read as a number is 0x0100007F.  Same convention as
// nvmeof_wsktest.c and nvmeofk.c; the port is HOST order (4421, not 0x4511).
#define NVMEOFND_TARGET_IPV4  0x0100007Fu
#define NVMEOFND_TARGET_PORT  4421u
#define NVMEOFND_NSID         1u

#define NVMEOFND_MAX_TARGETS  1
#define NVMEOFND_MAX_LUNS     1

// One NVMe command moves at most 4096 bytes in-capsule, so a transfer is split
// into 8-block (4 KiB) commands.  The target refuses anything larger with
// NVME_SC_DATA_XFER_ERR (nvmeof_tcp.c: "bytes > 4096").
#define NVMEOFND_CHUNK_BYTES  NVMEOF_TCPK_MAX_DATA
#define NVMEOFND_CHUNK_BLOCKS (NVMEOFND_CHUNK_BYTES / NVMEOFND_BLOCK_BYTES)

// Queue depth: StorPort is told 32 outstanding requests per LUN; the queue is
// twice that so a burst can never be refused in practice.
#define NVMEOFND_MAX_PENDING  64
#define NVMEOFND_MAX_QUEUES   8u
#define NVMEOFND_MAX_PIPELINE 8u

// The worker waits with a timeout rather than forever, so a stop request can
// never leave a thread parked on an event that nobody will signal.
#define NVMEOFND_WAIT_MS      250u

// WskCaptureProviderNPI: bounded.  WSK_INFINITE_WAIT is exactly what a driver
// must not use (see evidence/wsktest_incident.txt, which is why this project
// always bounds it).
#define NVMEOFND_CAPTURE_WAIT_MS 5000u

// SCSI opcodes come from scsi.h where it names them; the two 10-byte block
// opcodes are written as literals because this header spells them differently
// across WDK versions and a wrong name would simply fail to compile.
#define SCSIOP_READ10_          0x28
#define SCSIOP_WRITE10_         0x2A

// ---------------------------------------------------------------------------
//  Incremental logging.  A kernel driver's only output is the registry, and the
//  point of writing as we go is that a bugcheck still leaves the trail behind.
//  ZwCreateKey/ZwSetValueKey are PASSIVE_LEVEL only, so both writers return
//  immediately at any other IRQL: an unlogged line is survivable, a registry
//  call at DISPATCH_LEVEL is not.
// ---------------------------------------------------------------------------
#define NVMEOFND_REG_KEY  L"\\Registry\\Machine\\Software\\NVMeoFProbe"

static char   g_log[6144];
static size_t g_logLen = 0;
static ULONG  g_logDropped = 0;
static WCHAR  g_logW[6144 + 1];
static ULONGLONG g_startTime = 0;

static ULONG NvmeofndElapsedMs(void)
{
    ULONGLONG now;
    if (g_startTime == 0) {
        return 0;
    }
    now = KeQueryInterruptTime();          // 100 ns units
    return (ULONG)((now - g_startTime) / 10000ULL);
}

static VOID NvmeofndLogRaw(const char* s, size_t n)
{
    if (g_logLen + n + 3 >= sizeof(g_log)) {
        g_logDropped++;
        return;
    }
    RtlCopyMemory(g_log + g_logLen, s, n);
    g_logLen += n;
}

static VOID NvmeofndLogNum(unsigned long long v, int base, int width, int upper)
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
        tmp[i++] = '0';
    }
    while (i > 0) {
        NvmeofndLogRaw(&tmp[--i], 1);
    }
}

// Supported specifiers: %s %u %d %x %X %p %c %% - the same subset the other
// drivers in this project use, because the RtlStringCbVPrintf family pulls UCRT
// symbols that a driver cannot link.
static VOID NvmeofndLogF(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (const char* p = fmt; *p != 0; p++) {
        int width = 0, longs = 0;
        if (*p != '%') {
            NvmeofndLogRaw(p, 1);
            continue;
        }
        p++;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { longs++; p++; }
        switch (*p) {
        case 'u':
            NvmeofndLogNum((longs >= 2) ? va_arg(ap, unsigned long long)
                                        : (unsigned long long)va_arg(ap, unsigned int),
                           10, width, 0);
            break;
        case 'd': {
            int iv = va_arg(ap, int);
            if (iv < 0) { NvmeofndLogRaw("-", 1); iv = -iv; }
            NvmeofndLogNum((unsigned long long)iv, 10, width, 0);
            break;
        }
        case 'x':
            NvmeofndLogNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 0);
            break;
        case 'X':
            NvmeofndLogNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 1);
            break;
        case 'p':
            NvmeofndLogRaw("0x", 2);
            NvmeofndLogNum((unsigned long long)(ULONG_PTR)va_arg(ap, PVOID), 16, 16, 0);
            break;
        case 's': {
            const char* sv = va_arg(ap, const char*);
            if (sv == NULL) { sv = "(null)"; }
            while (*sv != 0) { NvmeofndLogRaw(sv, 1); sv++; }
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            NvmeofndLogRaw(&cv, 1);
            break;
        }
        case '%':
            NvmeofndLogRaw("%", 1);
            break;
        default:
            NvmeofndLogRaw("%", 1);
            NvmeofndLogRaw(p, 1);
            break;
        }
    }
    va_end(ap);
    NvmeofndLogRaw("\r\n", 2);
}

static VOID NvmeofndWriteValue(PCWSTR name, ULONG value)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&keyName, NVMEOFND_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    RtlInitUnicodeString(&valueName, name);
    ZwSetValueKey(key, &valueName, 0, REG_DWORD, &value, sizeof(value));
    ZwClose(key);
}

// Publish the text buffer as a well-formed UTF-16 REG_SZ (regedit, reg.exe and
// PowerShell all render it correctly; a raw ANSI REG_SZ does not).
static VOID NvmeofndLogFlush(void)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;
    ULONG i;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_logLen == 0) {
        return;
    }
    RtlInitUnicodeString(&keyName, NVMEOFND_REG_KEY);
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    for (i = 0; i < (ULONG)g_logLen; i++) {
        g_logW[i] = (WCHAR)(UCHAR)g_log[i];       // every byte the formatter emits is ASCII
    }
    g_logW[g_logLen] = 0;
    RtlInitUnicodeString(&valueName, L"nvmeofnd2_log");
    ZwSetValueKey(key, &valueName, 0, REG_SZ, g_logW, (ULONG)((g_logLen + 1) * sizeof(WCHAR)));
    if (g_logDropped != 0) {
        RtlInitUnicodeString(&valueName, L"nvmeofnd2_logdropped");
        ZwSetValueKey(key, &valueName, 0, REG_DWORD, &g_logDropped, sizeof(g_logDropped));
    }
    ZwClose(key);
}

// One DWORD per interesting fact, so a human does not have to grep the log for
// the single number that answers "how far did it get".
static VOID NvmeofndTrace(PCWSTR name, ULONG value)
{
    NvmeofndWriteValue(name, value);
}

// ---------------------------------------------------------------------------
//  The engine: everything the worker thread and HwStartIo share.
//
//  Allocated from non-paged pool ONCE and never freed.  That is the whole point
//  of the design: whatever happens to the adapter (stop, restart, surprise
//  removal), the worker can never find itself standing on freed memory.
// ---------------------------------------------------------------------------
typedef struct _NVMEOFND_SLOT {
    PSCSI_REQUEST_BLOCK Srb;      // the request to complete
    PVOID               Ext;      // its adapter extension, captured at enqueue
    ULONGLONG           Stamp;    // QPC at HwStartIo enqueue, for phase A timing
} NVMEOFND_SLOT;

typedef struct _NVMEOFND_ENGINE {
    KSPIN_LOCK      Lock;
    KEVENT          Wake;         // signalled by HwStartIo when a slot is pushed
    NVMEOFND_SLOT   Slot[NVMEOFND_MAX_PENDING];
    ULONG           Head;
    ULONG           Tail;
    ULONG           Count;
    volatile LONG   Stop;         // never set in practice; checked by the worker
    BOOLEAN         ThreadStarted;
    ULONG           Queues;       // connections/worker threads actually open (1..MAX)
    ULONG           PipeDepth;    // SRBs batched per wake (nvmeofnd_pipelinedepth, live)
    ULONG           DepthCheckedAt; // ms of the last live re-read of the depth
    PVOID           Conn[8];      // one NVTCP connection per queue
    PVOID           IdBuf;        // 4096-byte non-paged Identify buffer
    ULONG           Blocks;       // NSZE from Identify CNS=0
    ULONG           BlockBytes;
    ULONG           ChunkBytes;   // bytes per NVMe READ/WRITE (nvmeofnd_chunkkb, live)
    ULONG           ChunkCheckedAt; // ms of the last live re-read of the knob
    ULONG           CmdOk;        // NVMe commands answered with status 0
    ULONG           CmdNvmeErr;   // NVMe commands answered with a non-zero status
    ULONG           CmdTransportErr;
    ULONG           SrbsCompleted;
} NVMEOFND_ENGINE, *PNVMEOFND_ENGINE;

typedef struct _NVMEOFND_EXTENSION {
    PNVMEOFND_ENGINE Eng;              // never freed; NULL only if bring-up failed
    ULONG   Blocks;
    ULONG   BlockBytes;
    BOOLEAN Ready;
    ULONG   InitStatus;                // NTSTATUS of the last bring-up, for the log
    // SCSI I/O runs in the worker at PASSIVE_LEVEL, but the counters still live
    // in the extension: they are published from the WMI SRB path and every 128
    // completions, and keeping them per-adapter matches the rest of this project.
    ULONG   OpCount[256];
    ULONG   TotalSrbs;
    ULONG   Reads;
    ULONG   Writes;
    ULONG   IoBytes;
    ULONG   TransportErrors;
    ULONG   NotReadyAnswers;
    ULONG   ResetBusCount;
    ULONG   QueueFullCount;
} NVMEOFND_EXTENSION, *PNVMEOFND_EXTENSION;

// ---------------------------------------------------------------------------
//  Globals.  WSK registration belongs to the driver's lifetime, so it is taken
//  in DriverEntry and released only if DriverEntry fails: WskDeregister on a
//  registration that never succeeded is illegal, and a registration that is
//  still live when the image unloads is a callback into freed code.
// ---------------------------------------------------------------------------
static WSK_REGISTRATION    g_registration;
static WSK_CLIENT_NPI      g_wskClient;
static WSK_CLIENT_DISPATCH g_wskDispatch;
static WSK_PROVIDER_NPI    g_wskProvider;
static BOOLEAN             g_registered = FALSE;
static BOOLEAN             g_npiCaptured = FALSE;
static PNVMEOFND_ENGINE    g_engine = NULL;

static VOID NvmeofndProcessOne(PNVMEOFND_ENGINE Eng, PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb, PVOID Conn);
static NTSTATUS NvmeofndEngineAttach(PNVMEOFND_EXTENSION Ext);

// ---------------------------------------------------------------------------
//  Inquiry data: what a disk driver must answer before anything else works.
//  Vendor/product/revision are also what Get-Disk shows as FriendlyName, so
//  they name the remote nature of the LUN ("TCP REMOTE LUN") and carry a new
//  revision, which keeps this disk a NEW device identity rather than looking
//  like the D1.1 RAM LUN.
// ---------------------------------------------------------------------------
static const UCHAR g_Inquiry[36] = {
    0x00,           // peripheral qualifier 0, device type 0 = direct access block
    0x00,           // not removable
    0x05,           // SPC-3
    0x02,           // response data format 2
    0x1F,           // additional length = 31 (36 - 5)
    0x00, 0x00,     // reserved
    0x00,           // no linked commands
    'N','V','M','e','o','F','N','D',        // vendor  (8)
    'T','C','P',' ','R','E','M','O','T','E',' ','L','U','N',' ',' ',   // product (16)
    '0','0','0','2',                        // revision (4)
};

// Note the OR: NvmeofndSetSense marks the request with SRB_STATUS_AUTOSENSE_VALID
// so that StorPort copies the sense bytes up.  A plain assignment here would
// throw that flag away again (the D1.1 revision had exactly that bug: the sense
// buffer was filled but never advertised, which is why the host retried instead
// of reporting the error).
static VOID NvmeofndComplete(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb, UCHAR SrbStatus)
{
    Srb->SrbStatus = (UCHAR)(SrbStatus | (Srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID));
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}

static VOID NvmeofndSetSense(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb,
                             UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    UNREFERENCED_PARAMETER(DeviceExtension);

    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength >= 18) {
        PUCHAR s = (PUCHAR)Srb->SenseInfoBuffer;
        RtlZeroMemory(s, 18);
        s[0] = 0x70;            // current error, valid
        s[2] = Key;             // sense key
        s[7] = 10;              // additional sense length
        s[12] = Asc;
        s[13] = Ascq;
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
    Srb->ScsiStatus = NVMEOFND_STAT_CHECK_COND;
}

// ---------------------------------------------------------------------------
//  Little-endian readers.  The Identify payload is a byte image, so it is read
//  byte by byte instead of being cast to a struct with alignment assumptions.
// ---------------------------------------------------------------------------
static ULONG NvmeofndGet32(const UCHAR* p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static ULONGLONG NvmeofndGet64(const UCHAR* p)
{
    return (ULONGLONG)NvmeofndGet32(p) | ((ULONGLONG)NvmeofndGet32(p + 4) << 32);
}

// ---------------------------------------------------------------------------
//  Transport: one SCSI READ/WRITE, split into chunks of at most Eng->ChunkBytes.
//  The chunk size is a LIVE knob (nvmeofnd_chunkkb): the default is 4096, and
//  setting 64 makes one NVMe command move 64 KiB, whose payload the transport
//  carries as H2CData/C2HData PDUs.  It is deliberately NOT applied by restarting
//  the device: on 2026-10-18 `pnputil /restart-device` on this connected miniport
//  bugchecked with 0xCE (P2 = 0x10, unload with pending operations).
//
//  The CDW12 low 16 bits are the zero-based NLB, exactly as the NVMe spec and
//  this target's parser (nvmeof_tcp.c: nlb = (cdw12 & 0xFFFF) + 1) expect.  The
//  transport always puts CDW11 at zero, so SLBA[63:32] is zero: rejected here
//  rather than silently addressing the wrong blocks.
// ---------------------------------------------------------------------------
static ULONG NvmeofndReadChunkBytes(VOID);

static ULONG NvmeofndNowMs(VOID)
{
    LARGE_INTEGER t;
    KeQuerySystemTime(&t);          // 100 ns units since 1601
    return (ULONG)(t.QuadPart / 10000);
}

// At most one registry read per second: the knob is a test control, not something
// that changes per command, and ZwOpenKey/ZwQueryValueKey at PASSIVE is cheap but
// not free.
static VOID NvmeofndRefreshChunk(PNVMEOFND_ENGINE Eng)
{
    ULONG now = NvmeofndNowMs();
    ULONG bytes;

    if (Eng->ChunkCheckedAt != 0 && (ULONG)(now - Eng->ChunkCheckedAt) < 1000u) {
        return;
    }
    Eng->ChunkCheckedAt = now;
    bytes = NvmeofndReadChunkBytes();
    if (bytes != Eng->ChunkBytes) {
        NvmeofndLogF("command size changed live: %u -> %u bytes (nvmeofnd_chunkkb)",
                     Eng->ChunkBytes, bytes);
        Eng->ChunkBytes = bytes;
    }
}

static NTSTATUS NvmeofndTransportRw(PNVMEOFND_ENGINE Eng, PVOID Conn, BOOLEAN IsRead, ULONG Lba,
                                    PUCHAR Buf, ULONG Bytes, UCHAR* NvmeStatus,
                                    ULONG* Done)
{
    ULONG off = 0;

    *Done = 0;
    *NvmeStatus = 0xFF;
    NvmeofndRefreshChunk(Eng);

    while (off < Bytes) {
        ULONG chunk = Bytes - off;
        ULONG blocks;
        UINT32 cdw12;
        UINT8  status = 0xFF;
        NTSTATUS st;

        if (chunk > Eng->ChunkBytes) {
            chunk = Eng->ChunkBytes;
        }
        if ((chunk % NVMEOFND_BLOCK_BYTES) != 0) {
            // A partial block cannot be expressed as an NLB; this would be a bug
            // in the CDB parsing above, so refuse instead of writing short.
            return STATUS_INVALID_PARAMETER;
        }
        blocks = chunk / NVMEOFND_BLOCK_BYTES;
        cdw12  = (blocks - 1) & 0xFFFFu;

        st = NvtcpCommand(Conn,
                          IsRead ? NVMEOF_TCPK_OPC_READ : NVMEOF_TCPK_OPC_WRITE,
                          NVMEOFND_NSID,
                          Lba + (off / NVMEOFND_BLOCK_BYTES),
                          cdw12,
                          IsRead ? NULL : (PVOID)(Buf + off),
                          IsRead ? 0 : chunk,
                          IsRead ? (PVOID)(Buf + off) : NULL,
                          IsRead ? chunk : 0,
                          &status);
        *NvmeStatus = status;
        if (!NT_SUCCESS(st)) {
            Eng->CmdTransportErr++;
            return st;
        }
        if (status != 0) {
            Eng->CmdNvmeErr++;
            return STATUS_IO_DEVICE_ERROR;
        }
        Eng->CmdOk++;
        off += chunk;
        *Done = off;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  Bring-up: capture the provider NPI (once), open the connection, Identify
//  controller, Identify namespace 1, learn the geometry.
// ---------------------------------------------------------------------------
static NTSTATUS NvmeofndConnect(PNVMEOFND_ENGINE Eng, PULONG OutBlocks)
{
    NTSTATUS st;
    PVOID conn = NULL;
    ULONGLONG nsze;
    ULONG nlbaf, flbas, lbaf0;
    UCHAR rawLbads;
    char sn[21];
    char mn[41];

    if (!g_registered) {
        NvmeofndLogF("!! WskRegister never succeeded in DriverEntry; cannot open a connection");
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!g_npiCaptured) {
        st = WskCaptureProviderNPI(&g_registration, NVMEOFND_CAPTURE_WAIT_MS, &g_wskProvider);
        NvmeofndTrace(L"TraceWskCapture", (ULONG)st);
        NvmeofndLogF("WskCaptureProviderNPI(wait %u ms) = 0x%08X dispatch=%p client=%p",
                     (unsigned)NVMEOFND_CAPTURE_WAIT_MS, (unsigned)st,
                     (PVOID)g_wskProvider.Dispatch, g_wskProvider.Client);
        NvmeofndLogFlush();
        if (!NT_SUCCESS(st)) {
            return st;
        }
        g_npiCaptured = TRUE;
    } else {
        NvmeofndLogF("WSK provider NPI already captured (adapter restart): reusing it");
    }

    st = NvtcpOpen(&g_registration, &g_wskProvider, NVMEOFND_TARGET_IPV4,
                   (USHORT)NVMEOFND_TARGET_PORT, &conn);
    NvmeofndTrace(L"TraceNvtcpOpen", (ULONG)st);
    NvmeofndLogF("NvtcpOpen(127.0.0.1:%u) = 0x%08X conn=%p",
                 (unsigned)NVMEOFND_TARGET_PORT, (unsigned)st, conn);
    NvmeofndLogFlush();
    if (!NT_SUCCESS(st)) {
        return st;
    }
    Eng->Conn[0] = conn;

    // CNS=1, nsid 0: the controller.  Its model/serial fields are worth logging
    // before the same buffer is overwritten by the namespace Identify below.
    st = NvtcpIdentify(conn, NVMEOF_TCPK_CNS_CONTROLLER, 0, Eng->IdBuf);
    NvmeofndTrace(L"TraceIdentifyCtl", (ULONG)st);
    NvmeofndLogF("NvtcpIdentify(cns=1, nsid=0) = 0x%08X", (unsigned)st);
    if (NT_SUCCESS(st)) {
        // Model number is 40 ASCII bytes at offset 24, serial 20 bytes at 4.
        // Copied out with explicit lengths because the log formatter in this
        // file supports %s but not a precision (%.20s) field.
        RtlCopyMemory(sn, (PUCHAR)Eng->IdBuf + 4, 20);
        sn[20] = 0;
        RtlCopyMemory(mn, (PUCHAR)Eng->IdBuf + 24, 40);
        mn[40] = 0;
        NvmeofndLogF("  controller: VID 0x%04X SN %s MN %s",
                     (unsigned)((UCHAR*)Eng->IdBuf)[0] |
                     ((unsigned)((UCHAR*)Eng->IdBuf)[1] << 8),
                     sn, mn);
    }
    NvmeofndLogFlush();
    if (!NT_SUCCESS(st)) {
        goto fail;
    }

    // CNS=0, nsid 1: the namespace, which is where the real block count lives.
    st = NvtcpIdentify(conn, NVMEOF_TCPK_CNS_NAMESPACE, NVMEOFND_NSID, Eng->IdBuf);
    NvmeofndTrace(L"TraceIdentifyNs", (ULONG)st);
    NvmeofndLogF("NvtcpIdentify(cns=0, nsid=%u) = 0x%08X", (unsigned)NVMEOFND_NSID, (unsigned)st);
    NvmeofndLogFlush();
    if (!NT_SUCCESS(st)) {
        goto fail;
    }

    nsze   = NvmeofndGet64((const UCHAR*)Eng->IdBuf + 0);      // NSZE
    nlbaf  = ((const UCHAR*)Eng->IdBuf)[25];                    // NLBAF (zero based)
    flbas  = ((const UCHAR*)Eng->IdBuf)[26];                    // FLBAS
    lbaf0  = NvmeofndGet32((const UCHAR*)Eng->IdBuf + 128 + 4 * (flbas & 0x0F));
    rawLbads = (UCHAR)(lbaf0 & 0xFF);

    NvmeofndTrace(L"TraceNsze", (ULONG)nsze);
    NvmeofndTrace(L"TraceNlbaf", nlbaf);
    NvmeofndTrace(L"TraceFlbas", flbas);
    NvmeofndTrace(L"TraceLbads", (ULONG)rawLbads);
    NvmeofndLogF("  namespace 1: NSZE=%llu blocks, NLBAF=%u FLBAS=%u LBADS_byte=%u"
                 " (target writes its LBAF table off by four; block size is taken as %u)",
                 nsze, nlbaf, flbas, (unsigned)rawLbads, (unsigned)NVMEOFND_BLOCK_BYTES);
    NvmeofndLogFlush();

    if (nsze == 0 || nsze > 0xFFFFFFFFull) {
        // Zero blocks is an empty namespace; more than 4 Gi blocks cannot be
        // addressed because the transport hardcodes CDW11 (SLBA[63:32]) to zero.
        NvmeofndLogF("!! NSZE=%llu is unusable (must be 1..0xFFFFFFFF)", nsze);
        NvmeofndLogFlush();
        st = STATUS_INVALID_DEVICE_STATE;
        goto fail;
    }

    Eng->Blocks     = (ULONG)nsze;
    Eng->BlockBytes = NVMEOFND_BLOCK_BYTES;
    *OutBlocks      = Eng->Blocks;
    return STATUS_SUCCESS;

fail:
    // Nothing else has used the connection yet, so closing it here is the clean
    // path the transport supports (no command in flight, no poisoned wait).
    (void)NvtcpClose(Eng->Conn[0]);
    Eng->Conn[0] = NULL;
    return st;
}

typedef struct _NVMEOFND_WORKER_CTX {
    PNVMEOFND_ENGINE Eng;
    ULONG            Index;
} NVMEOFND_WORKER_CTX, *PNVMEOFND_WORKER_CTX;

static ULONG NvmeofndReadDepth(VOID);

// The depth is a LIVE knob, and boot always starts at 1 (the safe value): a batch
// test that wedges the machine therefore comes back safe by itself.  Read at most
// once a second, so setting 1 in the registry stops batching within a second.
static VOID NvmeofndRefreshDepth(PNVMEOFND_ENGINE Eng)
{
    ULONG now = NvmeofndNowMs();
    ULONG d;

    if (Eng->DepthCheckedAt != 0 && (ULONG)(now - Eng->DepthCheckedAt) < 1000u) {
        return;
    }
    Eng->DepthCheckedAt = now;
    d = NvmeofndReadDepth();
    if (d != Eng->PipeDepth) {
        NvmeofndLogF("pipeline depth changed live: %u -> %u", Eng->PipeDepth, d);
        Eng->PipeDepth = d;
    }
}

// A READ/WRITE SRB that fits in ONE NVMe command at the current chunk size can go
// into a batch.  The parsing deliberately matches ProcessOne: same CDB forms,
// same range check, same whole-block rounding; anything that does not fit is left
// to ProcessOne, which also owns the chunked path.
static BOOLEAN NvmeofndPrepareRw(PNVMEOFND_ENGINE Eng, PVOID Ext, PSCSI_REQUEST_BLOCK Srb,
                                 NVTCPK_CMD* Cmd, ULONG* BytesOut, BOOLEAN* IsReadOut)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)Ext;
    UCHAR* cdb = Srb->Cdb;
    UCHAR op = cdb[0];
    BOOLEAN isRead;
    ULONG lba = 0, blocks = 0, bytes;
    ULONGLONG lba64 = 0;
    PVOID buffer = NULL;

    switch (op) {
    case NVMEOFND_OPC_READ6:
    case NVMEOFND_OPC_WRITE6:
    case NVMEOFND_OPC_READ10:
    case NVMEOFND_OPC_WRITE10:
    case NVMEOFND_OPC_READ16:
    case NVMEOFND_OPC_WRITE16:
        break;
    default:
        return FALSE;
    }
    isRead = (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_READ10 ||
              op == NVMEOFND_OPC_READ16);
    if (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_WRITE6) {
        lba    = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        blocks = (cdb[4] == 0) ? 256u : (ULONG)cdb[4];
    } else if (op == NVMEOFND_OPC_READ10 || op == NVMEOFND_OPC_WRITE10) {
        lba    = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
        blocks = ((ULONG)cdb[7] << 8) | cdb[8];
    } else {
        lba64  = ((ULONGLONG)cdb[2] << 56) | ((ULONGLONG)cdb[3] << 48) |
                 ((ULONGLONG)cdb[4] << 40) | ((ULONGLONG)cdb[5] << 32) |
                 ((ULONGLONG)cdb[6] << 24) | ((ULONGLONG)cdb[7] << 16) |
                 ((ULONGLONG)cdb[8] << 8)  | (ULONGLONG)cdb[9];
        blocks = ((ULONG)cdb[10] << 24) | ((ULONG)cdb[11] << 16) |
                 ((ULONG)cdb[12] << 8)  | (ULONG)cdb[13];
        lba    = (lba64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (ULONG)lba64;
    }
    if (blocks == 0) {
        return FALSE;
    }
    if (lba64 > 0xFFFFFFFFull || lba >= ext->Blocks || blocks > (ext->Blocks - lba)) {
        return FALSE;                       // out of range: let ProcessOne reject it
    }
    bytes = blocks * ext->BlockBytes;
    if (bytes > Srb->DataTransferLength) {
        ULONG fits = Srb->DataTransferLength / ext->BlockBytes;
        if (fits == 0) {
            return FALSE;
        }
        blocks = fits;
        bytes  = blocks * ext->BlockBytes;
    }
    if (bytes == 0 || bytes > Eng->ChunkBytes) {
        return FALSE;                       // needs the chunked path
    }
    if (StorPortGetSystemAddress(Ext, Srb, &buffer) != STOR_STATUS_SUCCESS || buffer == NULL) {
        return FALSE;
    }

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->opcode = isRead ? NVMEOF_TCPK_OPC_READ : NVMEOF_TCPK_OPC_WRITE;
    Cmd->nsid   = NVMEOFND_NSID;
    Cmd->cdw10  = lba;
    Cmd->cdw12  = (blocks - 1) & 0xFFFFu;
    if (isRead) {
        Cmd->outData  = buffer;
        Cmd->outBytes = bytes;
    } else {
        Cmd->inData   = buffer;
        Cmd->inBytes  = bytes;
    }
    *BytesOut = bytes;
    *IsReadOut = isRead;
    return TRUE;
}

/* Phase A timing: how long an SRB sits between HwStartIo's enqueue and the
 * worker picking it up (queue wait + thread wake), and how long the worker
 * spends on the whole command.  Averaged over 64 SRBs, one log line plus two
 * registry DWORDs (nvmeofnd_phA_us / nvmeofnd_phCmd_us). */
static ULONGLONG g_ndFreq = 0;
static ULONGLONG g_pmA, g_pmCmd;
static ULONG     g_pmN;

static ULONGLONG NvmeofndQpcNow(VOID)
{
    LARGE_INTEGER now;

    if (g_ndFreq == 0) {
        LARGE_INTEGER f;
        (void)KeQueryPerformanceCounter(&f);
        g_ndFreq = (ULONGLONG)f.QuadPart;
    }
    now = KeQueryPerformanceCounter(NULL);
    return (ULONGLONG)now.QuadPart;
}

static ULONGLONG NvmeofndQpcUs(ULONGLONG from, ULONGLONG to)
{
    if (g_ndFreq == 0 || to < from) {
        return 0;
    }
    return ((to - from) * 1000000ULL) / g_ndFreq;
}

// One worker per connection.  With nvmeofnd_pipelinedepth = 1 it takes one slot per
// wake and runs it; above that it takes up to that many ready READ/WRITE SRBs and
// hands them to NvtcpkBatch, so one connection keeps several commands in flight.
static VOID NvmeofndWorker(PVOID Context)
{
    PNVMEOFND_WORKER_CTX w = (PNVMEOFND_WORKER_CTX)Context;
    PNVMEOFND_ENGINE eng = w->Eng;
    PVOID conn = eng->Conn[w->Index];

    for (;;) {
        LARGE_INTEGER t;
        KIRQL irql;

        t.QuadPart = -((LONGLONG)NVMEOFND_WAIT_MS * 10000);
        (void)KeWaitForSingleObject(&eng->Wake, Executive, KernelMode, FALSE, &t);

        if (InterlockedCompareExchange(&eng->Stop, 0, 0) != 0) {
            break;
        }
        KeResetEvent(&eng->Wake);
        NvmeofndRefreshDepth(eng);

        if (eng->PipeDepth > 1) {
            NVTCPK_CMD cmds[NVMEOFND_MAX_PIPELINE];
            PSCSI_REQUEST_BLOCK srbs[NVMEOFND_MAX_PIPELINE];
            PVOID exts[NVMEOFND_MAX_PIPELINE];
            ULONG lens[NVMEOFND_MAX_PIPELINE];
            BOOLEAN isReads[NVMEOFND_MAX_PIPELINE];
            ULONG n = 0, i;
            UCHAR batchOp = 0;
            ULONG batchBytes = 0;
            NTSTATUS st;

            while (n < eng->PipeDepth) {
                NVMEOFND_SLOT sl = { NULL, NULL };
                BOOLEAN got = FALSE;
                BOOLEAN isRead = FALSE;
                ULONG bytes = 0;

                KeAcquireSpinLock(&eng->Lock, &irql);
                if (eng->Count != 0) {
                    sl = eng->Slot[eng->Head];
                    eng->Head = (eng->Head + 1) % NVMEOFND_MAX_PENDING;
                    eng->Count--;
                    got = TRUE;
                }
                KeReleaseSpinLock(&eng->Lock, irql);
                if (!got) {
                    break;
                }
                if (!NvmeofndPrepareRw(eng, sl.Ext, sl.Srb, &cmds[n], &bytes, &isRead)) {
                    NvmeofndProcessOne(eng, sl.Ext, sl.Srb, conn);
                    continue;
                }
                // ONE DIRECTION PER BATCH.  A mixed batch is what deadlocked the
                // first version: the host sends a Write's payload before reading,
                // while the target is blocked sending the earlier Read's data that
                // the host has not read yet - with the default (small) socket
                // buffers both sides end up waiting for the other.  Reads only send
                // tiny capsules; Writes only receive tiny completions; mixing them
                // is what fills both directions.
                if (n != 0 && cmds[n].opcode != batchOp) {
                    NvmeofndProcessOne(eng, sl.Ext, sl.Srb, conn);
                    continue;
                }
                if (n != 0 && batchBytes + bytes > 1024u * 1024u) {
                    NvmeofndProcessOne(eng, sl.Ext, sl.Srb, conn);
                    continue;
                }
                if (n == 0) {
                    batchOp = cmds[n].opcode;
                }
                batchBytes += bytes;
                srbs[n] = sl.Srb;
                exts[n] = sl.Ext;
                lens[n] = bytes;
                isReads[n] = isRead;
                n++;
            }
            if (n == 0) {
                // nothing to do this wake
            } else if (NT_SUCCESS(st = NvtcpkBatch(conn, cmds, n))) {
                for (i = 0; i < n; i++) {
                    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)exts[i];
                    PSCSI_REQUEST_BLOCK srb = srbs[i];

                    if (cmds[i].status != 0) {
                        if (isReads[i]) {
                            NvmeofndSetSense(ext, srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x11, 0x00);
                        } else {
                            NvmeofndSetSense(ext, srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
                        }
                        NvmeofndComplete(ext, srb, SRB_STATUS_ERROR);
                        continue;
                    }
                    if (isReads[i]) {
                        ext->Reads++;
                    } else {
                        ext->Writes++;
                    }
                    ext->IoBytes += lens[i];
                    srb->DataTransferLength = (cmds[i].dataLen != 0) ? cmds[i].dataLen : lens[i];
                    srb->ScsiStatus = NVMEOFND_STAT_GOOD;
                    NvmeofndComplete(ext, srb, SRB_STATUS_SUCCESS);
                }
            } else {
                for (i = 0; i < n; i++) {
                    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)exts[i];
                    PSCSI_REQUEST_BLOCK srb = srbs[i];

                    ext->TransportErrors++;
                    if (st == STATUS_DEVICE_NOT_READY || st == STATUS_DEVICE_BUSY ||
                        st == STATUS_INVALID_HANDLE) {
                        ext->NotReadyAnswers++;
                        NvmeofndSetSense(ext, srb, NVMEOFND_SENSE_NOT_READY, 0x04, 0x02);
                    } else if (isReads[i]) {
                        NvmeofndSetSense(ext, srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x11, 0x00);
                    } else {
                        NvmeofndSetSense(ext, srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
                    }
                    NvmeofndComplete(ext, srb, SRB_STATUS_ERROR);
                }
            }
        } else {
            NVMEOFND_SLOT s = { NULL, NULL };
            BOOLEAN have = FALSE;

            KeAcquireSpinLock(&eng->Lock, &irql);
            if (eng->Count != 0) {
                s = eng->Slot[eng->Head];
                eng->Head = (eng->Head + 1) % NVMEOFND_MAX_PENDING;
                eng->Count--;
                have = TRUE;
            }
            KeReleaseSpinLock(&eng->Lock, irql);

            if (have) {
                // NEVER under the spinlock: completion can call back into HwStartIo
                // (StorPort may start the next request from here), and that path
                // takes this same lock.
                ULONGLONG t0 = NvmeofndQpcNow();
                ULONGLONG t1;

                NvmeofndProcessOne(eng, s.Ext, s.Srb, conn);
                t1 = NvmeofndQpcNow();
                g_pmA += NvmeofndQpcUs(s.Stamp, t0);
                g_pmCmd += NvmeofndQpcUs(t0, t1);
                if (++g_pmN >= 64) {
                    NvmeofndLogF("phase miniport n=%u: A(startio->worker)=%u us"
                                 "  cmd(worker whole)=%u us",
                                 g_pmN, (unsigned)(g_pmA / g_pmN),
                                 (unsigned)(g_pmCmd / g_pmN));
                    NvmeofndTrace(L"nvmeofnd_phA_us", (ULONG)(g_pmA / g_pmN));
                    NvmeofndTrace(L"nvmeofnd_phCmd_us", (ULONG)(g_pmCmd / g_pmN));
                    g_pmA = g_pmCmd = 0;
                    g_pmN = 0;
                }
            }
        }
        // More work waiting: wake somebody (possibly this thread again) rather
        // than leave a slot stranded behind an unsignalled event.
        if (InterlockedCompareExchange((volatile LONG*)&eng->Count, 0, 0) != 0) {
            KeSetEvent(&eng->Wake, IO_NO_INCREMENT, FALSE);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS NvmeofndStartWorker(PNVMEOFND_ENGINE Eng)
{
    ULONG i;

    if (Eng->ThreadStarted) {
        return STATUS_SUCCESS;
    }
    if (Eng->Queues == 0) {
        Eng->Queues = 1;
    }
    for (i = 0; i < Eng->Queues; i++) {
        PNVMEOFND_WORKER_CTX ctx;
        HANDLE hThread = NULL;
        NTSTATUS st;

        ctx = (PNVMEOFND_WORKER_CTX)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx),
                                                    NVMEOFND_TAG);
        if (ctx == NULL) {
            break;
        }
        ctx->Eng = Eng;
        ctx->Index = i;
        st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                  NvmeofndWorker, ctx);
        NvmeofndLogF("PsCreateSystemThread(worker %u) = 0x%08X", i, (unsigned)st);
        if (!NT_SUCCESS(st)) {
            ExFreePoolWithTag(ctx, NVMEOFND_TAG);
            break;
        }
        // The thread owns its own reference once created; the handle is not needed
        // (and must not be waited on, because this driver is never unloaded).
        ZwClose(hThread);
    }
    NvmeofndTrace(L"TraceWorkerCreate", i);
    if (i == 0) {
        return STATUS_UNSUCCESSFUL;
    }
    Eng->Queues = i;
    Eng->ThreadStarted = TRUE;
    NvmeofndLogF("worker threads started: %u connection(s) in flight", i);
    return STATUS_SUCCESS;
}

// How many bytes one NVMe READ/WRITE moves.  4 KiB is the old in-capsule
// behaviour and stays the default, so a boot with the new driver is identical to
// the one before it.  nvmeofnd_chunkkb = 64 makes one command cover a 64 KiB
// transfer, whose payload the transport then moves as H2CData/C2HData PDUs.
// Read at engine attach, which HwInitialize calls: a device restart picks up a
// new value without unloading the driver.
#define NVMEOFND_CHUNK_MIN      4096u
/* 1 MiB now: the miniport already reports MaximumTransferLength = 1 MiB and the
 * transport takes NVMEOF_TCPK_IO_MAX = 1 MiB, so 512 KiB only forced an internal
 * split of exactly the commands NTFS was already issuing.  A single command is
 * latency-bound (~6 ms per 512 KiB), so doubling the data per command is the
 * cheapest throughput lever there is. */
#define NVMEOFND_CHUNK_MAX      (1024u * 1024u)
#define NVMEOFND_MAX_QUEUES     8u
#define NVMEOFND_MAX_PIPELINE   8u

static ULONG NvmeofndReadChunkBytes(VOID)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buf[24];
    ULONG len = sizeof(buf);
    ULONG kb = 4;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return NVMEOFND_CHUNK_MIN;
    }
    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&valueName, L"nvmeofnd_chunkkb");
        if (NT_SUCCESS(ZwQueryValueKey(key, &valueName, KeyValuePartialInformation, buf,
                                       sizeof(buf), &len))) {
            KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                kb = *(ULONG*)p->Data;
            }
        }
        ZwClose(key);
    }
    if (kb < 4)   { kb = 4; }
    if (kb > (NVMEOFND_CHUNK_MAX / 1024u)) { kb = (NVMEOFND_CHUNK_MAX / 1024u); }
    return kb * 1024u;
}

// How many connections (and worker threads) to use.  Default 1 keeps the old
// single-command-at-a-time behaviour; nvmeofnd_queues = 4 puts four commands in
// flight on four connections.  Read at attach like the chunk size, so the value
// is changed by a reboot (never by restarting the device: that is the 0xCE).
static ULONG NvmeofndReadQueues(VOID)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buf[24];
    ULONG len = sizeof(buf);
    ULONG q = 1;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return 1;
    }
    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&valueName, L"nvmeofnd_queues");
        if (NT_SUCCESS(ZwQueryValueKey(key, &valueName, KeyValuePartialInformation, buf,
                                       sizeof(buf), &len))) {
            KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                q = *(ULONG*)p->Data;
            }
        }
        ZwClose(key);
    }
    if (q < 1) { q = 1; }
    if (q > NVMEOFND_MAX_QUEUES) { q = NVMEOFND_MAX_QUEUES; }
    return q;
}

// How many SRBs one worker hands to the transport at once.  Default 1 keeps the
// old behaviour exactly; nvmeofnd_pipelinedepth = 4 sends four capsules before
// reading any response, which is what hides the per-command round trip.
static ULONG NvmeofndReadDepth(VOID)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buf[24];
    ULONG len = sizeof(buf);
    ULONG d = 1;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return 1;
    }
    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&valueName, L"nvmeofnd_pipelinedepth");
        if (NT_SUCCESS(ZwQueryValueKey(key, &valueName, KeyValuePartialInformation, buf,
                                       sizeof(buf), &len))) {
            KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                d = *(ULONG*)p->Data;
            }
        }
        ZwClose(key);
    }
    if (d < 1) { d = 1; }
    if (d > NVMEOFND_MAX_PIPELINE) { d = NVMEOFND_MAX_PIPELINE; }
    return d;
}

// Idempotent: HwInitialize calls it once, and a device restart finds the engine
// already built and simply reuses it.
static NTSTATUS NvmeofndEngineAttach(PNVMEOFND_EXTENSION Ext)
{
    NTSTATUS st;
    ULONG blocks = 0;

    if (g_engine == NULL) {
        PNVMEOFND_ENGINE eng = (PNVMEOFND_ENGINE)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                                sizeof(NVMEOFND_ENGINE),
                                                                NVMEOFND_TAG);
        if (eng == NULL) {
            NvmeofndLogF("!! no non-paged pool for the engine");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(eng, sizeof(*eng));
        KeInitializeSpinLock(&eng->Lock);
        KeInitializeEvent(&eng->Wake, NotificationEvent, FALSE);
        eng->IdBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, NVMEOF_TCPK_MAX_DATA, NVMEOFND_TAG);
        if (eng->IdBuf == NULL) {
            NvmeofndLogF("!! no non-paged pool for the Identify buffer");
            ExFreePoolWithTag(eng, NVMEOFND_TAG);      // nothing points at it yet
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        g_engine = eng;
        NvmeofndLogF("engine %p allocated (non-paged, %u slots, never freed on purpose)",
                     eng, (unsigned)NVMEOFND_MAX_PENDING);
    }

    if (g_engine->Conn[0] == NULL) {
        st = NvmeofndConnect(g_engine, &blocks);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    } else {
        NvmeofndLogF("connection already open (%p): reusing it", g_engine->Conn[0]);
    }

    // Extra connections = extra commands in flight.  Each one is a separate
    // NVMe/TCP connection, which is exactly how the standard expresses multiple
    // queues; the target already serves each connection in its own thread.
    g_engine->Queues = NvmeofndReadQueues();
    {
        ULONG i;
        for (i = 1; i < g_engine->Queues; i++) {
            PVOID extra = NULL;
            if (g_engine->Conn[i] != NULL) {
                continue;
            }
            st = NvtcpOpen(&g_registration, &g_wskProvider, NVMEOFND_TARGET_IPV4,
                           (USHORT)NVMEOFND_TARGET_PORT, &extra);
            if (!NT_SUCCESS(st)) {
                NvmeofndLogF("extra connection %u/%u failed 0x%08X; keeping %u queue(s)",
                             i, g_engine->Queues, (unsigned)st, i);
                g_engine->Queues = i;
                break;
            }
            g_engine->Conn[i] = extra;
        }
    }
    NvmeofndLogF("queues: %u connection(s) -> up to %u commands in flight",
                 g_engine->Queues, g_engine->Queues);

    // Boot always arms depth 1 AND writes 1 back: a batch test that wedges the
    // machine therefore cannot arm a wedge loop at the next boot.  The knob is
    // live (NvmeofndRefreshDepth), so a test sets 2 once the system is up.
    g_engine->PipeDepth = 1;
    NvmeofndTrace(L"nvmeofnd_pipelinedepth", 1);
    NvmeofndLogF("pipeline depth: 1 at boot; nvmeofnd_pipelinedepth is a live knob");

    st = NvmeofndStartWorker(g_engine);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    // Boot SAFE: always start at 4 KiB and write that back to the registry, so a
    // crash caused by a larger live value cannot arm the next boot with it too.
    // NvmeofndRefreshChunk picks up a live change within a second.
    g_engine->ChunkBytes = NVMEOFND_CHUNK_MIN;
    g_engine->ChunkCheckedAt = 0;
    NvmeofndTrace(L"nvmeofnd_chunkkb", NVMEOFND_CHUNK_MIN / 1024u);
    NvmeofndLogF("command size at boot: %u bytes; nvmeofnd_chunkkb is re-read live (1 s)",
                 g_engine->ChunkBytes);

    Ext->Eng        = g_engine;
    Ext->Blocks     = g_engine->Blocks;
    Ext->BlockBytes = g_engine->BlockBytes;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  Stats: published from the worker at PASSIVE_LEVEL, so registry writes are
//  legal here.  Called from the WMI SRB path and every 128 completions.
// ---------------------------------------------------------------------------
static VOID NvmeofndPublishStats(PVOID DeviceExtension)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    static const UCHAR interesting[] = {
        0x00, 0x03, 0x12, 0x1A, 0x1B, 0x25, 0x28, 0x2A, 0x2F, 0x35,
        0x5A, 0x88, 0x8A, 0x91, 0x9E, 0xA0
    };
    WCHAR name[32];
    ULONG i;

    NvmeofndTrace(L"StatTotal", ext->TotalSrbs);
    NvmeofndTrace(L"StatReads", ext->Reads);
    NvmeofndTrace(L"StatWrites", ext->Writes);
    NvmeofndTrace(L"StatIoBytes", ext->IoBytes);
    NvmeofndTrace(L"StatTransportErr", ext->TransportErrors);
    NvmeofndTrace(L"StatNotReady", ext->NotReadyAnswers);
    NvmeofndTrace(L"StatReady", ext->Ready ? 1 : 0);
    NvmeofndTrace(L"StatResetBus", ext->ResetBusCount);
    NvmeofndTrace(L"StatQueueFull", ext->QueueFullCount);
    if (ext->Eng != NULL) {
        NvmeofndTrace(L"StatCmdOk", ext->Eng->CmdOk);
        NvmeofndTrace(L"StatCmdNvmeErr", ext->Eng->CmdNvmeErr);
        NvmeofndTrace(L"StatCmdTransportErr", ext->Eng->CmdTransportErr);
    }
    for (i = 0; i < sizeof(interesting) / sizeof(interesting[0]); i++) {
        UCHAR op = interesting[i];
        if (ext->OpCount[op] == 0) {
            continue;
        }
        {   // build "StatOp_XX" by hand: no ntstrsafe dependency for nine characters
            static const WCHAR hexd[] = L"0123456789ABCDEF";
            name[0] = L'S'; name[1] = L't'; name[2] = L'a'; name[3] = L't';
            name[4] = L'O'; name[5] = L'p'; name[6] = L'_';
            name[7] = hexd[(op >> 4) & 0x0F];
            name[8] = hexd[op & 0x0F];
            name[9] = 0;
            NvmeofndTrace(name, ext->OpCount[op]);
        }
    }
}

// ---------------------------------------------------------------------------
//  The SCSI command set Windows actually uses against a disk, answered at
//  PASSIVE_LEVEL in the worker.  READ and WRITE come from the remote namespace;
//  everything else is answered locally, exactly as D1.1 did.
//  Every command that cannot be served gets CHECK CONDITION and a real sense
//  key rather than a bare failure, because a host that gets a bare failure
//  retries forever.
// ---------------------------------------------------------------------------
static VOID NvmeofndProcessOne(PNVMEOFND_ENGINE Eng, PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb, PVOID Conn)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    PUCHAR cdb;
    PVOID  buffer = NULL;
    UCHAR  op;

    ext->TotalSrbs++;

    if (Srb->Function == SRB_FUNCTION_WMI) {
        NvmeofndPublishStats(DeviceExtension);
        NvmeofndLogFlush();
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return;
    }
    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_INVALID_REQUEST);
        return;
    }
    if (!ext->Ready || Eng == NULL || Eng->Conn[0] == NULL) {
        // Bringing the adapter up failed, or the connection is gone.  Say "not
        // ready, initialisation required" (0x02/0x04/0x02): the host retries
        // instead of deciding the disk is broken or zero-sized.
        ext->NotReadyAnswers++;
        NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_NOT_READY, 0x04, 0x02);
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return;
    }
    if (Srb->CdbLength < 6) {
        NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return;
    }
    cdb = Srb->Cdb;
    op = cdb[0];
    ext->OpCount[op]++;

    if (Srb->DataTransferLength > 0 &&
        StorPortGetSystemAddress(DeviceExtension, Srb, &buffer) != STOR_STATUS_SUCCESS) {
        buffer = NULL;
    }

    switch (op) {

    case NVMEOFND_OPC_TEST_UNIT_READY:
    case NVMEOFND_OPC_START_STOP_UNIT:
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;

    case NVMEOFND_OPC_VERIFY10:
        // Nothing to verify locally: a successful no-op is what every RAM-less
        // miniport answers, and the bytes were read/written by NVMe already.
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;

    case NVMEOFND_OPC_INQUIRY: {
        BOOLEAN evpd = (cdb[1] & 0x01) != 0;
        UCHAR   page = cdb[2];
        ULONG   want = Srb->DataTransferLength;
        ULONG   give = 0;

        if (buffer == NULL) {
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        RtlZeroMemory(buffer, want);
        if (!evpd) {
            give = (want < sizeof(g_Inquiry)) ? want : sizeof(g_Inquiry);
            RtlCopyMemory(buffer, g_Inquiry, give);
            Srb->DataTransferLength = give;
        } else if (page == 0x00) {
            static const UCHAR pages[3] = { 0x00, 0x80, 0x83 };
            give = (want < 7) ? want : 7;
            ((PUCHAR)buffer)[1] = 0x00;
            ((PUCHAR)buffer)[3] = 3;
            RtlCopyMemory((PUCHAR)buffer + 4, pages, 3);
            Srb->DataTransferLength = give;
        } else if (page == 0x80) {
            // A NEW serial: the D1.1 RAM LUN used NVMEOFND0000000000001, and a
            // different serial keeps this disk a separate device identity.
            static const char serial[] = "NVMEOFNDTCP00000001";
            give = (want < (ULONG)(5 + sizeof(serial) - 1)) ? want : (ULONG)(5 + sizeof(serial) - 1);
            ((PUCHAR)buffer)[1] = 0x80;
            ((PUCHAR)buffer)[3] = (UCHAR)(sizeof(serial) - 1);
            RtlCopyMemory((PUCHAR)buffer + 4, serial, sizeof(serial) - 1);
            Srb->DataTransferLength = give;
        } else if (page == 0x83) {
            // One identification descriptor: T10 vendor id "NVMEOFND" + serial.
            static const char vpd83[] = "NVMEOFND:TCP000000001";
            ULONG dlen = (ULONG)(sizeof(vpd83) - 1);
            if (want >= 8 + dlen) {
                PUCHAR d = (PUCHAR)buffer + 4;
                d[0] = 0x02;                    // code set: ASCII
                d[1] = 0x01;                    // identifier type: T10 vendor ID
                d[3] = (UCHAR)dlen;
                RtlCopyMemory(d + 4, vpd83, dlen);
                ((PUCHAR)buffer)[1] = 0x83;
                ((PUCHAR)buffer)[3] = (UCHAR)(4 + dlen);
                Srb->DataTransferLength = 8 + dlen;
            } else {
                Srb->DataTransferLength = 4;
            }
        } else {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x24, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_READ_CAPACITY10: {
        PUCHAR out = (PUCHAR)buffer;
        ULONG  lastLba = ext->Blocks - 1;
        if (out == NULL || Srb->DataTransferLength < 8) {
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        out[0] = (UCHAR)(lastLba >> 24); out[1] = (UCHAR)(lastLba >> 16);
        out[2] = (UCHAR)(lastLba >> 8);  out[3] = (UCHAR)(lastLba);
        out[4] = (UCHAR)(ext->BlockBytes >> 24); out[5] = (UCHAR)(ext->BlockBytes >> 16);
        out[6] = (UCHAR)(ext->BlockBytes >> 8);  out[7] = (UCHAR)(ext->BlockBytes);
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_SERVICE_IN16: {
        // SERVICE ACTION IN(16), service action 0x10 = READ CAPACITY(16).
        PUCHAR out = (PUCHAR)buffer;
        ULONGLONG last = (ULONGLONG)(ext->Blocks - 1);
        if ((cdb[1] & 0x1F) != 0x10 || out == NULL || Srb->DataTransferLength < 32) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        RtlZeroMemory(out, 32);
        out[0] = (UCHAR)(last >> 56); out[1] = (UCHAR)(last >> 48);
        out[2] = (UCHAR)(last >> 40); out[3] = (UCHAR)(last >> 32);
        out[4] = (UCHAR)(last >> 24); out[5] = (UCHAR)(last >> 16);
        out[6] = (UCHAR)(last >> 8);  out[7] = (UCHAR)(last);
        out[8] = (UCHAR)(ext->BlockBytes >> 24); out[9] = (UCHAR)(ext->BlockBytes >> 16);
        out[10] = (UCHAR)(ext->BlockBytes >> 8); out[11] = (UCHAR)(ext->BlockBytes);
        Srb->DataTransferLength = 32;
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_MODE_SENSE6:
    case NVMEOFND_OPC_MODE_SENSE10: {
        BOOLEAN ten = (op == NVMEOFND_OPC_MODE_SENSE10);
        ULONG hdr = ten ? 8 : 4;
        PUCHAR out = (PUCHAR)buffer;
        if (out == NULL || Srb->DataTransferLength < hdr) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        RtlZeroMemory(out, hdr);
        if (ten) {
            out[1] = (UCHAR)(hdr - 2);          // mode data length = everything after this field
        } else {
            out[0] = (UCHAR)(hdr - 1);
        }
        Srb->DataTransferLength = hdr;          // no block descriptor, no pages
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_READ6:
    case NVMEOFND_OPC_WRITE6:
    case NVMEOFND_OPC_READ10:
    case NVMEOFND_OPC_WRITE10:
    case NVMEOFND_OPC_READ16:
    case NVMEOFND_OPC_WRITE16: {
        BOOLEAN isRead = (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_READ10 ||
                          op == NVMEOFND_OPC_READ16);
        ULONG lba = 0, blocks = 0, bytes;
        ULONGLONG lba64 = 0;
        NTSTATUS st;
        UCHAR    nvmeStatus = 0xFF;
        ULONG    done = 0;

        if (op == NVMEOFND_OPC_READ6 || op == NVMEOFND_OPC_WRITE6) {
            lba    = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
            blocks = (cdb[4] == 0) ? 256u : (ULONG)cdb[4];
        } else if (op == NVMEOFND_OPC_READ10 || op == NVMEOFND_OPC_WRITE10) {
            lba    = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
            blocks = ((ULONG)cdb[7] << 8) | cdb[8];
        } else {
            lba64  = ((ULONGLONG)cdb[2] << 56) | ((ULONGLONG)cdb[3] << 48) |
                     ((ULONGLONG)cdb[4] << 40) | ((ULONGLONG)cdb[5] << 32) |
                     ((ULONGLONG)cdb[6] << 24) | ((ULONGLONG)cdb[7] << 16) |
                     ((ULONGLONG)cdb[8] << 8)  | (ULONGLONG)cdb[9];
            blocks = ((ULONG)cdb[10] << 24) | ((ULONG)cdb[11] << 16) |
                     ((ULONG)cdb[12] << 8)  | (ULONG)cdb[13];
            lba    = (lba64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (ULONG)lba64;
        }

        if (blocks == 0) {
            Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
            return;
        }
        if (lba64 > 0xFFFFFFFFull || lba >= ext->Blocks || blocks > (ext->Blocks - lba)) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x21, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }
        bytes = blocks * ext->BlockBytes;
        if (bytes > Srb->DataTransferLength) {
            // The host asked for fewer bytes than the CDB implies.  Transfer only
            // whole blocks: an NVMe READ/WRITE cannot express a partial block.
            ULONG fits = Srb->DataTransferLength / ext->BlockBytes;
            if (fits == 0) {
                NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
                NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
                return;
            }
            blocks = fits;
            bytes  = blocks * ext->BlockBytes;
        }
        if (buffer == NULL) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }

        st = NvmeofndTransportRw(Eng, Conn, isRead, lba, (PUCHAR)buffer, bytes, &nvmeStatus, &done);
        if (!NT_SUCCESS(st)) {
            ext->TransportErrors++;
            NvmeofndLogF("!! %s lba=%u blocks=%u bytes=%u: transport 0x%08X nvme_status=0x%02X"
                         " (done %u bytes)",
                         isRead ? "READ" : "WRITE", lba, blocks, bytes,
                         (unsigned)st, (unsigned)nvmeStatus, done);
            NvmeofndLogFlush();
            if (st == STATUS_DEVICE_NOT_READY || st == STATUS_DEVICE_BUSY ||
                st == STATUS_INVALID_HANDLE) {
                // The connection is unusable: report "not ready" so the host
                // retries instead of failing the volume hard.
                ext->NotReadyAnswers++;
                NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_NOT_READY, 0x04, 0x02);
            } else if (isRead) {
                NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x11, 0x00);
            } else {
                NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
            }
            NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
            return;
        }

        if (isRead) {
            ext->Reads++;
        } else {
            ext->Writes++;
        }
        ext->IoBytes += bytes;
        Srb->DataTransferLength = bytes;
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_SYNC_CACHE10:
    case NVMEOFND_OPC_SYNC_CACHE16: {
        // Best effort: every write this driver issues has already been answered
        // by the target's CapsuleResp before the SCSI write completes, so a
        // failed NVMe Flush cannot lose data that was acknowledged.  A flush that
        // the target does not implement must not fail the volume.
        UCHAR status = 0xFF;
        NTSTATUS st = NvtcpCommand(Conn, NVMEOF_TCPK_OPC_FLUSH, NVMEOFND_NSID,
                                   0, 0, NULL, 0, NULL, 0, &status);
        if (!NT_SUCCESS(st) || status != 0) {
            NvmeofndLogF("SYNC CACHE: NVMe Flush returned 0x%08X status 0x%02X"
                         " (ignored: writes are already synchronous)", (unsigned)st,
                         (unsigned)status);
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_REQUEST_SENSE: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 18) {
            RtlZeroMemory(out, 18);
            out[0] = 0x70;
            out[7] = 10;
            Srb->DataTransferLength = 18;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    case NVMEOFND_OPC_REPORT_LUNS: {
        PUCHAR out = (PUCHAR)buffer;
        if (out != NULL && Srb->DataTransferLength >= 16) {
            RtlZeroMemory(out, 16);
            out[3] = 8;
            Srb->DataTransferLength = 16;
        }
        Srb->ScsiStatus = NVMEOFND_STAT_GOOD;
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_SUCCESS);
        return;
    }

    default:
        NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_ILLEGAL_REQ, 0x20, 0x00);
        NvmeofndComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return;
    }
}

// ---------------------------------------------------------------------------
//  HwFindAdapter - for a virtual device this is where the shape is declared.
//  ConfigInfo->VirtualDevice = TRUE is what tells StorPort not to go looking
//  for hardware, and it is the difference between a miniport that loads and one
//  that is never called at all.  No network work happens here: HwInitialize is
//  the adapter start, and the geometry is reported through SCSI commands that
//  are answered from the namespace Identify taken there.
// ---------------------------------------------------------------------------
static ULONG NvmeofndFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation,
                                 PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                                 PBOOLEAN Again)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Again);

    NvmeofndTrace(L"TraceFindAdapter", 1);
    NvmeofndLogF("HwFindAdapter entered (IRQL %u)", (unsigned)KeGetCurrentIrql());

    // MINIMAL ConfigInfo, deliberately.  The previous version set a dozen fields from
    // memory of the sample, and the adapter start failed between HwFindAdapter and
    // HwInitialize with no indication which field was unacceptable.  StorPort fills in
    // what it wants for a virtual device; a miniport should state only what it must.
    ConfigInfo->VirtualDevice                  = TRUE;
    ConfigInfo->MaximumTransferLength          = 1024u * 1024u;
    ConfigInfo->MaximumNumberOfTargets         = NVMEOFND_MAX_TARGETS;
    ConfigInfo->MaximumNumberOfLogicalUnits    = NVMEOFND_MAX_LUNS;
    ConfigInfo->SrbType                        = SRB_TYPE_SCSI_REQUEST_BLOCK;
    ConfigInfo->AddressType                    = STORAGE_ADDRESS_TYPE_BTL8;
    // NumberOfBuses is what tells StorPort how many buses to scan for devices.  Leaving
    // it out (as the first minimal version did) gives ZERO buses, so the adapter runs and
    // is never asked for a single SCSI command - measured exactly that way.
    ConfigInfo->NumberOfBuses                = 1;

    NvmeofndTrace(L"TraceCfgVirtual",    ConfigInfo->VirtualDevice ? 1 : 0);
    NvmeofndTrace(L"TraceCfgMaxXfer",    ConfigInfo->MaximumTransferLength);
    NvmeofndTrace(L"TraceCfgMaxLuns",    ConfigInfo->MaximumNumberOfLogicalUnits);
    NvmeofndTrace(L"TraceCfgMaxTargets", ConfigInfo->MaximumNumberOfTargets);
    NvmeofndTrace(L"TraceFindAdapterDone", 1);
    return SP_RETURN_FOUND;
}

// ---------------------------------------------------------------------------
//  HwInitialize - the adapter start, and the ONE place the remote namespace is
//  brought up.  StorPort calls this at PASSIVE_LEVEL; if that is not the IRQL
//  this build sees, the bring-up is refused rather than blocking at DISPATCH.
//  The connection is open before BusChangeDetected is posted, so the first
//  READ CAPACITY Windows sends already carries the real remote size.
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndInitialize(PVOID DeviceExtension)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    NTSTATUS st;
    ULONG irql = KeGetCurrentIrql();

    NvmeofndTrace(L"TraceInitialize", 1);
    NvmeofndTrace(L"TraceInitIrql", irql);
    NvmeofndLogF("=== HwInitialize: bring up NVMe/TCP %s:%u (IRQL %u) ===",
                 "127.0.0.1", (unsigned)NVMEOFND_TARGET_PORT, (unsigned)irql);

    ext->BlockBytes = NVMEOFND_BLOCK_BYTES;
    ext->Ready      = FALSE;

    if (irql != PASSIVE_LEVEL) {
        // NvtcpOpen blocks for ~2 s when it fails and NvtcpCommand waits up to
        // 30 s: doing that at DISPATCH_LEVEL is how a machine looks frozen.
        NvmeofndLogF("!! HwInitialize at IRQL %u, not PASSIVE_LEVEL: refusing to open the"
                     " connection here and failing the adapter start instead", (unsigned)irql);
        ext->InitStatus = (ULONG)STATUS_INVALID_DEVICE_STATE;
        NvmeofndLogFlush();
        return FALSE;
    }

    st = NvmeofndEngineAttach(ext);
    ext->InitStatus = (ULONG)st;
    NvmeofndTrace(L"TraceEngineAttach", (ULONG)st);
    if (!NT_SUCCESS(st)) {
        NvmeofndLogF("!! bring-up failed 0x%08X: the adapter will not start and no LUN is"
                     " announced", (unsigned)st);
        NvmeofndPublishStats(DeviceExtension);
        NvmeofndLogFlush();
        return FALSE;
    }

    NvmeofndTrace(L"TraceBlocks", ext->Blocks);
    NvmeofndTrace(L"TraceBlockBytes", ext->BlockBytes);
    StorPortSetDeviceQueueDepth(DeviceExtension, 0, 0, 0, 32);
    ext->Ready = TRUE;

    // Announce the LUN: until this fires, the storage stack does not know the
    // device exists and nothing else in this file is ever called.
    // StorPort takes PathId, TargetId AND Lun here.  With only one argument the
    // other two are read off the stack, so the device at (0,0,0) is never announced and
    // StorPort never scans it - measured as a running adapter that receives no SCSI
    // command at all, only one WMI SRB.
    StorPortNotification(BusChangeDetected, DeviceExtension, 0, 0, 0);
    NvmeofndTrace(L"TraceBusChange", 1);

    NvmeofndLogF("adapter ready: %u blocks x %u bytes = %llu MiB from the remote namespace;"
                 " bus change announced", ext->Blocks, ext->BlockBytes,
                 ((ULONGLONG)ext->Blocks * ext->BlockBytes) >> 20);
    NvmeofndPublishStats(DeviceExtension);
    NvmeofndLogFlush();
    return TRUE;
}

// ---------------------------------------------------------------------------
//  HwStartIo - DISPATCH_LEVEL.  This function must never block, so it does not
//  touch the transport at all: it queues the SRB and returns TRUE, and the
//  PASSIVE worker completes it.  The only work done here is one spinlock, a
//  16-byte copy into a preallocated slot and an event set.
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    PNVMEOFND_ENGINE eng = ext->Eng;
    KIRQL irql;
    BOOLEAN pushed = FALSE;

    if (eng == NULL) {
        // Bring-up failed, so there is no engine and no worker.  Fail with sense
        // here rather than queueing a request nobody would ever complete.
        ext->TotalSrbs++;
        ext->NotReadyAnswers++;
        if (Srb->Function == SRB_FUNCTION_WMI) {
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
        } else if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI) {
            NvmeofndSetSense(DeviceExtension, Srb, NVMEOFND_SENSE_NOT_READY, 0x04, 0x02);
            Srb->SrbStatus = SRB_STATUS_ERROR;
        } else {
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        }
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    KeAcquireSpinLock(&eng->Lock, &irql);
    if (eng->Count < NVMEOFND_MAX_PENDING) {
        eng->Slot[eng->Tail].Srb = Srb;
        eng->Slot[eng->Tail].Ext = DeviceExtension;
        eng->Slot[eng->Tail].Stamp = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
        eng->Tail = (eng->Tail + 1) % NVMEOFND_MAX_PENDING;
        eng->Count++;
        pushed = TRUE;
    }
    KeReleaseSpinLock(&eng->Lock, irql);

    if (!pushed) {
        // The queue is full (64 requests): tell StorPort to come back later
        // instead of dropping the request on the floor.
        ext->TotalSrbs++;
        ext->QueueFullCount++;
        Srb->SrbStatus = SRB_STATUS_BUSY;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }
    KeSetEvent(&eng->Wake, IO_NO_INCREMENT, FALSE);

    /* The documented contract this driver never kept: tell StorPort it may hand
     * over the NEXT request right away.  Without NextRequest the port driver
     * falls back to a slow delivery path, which is exactly the signature the
     * phase timers found - the driver itself answers a 1 MiB command in
     * ~0.9 ms while the application waits 10-17 ms, the queue stamp shows
     * HwStartIo -> worker at 10 us (so the wait is BEFORE HwStartIo), and the
     * same uncached pattern runs at 400+ MB/s on the local NVMe.  The engine
     * has room for 64 slots while StorPort queues at 32, so this is always
     * true whenever a slot was pushed. */
    StorPortNotification(NextRequest, DeviceExtension);
    return TRUE;
}

// ---------------------------------------------------------------------------
//  Reset and adapter control.
//
//  HwResetBus deliberately does nothing but count: the transport has no reset
//  semantics, every command already has a bounded timeout, and completing SRBs
//  from a reset path that runs under StorPort's lock is how a miniport races its
//  own worker into a double completion.  A timeout here is answered with "bus
//  reset" so StorPort can recover, and the worker finishes the request it holds.
// ---------------------------------------------------------------------------
static BOOLEAN NvmeofndResetBus(PVOID DeviceExtension, ULONG PathId)
{
    PNVMEOFND_EXTENSION ext = (PNVMEOFND_EXTENSION)DeviceExtension;
    UNREFERENCED_PARAMETER(PathId);

    ext->OpCount[0] = 0;                    // no-op that keeps OpCount[0] meaningful (opcode 0)
    ext->ResetBusCount++;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        NvmeofndLogF("HwResetBus(path %u) called; the worker keeps its current request"
                     " and the transport has no reset primitive", (unsigned)PathId);
        NvmeofndLogFlush();
    }
    return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS NvmeofndAdapterControl(PVOID DeviceExtension,
                                                          SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                                          PVOID Parameters)
{
    NvmeofndTrace(L"TraceAdapterControl", (ULONG)ControlType);

    // ScsiQuerySupportedControlTypes is a REQUIRED handshake, not a courtesy call.
    // StorPort asks which control types the miniport handles and the miniport must
    // ANSWER by writing into the caller's list.  Returning success while leaving
    // that list untouched makes the adapter fail to start - measured here as problem
    // 10 with HwFindAdapter completing and HwInitialize never being reached.
    //
    // Every entry is left FALSE on purpose: in particular ScsiStopAdapter is NOT
    // claimed, so StorPort never calls back into this driver while tearing the
    // adapter down.  There is nothing to release (the engine is never freed and
    // no unload path exists), so the safest stop path is the one that does not
    // call here at all.
    if (ControlType == ScsiQuerySupportedControlTypes) {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        ULONG i;
        if (list == NULL) {
            return ScsiAdapterControlUnsuccessful;
        }
        NvmeofndTrace(L"TraceCtlListMax", list->MaxControlType);
        for (i = 0; i < list->MaxControlType; i++) {
            list->SupportedTypeList[i] = FALSE;
        }
        return ScsiAdapterControlSuccess;
    }

    UNREFERENCED_PARAMETER(DeviceExtension);
    return ScsiAdapterControlSuccess;
}

// ---------------------------------------------------------------------------
//  DriverEntry
//
//  WSK registration lives here because it belongs to the DRIVER's lifetime, not
//  the adapter's: WskDeregister must not be called on a registration that never
//  succeeded, and a live registration must not outlive the image.  Capturing the
//  provider NPI and opening the socket is HwInitialize's job (PASSIVE as well,
//  but that is the adapter start and it is where the namespace geometry is
//  learned).
//
//  Failure paths here DO clean up, because DriverEntry runs in a normal system
//  thread and is not the unload path.
// ---------------------------------------------------------------------------
NTSTATUS DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    NTSTATUS st;
    HW_INITIALIZATION_DATA hw = {};

    g_startTime = KeQueryInterruptTime();
    NvmeofndTrace(L"TraceDriverEntry", 1);
    NvmeofndLogF("=== nvmeofnd2: StorPort virtual miniport over a REMOTE NVMe/TCP namespace"
                 " (target 127.0.0.1:%u, nsid %u) ===",
                 (unsigned)NVMEOFND_TARGET_PORT, (unsigned)NVMEOFND_NSID);

    // No DriverUnload is installed, here or anywhere else: this driver is
    // designed to be loaded once and left loaded until reboot.
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        NvmeofndLogF("!! DriverEntry at IRQL %u: WskRegister would be illegal, failing",
                     (unsigned)KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    g_wskDispatch.Version      = MAKE_WSK_VERSION(1, 0);
    g_wskDispatch.Reserved     = 0;
    g_wskDispatch.WskClientEvent = NULL;      // no event callbacks are used
    g_wskClient.ClientContext  = NULL;
    g_wskClient.Dispatch       = &g_wskDispatch;

    st = WskRegister(&g_wskClient, &g_registration);
    NvmeofndTrace(L"TraceWskRegister", (ULONG)st);
    NvmeofndLogF("WskRegister = 0x%08X", (unsigned)st);
    NvmeofndLogFlush();
    if (!NT_SUCCESS(st)) {
        // Nothing was registered, so there is nothing to deregister and nothing
        // else in this driver can work: fail the load and leave no state behind.
        NvmeofndLogF("!! WskRegister failed: the load is refused (no WSK, no transport)");
        NvmeofndLogFlush();
        return st;
    }
    g_registered = TRUE;

    hw.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    hw.AdapterInterfaceType     = Internal;          // virtual: no bus, no hardware
    hw.HwInitialize             = NvmeofndInitialize;
    hw.HwStartIo                = NvmeofndStartIo;
    hw.HwFindAdapter            = NvmeofndFindAdapter;
    hw.HwResetBus               = NvmeofndResetBus;
    hw.HwAdapterControl         = NvmeofndAdapterControl;
    hw.DeviceExtensionSize      = sizeof(NVMEOFND_EXTENSION);
    hw.SpecificLuExtensionSize  = 0;
    hw.SrbExtensionSize         = 0;
    hw.NumberOfAccessRanges     = 0;
    hw.MapBuffers               = STOR_MAP_ALL_BUFFERS;
    hw.NeedPhysicalAddresses    = FALSE;
    hw.TaggedQueuing            = TRUE;
    hw.AutoRequestSense         = TRUE;
    hw.MultipleRequestPerLu     = TRUE;
    hw.ReceiveEvent             = FALSE;
    hw.PortVersionFlags         = 0;

    st = StorPortInitialize(DriverObject, RegistryPath, &hw, NULL);
    NvmeofndTrace(L"TraceStorPortInitRc", (ULONG)st);
    NvmeofndTrace(L"TraceHwStructSize", (ULONG)hw.HwInitializationDataSize);
    NvmeofndLogF("StorPortInitialize = 0x%08X (HW_INITIALIZATION_DATA %u bytes)",
                 (unsigned)st, (unsigned)hw.HwInitializationDataSize);
    NvmeofndLogFlush();

    if (!NT_SUCCESS(st)) {
        // StorPort refused the driver, so no adapter will ever start: release the
        // WSK registration instead of leaving a live callback into an image that
        // is about to unload.
        NvmeofndLogF("!! StorPortInitialize failed; deregistering WSK and failing the load");
        NvmeofndLogFlush();
        if (g_npiCaptured) {
            WskReleaseProviderNPI(&g_registration);
            g_npiCaptured = FALSE;
        }
        WskDeregister(&g_registration);
        g_registered = FALSE;
    }
    return st;
}











