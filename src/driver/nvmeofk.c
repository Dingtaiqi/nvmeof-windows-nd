// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ===========================================================================
//  nvmeofk.sys - D2.1: NVMe-oF **in the kernel**.
//
//  D0 proved the kernel NDK path (WSK -> NDKD -> adapter -> PD/CQ/QP ->
//  connect -> a 4096-byte RDMA Read with zero differing bytes).  D1 proved the
//  storage side (a StorPort virtual miniport that Windows accepts as a disk with
//  a drive letter).  This driver is the join: the NVMe-oF protocol itself, spoken
//  from kernel mode, with no user-mode component anywhere in the path.
//
//  What it does, in the order a real host does it:
//     1. open the NDK adapter on the RoCE interface
//     2. connect an RC queue pair to the target's port, carrying the SAME
//        private data the Linux host sends (32 bytes; nvmet rejects a Connect
//        with none, and rejects hsqsize = depth instead of depth - 1)
//     3. Fabrics Connect on qid 0 - this is what creates the controller
//     4. Property Get CAP, so the answer proves an admin command round-tripped
//  Every step's result is written to the registry, because a kernel driver has
//  no stdout.
//
//  The wire details are not recalled, they are copied from this project's
//  user-mode implementation (src/nvmeof_wire.h), which is interop-proven against
//  both Linux nvmet and our own target:
//     capsule[1] = 0x40                       PSDT, checked on EVERY command
//     capsule[24..39] = keyed SGL             16 bytes: addr, 24-bit len, rkey,
//                                             type_subtype = 0x40
//     connect data 1024 bytes                 hostid[16], cntlid@16,
//                                             subnqn@256, hostnqn@512
//     private data 32 bytes                   recfmt, qid, hrqsize, hsqsize,
//                                             cntlid
// ===========================================================================

#include <ntddk.h>
#include <ntstrsafe.h>
#include <ndkpi.h>
#include <wsk.h>
#include <wskndk.h>

#define NVMEOFK_TAG 'kfon'          // "nfok" reversed

// ---- fabric / protocol constants (from src/nvmeof_wire.h) -----------------
#define NVMEOF_OPC_FABRICS        0x7F
#define NVMEOF_FCTYPE_CONNECT     0x01
#define NVMEOF_FCTYPE_PROP_GET    0x04
#define NVMEOF_CMD_FLAGS_METABUF  0x40      // PSDT = 0b01, mandatory
#define NVMEOF_SGL_KEYED_TS       0x40      // keyed data block, address subtype
#define NVMEOF_CONNECT_DATA_SIZE  1024
#define NVMEOF_CNTLID_DYNAMIC     0xFFFF
#define NVMEOF_KATO_DEFAULT       120000
#define NVMEOF_PROP_CAP           0x00
#define NVMEOF_STATUS_MASK        0x7FFE    // bits 1..13 of the 16-bit status word
#define NVMEOF_STATUS_SHIFT       1

#define NVMEOFK_SQ_DEPTH          32        // must be <= NVME_AQ_DEPTH(32) when +1 is applied
#define NVMEOFK_CAPSULE_BYTES     64
#define NVMEOFK_RESPONSE_BYTES    16
#define NVMEOFK_CONNECT_OFF       0
#define NVMEOFK_CAP_OFF           1024
#define NVMEOFK_RESP_OFF          1152
#define NVMEOFK_REGION_BYTES      4096

#define NVMEOFK_DEFAULT_PORT      4420
#define NVMEOFK_DEFAULT_TARGET    0x00000002u   // 192.168.100.2, network order applied below
#define NVMEOFK_DEFAULT_IFINDEX   37u           // 以太网 24, the RoCE endpoint (from D0's measurements)

static const char kSubsysNqn[] = "nqn.2024-01.local.rdma:windows-nd";
static const char kHostNqn[]   = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-kernel-0001";

// ---------------------------------------------------------------------------
//  Logging: a kernel driver's only output is the registry.
// ---------------------------------------------------------------------------
static char   g_log[8192];
static size_t g_len = 0;

// A tiny formatter instead of the RtlStringCbVPrintf family: in kernel mode the
// ANSI printf helpers pull UCRT symbols (__stdio_common_vsprintf) that a driver
// cannot link, and libcntpr does not provide them either.  Only what this file
// actually uses is supported: %s %u %d %x %X %p %c %%.
static void logRaw(const char* s, size_t n)
{
    if (g_len + n + 3 >= sizeof(g_log)) {
        return;
    }
    RtlCopyMemory(g_log + g_len, s, n);
    g_len += n;
}

static void logNum(unsigned long long v, int base, int width, int upper, int padZero)
{
    char tmp[24];
    int i = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0) {
        tmp[i++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    while (i < width) {
        tmp[i++] = padZero ? '0' : ' ';
    }
    while (i > 0) {
        logRaw(&tmp[--i], 1);
    }
}

static void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char* p = fmt; *p != 0; p++) {
        int width = 0, padZero = 0, longs = 0;
        if (*p != '%') {
            logRaw(p, 1);
            continue;
        }
        p++;
        if (*p == '0') { padZero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { longs++; p++; }
        switch (*p) {
        case 'u':
            logNum((longs >= 2) ? va_arg(ap, unsigned long long)
                                : (unsigned long long)va_arg(ap, unsigned int), 10, width, 0, padZero);
            break;
        case 'd': {
            int iv = va_arg(ap, int);
            if (iv < 0) { logRaw("-", 1); iv = -iv; }
            logNum((unsigned long long)iv, 10, width, 0, padZero);
            break;
        }
        case 'X':
            logNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 1, padZero);
            break;
        case 'x':
            logNum((unsigned long long)va_arg(ap, unsigned int), 16, width, 0, padZero);
            break;
        case 'p':
            logRaw("0x", 2);
            logNum((unsigned long long)(ULONG_PTR)va_arg(ap, PVOID), 16, 16, 0, 1);
            break;
        case 's': {
            const char* sv = va_arg(ap, const char*);
            if (sv == NULL) { sv = "(null)"; }
            while (*sv != 0) { logRaw(sv, 1); sv++; }
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            logRaw(&cv, 1);
            break;
        }
        case '%':
            logRaw("%", 1);
            break;
        default:
            logRaw("%", 1);
            logRaw(p, 1);
            break;
        }
    }
    va_end(ap);
    logRaw("\r\n", 2);
}

static void writeLog(void)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp = 0;

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, &disp))) {
        return;
    }
    RtlInitUnicodeString(&valueName, L"nvmeofk_log");
    ZwSetValueKey(key, &valueName, 0, REG_SZ, g_log, (ULONG)g_len + 1);

    RtlInitUnicodeString(&valueName, L"nvmeofk_ok");
    {
        ULONG ok = 1;
        ZwSetValueKey(key, &valueName, 0, REG_DWORD, &ok, sizeof(ok));
    }
    ZwClose(key);
}

static void traceValue(PCWSTR name, ULONG v)
{
    UNICODE_STRING keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, 0, NULL))) {
        return;
    }
    RtlInitUnicodeString(&valueName, name);
    ZwSetValueKey(key, &valueName, 0, REG_DWORD, &v, sizeof(v));
    ZwClose(key);
}

// ---------------------------------------------------------------------------
//  The NDK end: everything the adapter hands us lives here.
// ---------------------------------------------------------------------------
typedef struct _NVMEOFK_ASYNC {
    KEVENT   done;
    NTSTATUS status;
    PVOID    object;
    ULONG    transferred;
    // THE ROOT CAUSE OF THE 0xA, found in the minidump rather than by guessing:
    // StepStart() called KeInitializeEvent() on every single use.  Reinitialising a
    // dispatcher object that is still linked into a wait list corrupts that list.
    // The dump showed exactly that: rsi pointed at g_async+0x30, the event's
    // WaitListHead.Flink and .Blink both held a stale ffffe48a2de0e180 instead of
    // pointing back at themselves, and the kernel, walking that list, executed
    // nt+0x25b58f "mov r12,[r12]" with r12 = 0 at DISPATCH_LEVEL -> 0xA.
    // So now: initialise ONCE, and afterwards only ever KeResetEvent, which changes
    // SignalState and never touches the wait list.
    BOOLEAN  initialized;
    BOOLEAN  dead;          // an operation that timed out may still complete: never trust it again
} NVMEOFK_ASYNC;

static WSK_REGISTRATION           g_registration;
static WSK_CLIENT_NPI             g_wskClient;
static WSK_CLIENT_DISPATCH        g_wskDispatch;
static WSK_PROVIDER_NPI           g_wskProvider;
static WSK_PROVIDER_NDK_DISPATCH  g_ndk;
// STATIC ON PURPOSE: these calls are asynchronous and can complete after the enclosing
// frame is gone.  A stack context here is what bugchecked D0 with 0x7E (the provider wrote
// into dead stack).  Every async call in this file uses this one context.
static NVMEOFK_ASYNC g_async;
static NVMEOFK_ASYNC g_connectCtx;   // separate contexts: sharing one across overlapping
static NVMEOFK_ASYNC g_sendCtx;      // asynchronous operations is how a completion ends up writing
static NVMEOFK_ASYNC g_recvCtx;      // into a context that is already in use by another call.

static NDK_ADAPTER*   g_adapter;
static NDK_PD*        g_pd;
static NDK_CQ*        g_cq;
static NDK_QP*        g_qp;
static NDK_CONNECTOR* g_connector;
static NDK_MR*        g_mr;
static MDL*           g_mdl;
static PVOID          g_region;
static UINT32         g_lkey;
static UINT32         g_rkey;

static KEVENT g_cqEvent;
static KSPIN_LOCK g_cqLock;
static NDK_RESULT g_cqResults[16];
static volatile LONG g_cqCount;
static volatile LONG g_cqDropped;

// The D0 wait helpers: start the context, then wait only when the provider said
// STATUS_PENDING (a synchronous success is normal and must not be waited on).
static void StepStart(NVMEOFK_ASYNC* a)
{
    if (!a->initialized) {
        KeInitializeEvent(&a->done, NotificationEvent, FALSE);
        a->initialized = TRUE;
    } else {
        if (a->dead) {
            // Signalling this event late is harmless.  The hazard was reinitialising
            // it, and that no longer happens.
            logf("!! async context %p was left dead by a timeout and is being reused", a);
        }
        KeResetEvent(&a->done);      // SignalState only - never rewrites the wait list
    }
    a->status = STATUS_PENDING;
    a->object = NULL;
    a->transferred = 0;
}

static NTSTATUS StepFinish(NTSTATUS status, NVMEOFK_ASYNC* a, ULONG waitMs)
{
    LARGE_INTEGER t;
    if (status != STATUS_PENDING) {
        return status;
    }
    t.QuadPart = -((LONGLONG)waitMs * 10000);
    if (KeWaitForSingleObject(&a->done, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
        a->dead = TRUE;
        logf("!! StepFinish timed out after %u ms; context %p marked dead", waitMs, a);
        return STATUS_IO_TIMEOUT;
    }
    return a->status;
}

static void complete(PVOID context, NTSTATUS status, NDK_OBJECT_HEADER* obj)
{
    NVMEOFK_ASYNC* a = (NVMEOFK_ASYNC*)context;
    if (a == NULL) {
        return;
    }
    a->status = status;
    a->object = obj;
    KeSetEvent(&a->done, IO_NO_INCREMENT, FALSE);
}

static void requestComplete(PVOID context, NTSTATUS status)
{
    NVMEOFK_ASYNC* a = (NVMEOFK_ASYNC*)context;
    if (a == NULL) {
        return;
    }
    a->status = status;
    KeSetEvent(&a->done, IO_NO_INCREMENT, FALSE);
}

static void cqNotification(PVOID context, NTSTATUS status)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(status);
    // NOT decoration: reap() only reads the CQ when this counter says there is
    // something in it.  The first version never incremented it, so completions were
    // never consumed at all.
    InterlockedIncrement(&g_cqCount);
    KeSetEvent(&g_cqEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS wait(NVMEOFK_ASYNC* a, ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -((LONGLONG)ms * 10000);
    if (KeWaitForSingleObject(&a->done, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
        return STATUS_IO_TIMEOUT;
    }
    return a->status;
}

// Drain the CQ, remembering any completion the caller is not waiting for yet.
static NTSTATUS reap(ULONG wantCtx, ULONG ms, ULONG* bytesOut)
{
    LARGE_INTEGER t;
    ULONG spin = 0;

    for (;;) {
        LONG have = InterlockedExchange(&g_cqCount, 0);
        if (have > 0) {
            ULONG got = g_cq->Dispatch->NdkGetCqResults(g_cq, g_cqResults, (ULONG)(have > 16 ? 16 : have));
            for (ULONG i = 0; i < got && i < (ULONG)(sizeof(g_cqResults)/sizeof(g_cqResults[0])); i++) {   // never trust the count past our own array
                NDK_RESULT* r = &g_cqResults[i];
                if ((ULONG)(ULONG_PTR)r->RequestContext == wantCtx) {
                    if (bytesOut != NULL) {
                        *bytesOut = r->BytesTransferred;
                    }
                    traceValue(L"nvmeofk_lastcq", r->Status);
                    return r->Status;
                }
                InterlockedIncrement(&g_cqDropped);
            }
        }
        spin++;
        if (spin > (ms * 4)) {
            return STATUS_IO_TIMEOUT;
        }
        t.QuadPart = -10000;        // 1 ms
        KeWaitForSingleObject(&g_cqEvent, Executive, KernelMode, FALSE, &t);
    }
}

// ---------------------------------------------------------------------------
//  Bring-up
// ---------------------------------------------------------------------------
static NTSTATUS openAdapter(void)
{
    NTSTATUS st;
    ULONG ifIndex = 0;
    NDK_VERSION ver;
    NDK_ADAPTER_INFO info;

    g_wskDispatch.Version = MAKE_WSK_VERSION(1, 0);
    g_wskDispatch.Reserved = 0;
    g_wskDispatch.WskClientEvent = NULL;
    g_wskClient.ClientContext = NULL;
    g_wskClient.Dispatch = &g_wskDispatch;

    st = WskRegister(&g_wskClient, &g_registration);
    if (!NT_SUCCESS(st)) {
        logf("WskRegister failed 0x%08X", st);
        return st;
    }
    st = WskCaptureProviderNPI(&g_registration, WSK_INFINITE_WAIT, &g_wskProvider);
    if (!NT_SUCCESS(st)) {
        logf("WskCaptureProviderNPI failed 0x%08X", st);
        return st;
    }

    // The NDK provider comes through the WSK client-control path under the name
    // WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH.  What that call wants in the output
    // buffer is the STRUCT, so the length is sizeof(struct) - passing sizeof(pointer)
    // makes the provider answer STATUS_INVALID_PARAMETER (0xC000000D), and that cost
    // real time in D0.
    RtlZeroMemory(&g_ndk, sizeof(g_ndk));
    st = g_wskProvider.Dispatch->WskControlClient(g_wskProvider.Client,
                                                  WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH,
                                                  0, NULL, sizeof(g_ndk), &g_ndk, NULL, NULL);
    logf("WskControlClient('NDKD') = 0x%08X", st);
    if (!NT_SUCCESS(st) || g_ndk.WskOpenNdkAdapter == NULL) {
        return NT_SUCCESS(st) ? STATUS_INVALID_DEVICE_STATE : st;
    }

    // Interface index: from the registry when present, otherwise the RoCE endpoint
    // this machine actually runs (192.168.100.2 = 以太网 24).
    {
        UNICODE_STRING vn, kn;
        OBJECT_ATTRIBUTES oa;
        HANDLE key = NULL;
        UCHAR buf[24];
        ULONG len = sizeof(buf);

        ifIndex = NVMEOFK_DEFAULT_IFINDEX;
        RtlInitUnicodeString(&kn, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
        InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
            RtlInitUnicodeString(&vn, L"nvmeofk_ifindex");
            if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
                KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
                if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                    ifIndex = *(ULONG*)p->Data;
                    logf("ifIndex from registry: %u", ifIndex);
                }
            }
            ZwClose(key);
        }
    }

    RtlZeroMemory(&ver, sizeof(ver));
    ver.Major = 2;
    ver.Minor = 0;
    logf("NDK version 0x%08X  ifIndex %u", ver, ifIndex);

    st = g_ndk.WskOpenNdkAdapter(g_wskProvider.Client, ver, (NET_IFINDEX)ifIndex, &g_adapter);
    if (!NT_SUCCESS(st) || g_adapter == NULL) {
        logf("WskOpenNdkAdapter(ifIndex=%u, ver=0x%08X) failed 0x%08X adapter=%p",
             ifIndex, ver, st, g_adapter);
        return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    }
    RtlZeroMemory(&info, sizeof(info));
    // (adapter info is not consulted: the registry supplies what we need)
    logf("NdkOpenAdapter ok: ifIndex=%u infoStatus=0x%08X adapter=%p", ifIndex, st, g_adapter);
    traceValue(L"nvmeofk_ifindex", ifIndex);
    traceValue(L"nvmeofk_ndkver", (ULONG)ver.Major);
    return STATUS_SUCCESS;
}
static NTSTATUS createObjects(void)
{
    NTSTATUS st;

    KeInitializeEvent(&g_cqEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&g_cqLock);

    StepStart(&g_async);
    st = g_adapter->Dispatch->NdkCreatePd(g_adapter, complete, &g_async, &g_pd);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (NT_SUCCESS(st) && g_pd == NULL) { g_pd = (NDK_PD*)g_async.object; }
    logf("NdkCreatePd        = 0x%08X pd=%p", st, g_pd);
    if (!NT_SUCCESS(st) || g_pd == NULL) { return STATUS_UNSUCCESSFUL; }

    StepStart(&g_async);
    st = g_adapter->Dispatch->NdkCreateCq(g_adapter, 64, cqNotification, &g_cqCount, NULL,
                                          complete, &g_async, &g_cq);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (NT_SUCCESS(st) && g_cq == NULL) { g_cq = (NDK_CQ*)g_async.object; }
    logf("NdkCreateCq        = 0x%08X cq=%p", st, g_cq);
    if (!NT_SUCCESS(st) || g_cq == NULL) { return STATUS_UNSUCCESSFUL; }

    // NdkCreateQp lives on the PD dispatch, not the adapter's, and takes the
    // queue depths and SGE counts as well: (pd, recvCq, initCq, srq, recvDepth,
    // initDepth, recvSge, initSge, inlineData, complete, ctx, &qp).
    StepStart(&g_async);
    st = g_pd->Dispatch->NdkCreateQp(g_pd, g_cq, g_cq, NULL, 16, 16, 4, 4, 0,
                                     complete, &g_async, &g_qp);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (NT_SUCCESS(st) && g_qp == NULL) { g_qp = (NDK_QP*)g_async.object; }
    logf("NdkCreateQp        = 0x%08X qp=%p", st, g_qp);
    if (!NT_SUCCESS(st) || g_qp == NULL) { return STATUS_UNSUCCESSFUL; }

    StepStart(&g_async);
    st = g_adapter->Dispatch->NdkCreateConnector(g_adapter, complete, &g_async, &g_connector);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (NT_SUCCESS(st) && g_connector == NULL) { g_connector = (NDK_CONNECTOR*)g_async.object; }
    logf("NdkCreateConnector = 0x%08X connector=%p", st, g_connector);
    if (!NT_SUCCESS(st) || g_connector == NULL) { return STATUS_UNSUCCESSFUL; }

    return STATUS_SUCCESS;
}
static NTSTATUS registerRegion(void)
{
    NTSTATUS st;

    g_region = ExAllocatePoolWithTag(NonPagedPoolNx, NVMEOFK_REGION_BYTES, NVMEOFK_TAG);
    if (g_region == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g_region, NVMEOFK_REGION_BYTES);

    g_mdl = IoAllocateMdl(g_region, NVMEOFK_REGION_BYTES, FALSE, FALSE, NULL);
    if (g_mdl == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    // The buffer is non-paged pool, so the MDL describes it directly.
    // D0 - the kernel driver in this project that never bugchecked - used
    // MmProbeAndLockPages on a non-paged pool region.  This file used
    // MmBuildMdlForNonPagedPool.  Both are documented, but only one of them has been
    // proven on this machine against this NDK provider, so match the proven one.
    __try {
        MmProbeAndLockPages(g_mdl, KernelMode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("!! MmProbeAndLockPages raised 0x%08X", GetExceptionCode());
        IoFreeMdl(g_mdl);
        g_mdl = NULL;
    }

    StepStart(&g_async);
    st = g_pd->Dispatch->NdkCreateMr(g_pd, FALSE, complete, &g_async, &g_mr);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (NT_SUCCESS(st) && g_mr == NULL) { g_mr = (NDK_MR*)g_async.object; }
    logf("NdkCreateMr        = 0x%08X mr=%p", st, g_mr);
    if (!NT_SUCCESS(st) || g_mr == NULL) { return STATUS_UNSUCCESSFUL; }

    // The flags are a promise the provider enforces, and D0 measured the price of
    // getting them wrong: with 0, the peer's RDMA access to this region fails with
    // 0xC0000005 and transfers 0 bytes.  The target must be able to READ the
    // Connect data and the Identify buffer we hand it through a keyed SGL.
    StepStart(&g_async);
    // A SINGLE flag - these are ENUMERATED VALUES, not bit flags.  Note that
    // NDK_MR_FLAG_ALLOW_REMOTE_WRITE is 0x5 rather than 0x4, so OR-ing them
    // together (which this line did for several rounds: 0x1|0x2|0x5|0x8 = 0xF)
    // hands the provider a value it never defined.  D0 - the kernel driver in
    // this project that actually completed an RDMA read on this hardware -
    // passed exactly one flag, and this is that flag: we are the destination of
    // an RDMA read, so the HCA writes into our region.
    st = g_mr->Dispatch->NdkRegisterMr(g_mr, g_mdl, NVMEOFK_REGION_BYTES,
            NDK_MR_FLAG_ALLOW_LOCAL_WRITE,
            requestComplete, &g_async);
    st = StepFinish(st, &g_async, 10000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkRegisterMr(%u) = 0x%08X", NVMEOFK_REGION_BYTES, st);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    if (!NT_SUCCESS(st)) { return st; }

    g_rkey = g_mr->Dispatch->NdkGetRemoteTokenFromMr(g_mr);
    g_lkey = g_mr->Dispatch->NdkGetLocalTokenFromMr(g_mr);

    logf("region %p registered: rkey=0x%08X lkey=0x%08X (remote read+write allowed)",
         g_region, g_rkey, g_lkey);
    traceValue(L"nvmeofk_rkey", g_rkey);
    traceValue(L"nvmeofk_lkey", g_lkey);
    return STATUS_SUCCESS;
}
// 32-byte NVMe-oF RDMA private data, filled the way the Linux host fills it.
static void fillPrivateData(UCHAR* pd, USHORT qid, USHORT depth, USHORT cntlid)
{
    RtlZeroMemory(pd, 32);
    *(USHORT*)(pd + 0) = 0;                     // recfmt = NVME_RDMA_CM_FMT_1_0
    *(USHORT*)(pd + 2) = qid;
    *(USHORT*)(pd + 4) = depth;                 // our receive queue
    *(USHORT*)(pd + 6) = (USHORT)(depth - 1);   // 0-based, as Linux sends it
    *(USHORT*)(pd + 8) = cntlid;
}

// forward declaration: the call site sits above the definition
static void closeConnectorNow(void);

static NTSTATUS connectToTarget(void)
{
    NTSTATUS st;
    UCHAR pd[32];
    SOCKADDR_IN local, remote;
    ULONG peerLimitOut = 0, peerLimitIn = 0;
    UCHAR peerData[64];
    ULONG peerDataLen = sizeof(peerData);

    RtlZeroMemory(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;                 // 0 = let the provider allocate the endpoint

    RtlZeroMemory(&remote, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = RtlUshortByteSwap(NVMEOFK_DEFAULT_PORT);
    remote.sin_addr.S_un.S_addr = NVMEOFK_DEFAULT_TARGET;
    // nvmeofk_target / nvmeofk_local are REG_DWORDs holding addresses in NETWORK byte
    // order, so the peer can be changed without rebuilding the driver.
    {
        UNICODE_STRING kn, vn;
        OBJECT_ATTRIBUTES oa;
        HANDLE key = NULL;
        UCHAR buf[24];
        ULONG len = sizeof(buf);

        RtlInitUnicodeString(&kn, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
        InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
            RtlInitUnicodeString(&vn, L"nvmeofk_target");
            if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
                KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
                if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                    remote.sin_addr.S_un.S_addr = *(ULONG*)p->Data;
                }
            }
            len = sizeof(buf);
            RtlInitUnicodeString(&vn, L"nvmeofk_local");
            if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
                KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
                if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                    local.sin_addr.S_un.S_addr = *(ULONG*)p->Data;
                }
            }
            ZwClose(key);
        }
    }
    logf("connecting to %u.%u.%u.%u:%u (local %u.%u.%u.%u)",
         ((PUCHAR)&remote.sin_addr)[0], ((PUCHAR)&remote.sin_addr)[1],
         ((PUCHAR)&remote.sin_addr)[2], ((PUCHAR)&remote.sin_addr)[3], NVMEOFK_DEFAULT_PORT,
         ((PUCHAR)&local.sin_addr)[0], ((PUCHAR)&local.sin_addr)[1],
         ((PUCHAR)&local.sin_addr)[2], ((PUCHAR)&local.sin_addr)[3]);

    fillPrivateData(pd, 0, NVMEOFK_SQ_DEPTH, 0);

    // NdkConnect takes the completion routine directly, and NDKPI has NO bind step: a
    // connector has its own implicit local endpoint, and D0 found that port 0 is what
    // makes the provider allocate it.
    StepStart(&g_connectCtx);
    st = g_connector->Dispatch->NdkConnect(g_connector, g_qp,
                                           (CONST PSOCKADDR)&local, sizeof(local),
                                           (CONST PSOCKADDR)&remote, sizeof(remote),
                                           1, 1,
                                           pd, sizeof(pd),
                                           requestComplete, &g_connectCtx);
    st = StepFinish(st, &g_connectCtx, 20000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkConnect         = 0x%08X (0xC0000236 = connection refused)", st);
    if (!NT_SUCCESS(st)) {
        traceValue(L"nvmeofk_connect", (ULONG)st);
        closeConnectorNow();   // do not leave a failed connection dangling
        return st;
    }

    // pPrivateDataLength is IN/OUT: on input it MUST hold the size of the buffer.
    // It was passed uninitialised, so the provider wrote a garbage number of bytes
    // into this 64-byte stack buffer - which is exactly the "incorrect stack"
    // (0x139, P1=0xa) that took the machine down four times, and why the kernel log
    // was always empty: the log is written at the end, and the stack was already
    // destroyed by then.
    peerDataLen = sizeof(peerData);
    st = g_connector->Dispatch->NdkGetConnectionData(g_connector, &peerLimitIn, &peerLimitOut,
                                                     peerData, &peerDataLen);
    logf("NdkGetConnectionData = 0x%08X peer inbound=%u outbound=%u dataLen=%u recfmt=%u crqsize=%u",
         st, peerLimitIn, peerLimitOut, peerDataLen,
         (peerDataLen >= 2) ? *(USHORT*)peerData : 0,
         (peerDataLen >= 4) ? *(USHORT*)(peerData + 2) : 0);

    StepStart(&g_connectCtx);
    st = g_connector->Dispatch->NdkCompleteConnect(g_connector, NULL, NULL, requestComplete, &g_connectCtx);
    st = StepFinish(st, &g_connectCtx, 20000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkCompleteConnect = 0x%08X", st);
    traceValue(L"nvmeofk_connect", (ULONG)st);
    return st;
}
#define CTX_SEND 0x1001
#define CTX_RECV 0x1002

static void sglKeyed(UCHAR* sgl, ULONGLONG addr, ULONG len, UINT32 key)
{
    RtlZeroMemory(sgl, 16);
    *(ULONGLONG*)(sgl + 0) = addr;
    sgl[8] = (UCHAR)(len & 0xFF);
    sgl[9] = (UCHAR)((len >> 8) & 0xFF);
    sgl[10] = (UCHAR)((len >> 16) & 0xFF);      // 24-bit length
    *(UINT32*)(sgl + 11) = key;
    sgl[15] = NVMEOF_SGL_KEYED_TS;              // keyed data block, address subtype
}

// A command with no data still carries a keyed descriptor with length 0: the
// target's RDMA transport switches on that byte for EVERY command and answers
// "invalid SGL subtype: 0x0" (Invalid Field) to a zeroed dptr.
static void sglNull(UCHAR* sgl)
{
    RtlZeroMemory(sgl, 16);
    sgl[15] = NVMEOF_SGL_KEYED_TS;
}

static NTSTATUS submitCapsule(const UCHAR* capsule, UCHAR* responseOut, ULONG timeoutMs)
{
    NTSTATUS st;
    NDK_SGE sge;
    UCHAR* region = (UCHAR*)g_region;

    RtlCopyMemory(region + NVMEOFK_CAP_OFF, capsule, NVMEOFK_CAPSULE_BYTES);
    RtlZeroMemory(region + NVMEOFK_RESP_OFF, NVMEOFK_RESPONSE_BYTES);

    // 1. post the receive for the 16-byte response
    sge.VirtualAddress = region + NVMEOFK_RESP_OFF;
    sge.Length = NVMEOFK_RESPONSE_BYTES;
    sge.MemoryRegionToken = g_lkey;
    st = g_qp->Dispatch->NdkReceive(g_qp, (PVOID)(ULONG_PTR)(PVOID)(ULONG_PTR)CTX_RECV, &sge, 1);
    if (!NT_SUCCESS(st)) { logf("NdkReceive failed 0x%08X", st); return st; }

    // 2. send the 64-byte capsule
    sge.VirtualAddress = region + NVMEOFK_CAP_OFF;
    sge.Length = NVMEOFK_CAPSULE_BYTES;
    sge.MemoryRegionToken = g_lkey;
    st = g_qp->Dispatch->NdkSend(g_qp, (PVOID)(ULONG_PTR)CTX_SEND, &sge, 1, 0);
    if (!NT_SUCCESS(st)) { logf("NdkSend failed 0x%08X", st); return st; }

    st = reap(CTX_SEND, timeoutMs, NULL);
    if (!NT_SUCCESS(st)) { logf("send completion 0x%08X", st); return st; }

    st = reap(CTX_RECV, timeoutMs, NULL);
    if (!NT_SUCCESS(st)) { logf("response completion 0x%08X", st); return st; }

    if (responseOut != NULL) {
        RtlCopyMemory(responseOut, region + NVMEOFK_RESP_OFF, NVMEOFK_RESPONSE_BYTES);
    }
    return STATUS_SUCCESS;
}

static USHORT responseStatus(const UCHAR* resp)
{
    // 16-byte RDMA response: status is byte 14, and the 15-bit status field starts
    // at bit 1 of the 16-bit little-endian word at offset 14.
    USHORT raw = *(USHORT*)(resp + 14);
    return (USHORT)((raw & NVMEOF_STATUS_MASK) >> NVMEOF_STATUS_SHIFT);
}

// ---------------------------------------------------------------------------
//  The sequence
// ---------------------------------------------------------------------------
static NTSTATUS nvmeofRun(void)
{
    NTSTATUS st;
    UCHAR capsule[NVMEOFK_CAPSULE_BYTES];
    UCHAR response[NVMEOFK_RESPONSE_BYTES];
    UCHAR* region = (UCHAR*)g_region;
    USHORT cid = 0;

    // ---- Fabrics Connect: this is what creates the controller -------------
    {
        UCHAR* cd = region + NVMEOFK_CONNECT_OFF;
        RtlZeroMemory(cd, NVMEOF_CONNECT_DATA_SIZE);
        for (int i = 0; i < 16; i++) {
            cd[i] = (UCHAR)(0x5A ^ i);              // host id: any fixed value is legal
        }
        *(USHORT*)(cd + 16) = NVMEOF_CNTLID_DYNAMIC; // 0xFFFF is mandatory on admin connect
        RtlCopyMemory(cd + 256, kSubsysNqn, sizeof(kSubsysNqn) - 1);
        RtlCopyMemory(cd + 512, kHostNqn, sizeof(kHostNqn) - 1);

        RtlZeroMemory(capsule, sizeof(capsule));
        capsule[0] = NVMEOF_OPC_FABRICS;
        capsule[1] = NVMEOF_CMD_FLAGS_METABUF;
        *(USHORT*)(capsule + 2) = ++cid;
        capsule[4] = NVMEOF_FCTYPE_CONNECT;
        sglKeyed(capsule + 24, (ULONGLONG)(ULONG_PTR)cd, NVMEOF_CONNECT_DATA_SIZE, g_rkey);
        *(USHORT*)(capsule + 42) = 0;               // qid 0 = admin
        *(USHORT*)(capsule + 44) = NVMEOFK_SQ_DEPTH;
        *(ULONG*)(capsule + 48) = NVMEOF_KATO_DEFAULT;

        st = submitCapsule(capsule, response, 20000);
        if (!NT_SUCCESS(st)) {
            logf("Fabrics Connect exchange failed 0x%08X", st);
            return st;
        }
        USHORT sc = responseStatus(response);
        ULONG  result = *(ULONG*)(response + 0);
        logf("Fabrics Connect: status=0x%04X%s  cntlid=%u  (raw resp %02X %02X %02X %02X ... %02X %02X)",
             sc, (sc == 0) ? " (success)" : "", result,
             response[0], response[1], response[2], response[3], response[14], response[15]);
        traceValue(L"nvmeofk_connect_status", sc);
        traceValue(L"nvmeofk_cntlid", result);
        if (sc != 0) {
            return STATUS_UNSUCCESSFUL;
        }
    }

    // ---- Property Get CAP: an admin command round trip --------------------
    {
        RtlZeroMemory(capsule, sizeof(capsule));
        capsule[0] = 0x7F;                          // fabrics, again
        capsule[1] = NVMEOF_CMD_FLAGS_METABUF;
        *(USHORT*)(capsule + 2) = ++cid;
        capsule[4] = NVMEOF_FCTYPE_PROP_GET;
        sglNull(capsule + 24);                      // no data, but still a descriptor
        *(ULONG*)(capsule + 40) = NVMEOF_PROP_CAP;  // offset 0 = CAP

        st = submitCapsule(capsule, response, 20000);
        if (!NT_SUCCESS(st)) {
            logf("Property Get CAP exchange failed 0x%08X", st);
            return st;
        }
        USHORT sc = responseStatus(response);
        ULONG  cap = *(ULONG*)(response + 0);
        logf("Property Get CAP: status=0x%04X%s  CAP=0x%08X  MQES=%u",
             sc, (sc == 0) ? " (success)" : "", cap, (cap & 0xFFFF) + 1);
        traceValue(L"nvmeofk_cap", cap);
        traceValue(L"nvmeofk_cap_status", sc);
        if (sc != 0) {
            return STATUS_UNSUCCESSFUL;
        }
    }

    traceValue(L"nvmeofk_done", 1);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  Entry / exit
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Teardown.  The first version of this driver had NONE, and that is almost
//  certainly what the 0x139 came from: when the connect was refused the driver
//  returned with the QP, CQ, PD, connector, MR and a locked MDL still live, and
//  DriverUnload was an empty stub, so none of it was ever released - a loaded
//  driver holding NDK objects the provider can still walk into.
//
//  Everything created is destroyed here, in reverse order, and the WSK
//  registration is released, so the driver can actually stop.
// ---------------------------------------------------------------------------
// ONE CONTEXT PER CLOSE.  The teardown used to call StepStart(&g_shared) five times
// in a row - KeInitializeEvent on the same dispatcher object five times - which is
// exactly the wait-list corruption the minidump showed.
static NVMEOFK_ASYNC g_closeCtx[6];

static void closeDone(PVOID context, NTSTATUS status)
{
    requestComplete(context, status);
}

// ---------------------------------------------------------------------------
//  Correct close path.  Two facts from ndkpi.h drive all of this:
//
//   * NDK_FN_CLOSE_COMPLETION takes ONLY a Context - no status.  The old closeDone
//     below has two parameters, which is a type mismatch against every close method
//     (NdkCloseConnector/Qp/Cq/Pd/Mr are all NDK_FN_CLOSE_OBJECT).
//   * A connection attempt that FAILS still leaves CM state inside the provider.  The
//     driver used to return and leave it there ("leak everything, like D0").  D0 got
//     away with that because its connection SUCCEEDED and ended cleanly; a failed one
//     dangles, and about ninety seconds later mlx4eth63 trips
//     KERNEL_SECURITY_CHECK_FAILURE (0x139, P1=0xa).  That is what every one of the
//     delayed bugchecks in this project was.
//
//  So: on any failure after the connector exists, close it - and wait for the close
//  completion before returning, so nothing is left half-torn-down.
// ---------------------------------------------------------------------------
static void closeDone1(PVOID context)
{
    NVMEOFK_ASYNC* a = (NVMEOFK_ASYNC*)context;
    if (a == NULL) {
        return;
    }
    a->status = STATUS_SUCCESS;      // a close completion carries no status of its own
    KeSetEvent(&a->done, IO_NO_INCREMENT, FALSE);
}

// Close the connector after a failed connect (or on the way out).  Separate context,
// waits for the completion, never re-initialises an event that is already live.
static void closeConnectorNow(void)
{
    if (g_connector == NULL) {
        return;
    }
    StepStart(&g_closeCtx[0]);
    (void)g_connector->Dispatch->NdkCloseConnector(&g_connector->Header, closeDone1, &g_closeCtx[0]);
    (void)StepFinish(STATUS_PENDING, &g_closeCtx[0], 10000);
    logf("connector closed after the failed connect (this is what stops the delayed 0x139)");
    g_connector = NULL;
}

static BOOLEAN g_tornDown = FALSE;
static BOOLEAN g_regionSafeToFree = FALSE;   // set only when the MR really deregistered
static BOOLEAN g_mrRegistered     = FALSE;   // mirrors the register call result for logging


static void NvmeofkTeardown(void)
{
    if (g_tornDown) {
        return;
    }
    g_tornDown = TRUE;

    // ORDER MATTERS, and the previous version got it wrong in the way that WER
    // recorded as AV_nvmeofk2!unknown_function:
    //   * the MR is DEREGISTERED first and the result is checked.  Closing the MR
    //     and then freeing the MDL and the pool block unconditionally means that if
    //     the close had not really completed, the HCA still had the region
    //     registered and the next access to it faulted at DISPATCH_LEVEL.
    //   * if the deregister does not succeed the backing memory is deliberately NOT
    //     freed.  A leaked allocation is a far better outcome than a bugcheck.
    // NO NDK OBJECT IS CLOSED HERE, DELIBERATELY.
    //
    // D0 - the kernel driver in this project that actually completed an RDMA read
    // against this hardware - never closed a single NDK object.  It leaked them until
    // reboot and it never bugchecked.  This file tried to be tidy and added a full
    // close path; that path is where every remaining crash lives, and the minidump of
    // the last one shows it dying inside KeWaitForSingleObject on the event belonging
    // to a close context (r12 == g_closeCtx[5].done, with a corrupted wait list).
    // The header explains why the shape was wrong: closes take NDK_FN_CLOSE_COMPLETION
    // which receives ONLY a Context - no status - while NdkDeregisterMr takes a
    // NDK_FN_REQUEST_COMPLETION with (Context, Status).  One callback for both slots
    // is a type mismatch, and the provider is entitled to do anything with it.
    //
    // Getting kernel NVMe-oF working is the goal; a clean teardown is a later,
    // separate problem with its own testing budget.  Align with the proven driver.
    logf("teardown: NDK objects intentionally NOT closed (D0 behaviour);  WSK released");


    if (g_regionSafeToFree) {
        if (g_mdl != NULL) {
            MmUnlockPages(g_mdl);   // MmProbeAndLockPages has to be undone before the MDL goes away
            IoFreeMdl(g_mdl);
            g_mdl = NULL;
        }
        if (g_region != NULL) {
            ExFreePoolWithTag(g_region, NVMEOFK_TAG);
            g_region = NULL;
        }
    } else if (g_region != NULL) {
        logf("region %p intentionally leaked (MR was not safely deregistered)", g_region);
    }

    // NOTHING IS RELEASED HERE - not the NDK objects, not the adapter, not the WSK
    // provider.  This is deliberate and it is what D0 effectively did, because D0
    // never actually ran its cleanup path.
    //
    // The hang that produced this comment: with the NDK objects intentionally left
    // open (the previous change), the WskReleaseProviderNPI call below never
    // returned.  The provider still had live NDK usage, so releasing it blocked
    // forever, and the driver sat in Start Pending with DriverEntry never returning
    // - no bugcheck, just a dead driver and a machine that slowly suffocates.
    // The minidump route would never have caught that one: a hang leaves no dump.
    //
    // A driver that is loaded once for a measurement, leaks until reboot and returns
    // immediately is exactly the right shape for this stage.  Correct teardown is a
    // separate piece of work with its own test budget.
    logf("teardown: nothing released on purpose (D0 behaviour) - objects live until reboot");
}

// Is the network part of this driver wanted at all?  Default NO: a bare load then
// only opens the adapter and builds the objects, which is a test that cannot touch
// the network and cannot leave a half-open RDMA connection behind.
static BOOLEAN connectWanted(void)
{
    UNICODE_STRING kn, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buf[24];
    ULONG len = sizeof(buf);
    BOOLEAN want = FALSE;

    RtlInitUnicodeString(&kn, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&vn, L"nvmeofk_doconnect");
        if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
            KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                want = (*(ULONG*)p->Data != 0);
            }
        }
        ZwClose(key);
    }
    return want;
}

static VOID nvmeofkUnload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver);
    NvmeofkTeardown();
    // The probes in this project deliberately do not tear the NDK objects down on
    // unload: an outstanding operation against a freed object is a bugcheck, and
    // the process ends with the driver anyway.
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    NTSTATUS st;

    UNREFERENCED_PARAMETER(registryPath);
    driver->DriverUnload = nvmeofkUnload;

    logf("=== nvmeofk D2.1: NVMe-oF from kernel mode ===");
    st = openAdapter();
    if (!NT_SUCCESS(st)) {
        logf("openAdapter failed 0x%08X", st);
        writeLog();
        return STATUS_SUCCESS;      // the log is the result; do not fail the load
    }
    st = createObjects();
    if (!NT_SUCCESS(st)) { logf("createObjects failed 0x%08X", st); writeLog(); return STATUS_SUCCESS; }
    st = registerRegion();
    if (!NT_SUCCESS(st)) { logf("registerRegion failed 0x%08X", st); writeLog(); return STATUS_SUCCESS; }
    if (!connectWanted()) {
        logf("bring-up only: nvmeofk_doconnect is not set, so the network is not touched");
        logf("(set HKLM\\Software\\NVMeoFProbe\\nvmeofk_doconnect = 1 to enable the NVMe-oF exchange)");
        NvmeofkTeardown();
        writeLog();
        return STATUS_SUCCESS;
    }

    st = connectToTarget();
    if (!NT_SUCCESS(st)) {
        logf("connectToTarget failed 0x%08X", st);
        NvmeofkTeardown();
        writeLog();
        return STATUS_SUCCESS;
    }
    st = nvmeofRun();
    logf("nvmeofRun = 0x%08X%s", st, NT_SUCCESS(st) ? "  (NVME-OF FROM KERNEL: OK)" : "");
    NvmeofkTeardown();
    writeLog();
    return STATUS_SUCCESS;
    st = nvmeofRun();
    logf("nvmeofRun = 0x%08X%s", st, NT_SUCCESS(st) ? "  (NVME-OF FROM KERNEL: OK)" : "");
    writeLog();
    return STATUS_SUCCESS;
}
