// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
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

// ---- the LISTENER half (D3): does NDK let one HCA talk to itself? ----------
// The card has two ports which are physically cross-connected (40 Gbps, both
// "Connected"): 192.168.100.2 = 以太网 4 (ifIndex 30), 192.168.100.3 = 以太网 5
// (ifIndex 17).  A listener on the .2 adapter and a connector on the .3 adapter
// therefore test the CM loopback question for real - and unlike the user-mode
// attempt (f5_interop could not even open its adapter: no ndv2.dll, 0xC0000141),
// this listener actually exists, so 0xC0000236 would now be a real answer.
static NDK_ADAPTER*   g_ladapter;
static NDK_PD*        g_lpd;
static NDK_CQ*        g_lcq;
static NDK_QP*        g_lqp;
static NDK_MR*        g_lmr;
static NDK_LISTENER*  g_listener;
static MDL*           g_lmdl;
static PVOID          g_lregion;
static UINT32         g_llkey;
static UINT32         g_lrkey;

static KEVENT g_lcqEvent;
static KSPIN_LOCK g_lcqLock;
static NDK_RESULT g_lcqResults[16];
static volatile LONG g_lcqCount;
static volatile LONG g_lcqDropped;

static KEVENT g_acceptEvent;
static volatile LONG g_acceptEvents;
static NDK_CONNECTOR* g_incoming;    // handed to the connect-event callback
static NDK_CONNECTOR* g_accepted;    // the same object, after NdkAccept was issued

static NVMEOFK_ASYNC g_lpdCtx;
static NVMEOFK_ASYNC g_lcqCtx;
static NVMEOFK_ASYNC g_lqpCtx;
static NVMEOFK_ASYNC g_lmrCtx;
static NVMEOFK_ASYNC g_listenCtx;
static NVMEOFK_ASYNC g_acceptCtx;
static NVMEOFK_ASYNC g_lsendCtx;
static NVMEOFK_ASYNC g_lrecvCtx;
static NVMEOFK_ASYNC g_lreadCtx;

static ULONG   g_lport      = 4420;
static ULONG   g_listenIf   = 0;
static BOOLEAN g_loopback   = FALSE;
static BOOLEAN g_lconnect   = FALSE;
static BOOLEAN g_loopTornDown = FALSE;

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
// Every knob is a REG_DWORD under HKLM\Software\NVMeoFProbe, so a test can be
// reconfigured without a rebuild (and the previous build's registry code is
// replaced by this one helper).
static ULONG regDword(PCWSTR name, ULONG defaultValue)
{
    UNICODE_STRING kn, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    UCHAR buf[24];
    ULONG len = sizeof(buf);
    ULONG value = defaultValue;

    RtlInitUnicodeString(&kn, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&vn, name);
        if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
            KEY_VALUE_PARTIAL_INFORMATION* p = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (p->Type == REG_DWORD && p->DataLength == sizeof(ULONG)) {
                value = *(ULONG*)p->Data;
            }
        }
        ZwClose(key);
    }
    return value;
}

static BOOLEAN g_wskReady = FALSE;

// WSK registration is process-wide, so it happens once even when two adapters
// (one per port) are opened.
static NTSTATUS wskInit(void)
{
    NTSTATUS st;

    if (g_wskReady) {
        return STATUS_SUCCESS;
    }
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
    logf("WskControlClient('NDKD') = 0x%08X (open=%p close=%p)", st,
         g_ndk.WskOpenNdkAdapter, g_ndk.WskCloseNdkAdapter);
    if (!NT_SUCCESS(st) || g_ndk.WskOpenNdkAdapter == NULL) {
        return NT_SUCCESS(st) ? STATUS_INVALID_DEVICE_STATE : st;
    }
    g_wskReady = TRUE;
    return STATUS_SUCCESS;
}

// Open ONE NDK adapter for ONE port.  ifIndex 30 = the .2 port, 17 = the .3 port.
static NTSTATUS openAdapterOn(ULONG ifIndex, NDK_ADAPTER** out, const char* tag)
{
    NDK_VERSION ver;
    NTSTATUS st;

    *out = NULL;
    RtlZeroMemory(&ver, sizeof(ver));
    ver.Major = 2;
    ver.Minor = 0;

    st = g_ndk.WskOpenNdkAdapter(g_wskProvider.Client, ver, (NET_IFINDEX)ifIndex, out);
    logf("%s WskOpenNdkAdapter(ifIndex=%u, v%u.%u) = 0x%08X adapter=%p",
         tag, ifIndex, ver.Major, ver.Minor, st, *out);
    if (!NT_SUCCESS(st) || *out == NULL) {
        logf("!! %s adapter on ifIndex %u is not usable (0xC0010011 = the RDMA stack does not have that address)", tag, ifIndex);
        return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    }
    traceValue((tag[0] == 'L') ? L"nvmeofk_lifindex" : L"nvmeofk_ifindex", ifIndex);
    return STATUS_SUCCESS;
}

static NTSTATUS openAdapter(void)
{
    NTSTATUS st;
    ULONG ifIndex;

    st = wskInit();
    if (!NT_SUCCESS(st)) {
        return st;
    }
    ifIndex = regDword(L"nvmeofk_ifindex", NVMEOFK_DEFAULT_IFINDEX);
    logf("initiator ifIndex from registry: %u", ifIndex);
    return openAdapterOn(ifIndex, &g_adapter, "C");
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
    // ndkpi.h: 0x1 ALLOW_LOCAL_WRITE, 0x2 ALLOW_REMOTE_READ, 0x5 ALLOW_REMOTE_WRITE
    // (0x5 = 0x4|0x1, so this IS a bit mask).  In loopback mode our own in-kernel
    // target RDMA-Reads the 1024-byte Connect data out of this region, which only
    // works if remote read is granted - and the local write is what lets the NIC
    // drop the 16-byte response into the region at RESP_OFF.
    {
        ULONG mrFlags = NDK_MR_FLAG_ALLOW_LOCAL_WRITE;
        if (g_loopback) {
            mrFlags |= NDK_MR_FLAG_ALLOW_REMOTE_READ;
        }
        st = g_mr->Dispatch->NdkRegisterMr(g_mr, g_mdl, NVMEOFK_REGION_BYTES,
                mrFlags, requestComplete, &g_async);
        logf("NdkRegisterMr flags = 0x%X%s", mrFlags, (mrFlags & NDK_MR_FLAG_ALLOW_REMOTE_READ) ? " (remote read: the target may read this region)" : "");
    }
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

// ===========================================================================
//  D3: the LISTENER half.
//
//  The CX3's two ports are physically cross-connected.  If NDK lets us put a
//  listener on 192.168.100.2 (ifIndex 30) and connect to it from 192.168.100.3
//  (ifIndex 17) on the SAME card, then a single machine can carry the whole
//  NVMe-oF/RDMA path with no second host - which is exactly what this machine
//  needs, because the EndeavourOS laptop (192.168.100.5) is not here and
//  user-mode NetworkDirect does not exist on this install (no ndv2.dll).
//
//  The previous "loopback is refused" note was measured against a listener
//  that never existed (f5_interop dies at NdOpenAdapter with 0xC0000141), so
//  0xC0000236 was just "nobody is listening".  This listener really listens.
// ===========================================================================
static void lcqNotification(PVOID context, NTSTATUS status)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(status);
    InterlockedIncrement(&g_lcqCount);
    KeSetEvent(&g_lcqEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS lreap(ULONG wantCtx, ULONG ms, ULONG* bytesOut)
{
    LARGE_INTEGER t;
    ULONG spin = 0;

    for (;;) {
        LONG have = InterlockedExchange(&g_lcqCount, 0);
        if (have > 0) {
            ULONG got = g_lcq->Dispatch->NdkGetCqResults(g_lcq, g_lcqResults,
                                                         (ULONG)(have > 16 ? 16 : have));
            for (ULONG i = 0; i < got && i < (ULONG)(sizeof(g_lcqResults) / sizeof(g_lcqResults[0])); i++) {
                NDK_RESULT* r = &g_lcqResults[i];
                if ((ULONG)(ULONG_PTR)r->RequestContext == wantCtx) {
                    if (bytesOut != NULL) {
                        *bytesOut = r->BytesTransferred;
                    }
                    traceValue(L"nvmeofk_llastcq", r->Status);
                    return r->Status;
                }
                InterlockedIncrement(&g_lcqDropped);
            }
        }
        spin++;
        if (spin > (ms * 4)) {
            return STATUS_IO_TIMEOUT;
        }
        t.QuadPart = -10000;
        KeWaitForSingleObject(&g_lcqEvent, Executive, KernelMode, FALSE, &t);
    }
}

// Called by the provider at DISPATCH_LEVEL when a connection request arrives.
// Only two things are legal here: record the object and signal the event.
static VOID connectEvent(PVOID context, NDK_CONNECTOR* conn)
{
    UNREFERENCED_PARAMETER(context);
    g_incoming = conn;
    InterlockedIncrement(&g_acceptEvents);
    KeSetEvent(&g_acceptEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS lregisterRegion(void)
{
    NTSTATUS st;

    g_lregion = ExAllocatePoolWithTag(NonPagedPoolNx, NVMEOFK_REGION_BYTES, NVMEOFK_TAG);
    if (g_lregion == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g_lregion, NVMEOFK_REGION_BYTES);

    g_lmdl = IoAllocateMdl(g_lregion, NVMEOFK_REGION_BYTES, FALSE, FALSE, NULL);
    if (g_lmdl == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    __try {
        MmProbeAndLockPages(g_lmdl, KernelMode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("!! L MmProbeAndLockPages raised 0x%08X", GetExceptionCode());
        IoFreeMdl(g_lmdl);
        g_lmdl = NULL;
        return STATUS_UNSUCCESSFUL;
    }

    StepStart(&g_lmrCtx);
    st = g_lpd->Dispatch->NdkCreateMr(g_lpd, FALSE, complete, &g_lmrCtx, &g_lmr);
    st = StepFinish(st, &g_lmrCtx, 10000);
    writeLog();
    if (NT_SUCCESS(st) && g_lmr == NULL) { g_lmr = (NDK_MR*)g_lmrCtx.object; }
    logf("L NdkCreateMr        = 0x%08X mr=%p", st, g_lmr);
    if (!NT_SUCCESS(st) || g_lmr == NULL) { return STATUS_UNSUCCESSFUL; }

    // LOCAL_WRITE (the NIC writes the received capsule into it) + RDMA_READ_SINK
    // (it is also the local sink of the RDMA Read that pulls the Connect data out
    // of the initiator's region).
    StepStart(&g_lmrCtx);
    st = g_lmr->Dispatch->NdkRegisterMr(g_lmr, g_lmdl, NVMEOFK_REGION_BYTES,
            NDK_MR_FLAG_ALLOW_LOCAL_WRITE | NDK_MR_FLAG_RDMA_READ_SINK,
            requestComplete, &g_lmrCtx);
    st = StepFinish(st, &g_lmrCtx, 10000);
    writeLog();
    logf("L NdkRegisterMr(flags 0x%X) = 0x%08X", NDK_MR_FLAG_ALLOW_LOCAL_WRITE | NDK_MR_FLAG_RDMA_READ_SINK, st);
    if (!NT_SUCCESS(st)) { return st; }

    g_llkey = g_lmr->Dispatch->NdkGetLocalTokenFromMr(g_lmr);
    g_lrkey = g_lmr->Dispatch->NdkGetRemoteTokenFromMr(g_lmr);
    logf("L region %p: lkey=0x%08X rkey=0x%08X", g_lregion, g_llkey, g_lrkey);
    traceValue(L"nvmeofk_lrkey", g_lrkey);
    return STATUS_SUCCESS;
}

static NTSTATUS createListenerSide(void)
{
    NTSTATUS st;
    SOCKADDR_IN la;
    UCHAR abuf[64];
    ULONG alen = sizeof(abuf);

    KeInitializeEvent(&g_lcqEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&g_lcqLock);
    KeInitializeEvent(&g_acceptEvent, NotificationEvent, FALSE);

    st = openAdapterOn(g_listenIf, &g_ladapter, "L");
    if (!NT_SUCCESS(st)) { return st; }

    StepStart(&g_lpdCtx);
    st = g_ladapter->Dispatch->NdkCreatePd(g_ladapter, complete, &g_lpdCtx, &g_lpd);
    st = StepFinish(st, &g_lpdCtx, 10000);
    writeLog();
    if (NT_SUCCESS(st) && g_lpd == NULL) { g_lpd = (NDK_PD*)g_lpdCtx.object; }
    logf("L NdkCreatePd        = 0x%08X pd=%p", st, g_lpd);
    if (!NT_SUCCESS(st) || g_lpd == NULL) { return STATUS_UNSUCCESSFUL; }

    StepStart(&g_lcqCtx);
    st = g_ladapter->Dispatch->NdkCreateCq(g_ladapter, 64, lcqNotification, NULL, NULL,
                                           complete, &g_lcqCtx, &g_lcq);
    st = StepFinish(st, &g_lcqCtx, 10000);
    writeLog();
    if (NT_SUCCESS(st) && g_lcq == NULL) { g_lcq = (NDK_CQ*)g_lcqCtx.object; }
    logf("L NdkCreateCq        = 0x%08X cq=%p", st, g_lcq);
    if (!NT_SUCCESS(st) || g_lcq == NULL) { return STATUS_UNSUCCESSFUL; }

    // The QP that the accepted connection will run on.  Created before the
    // request arrives, because NdkAccept takes it as a parameter.
    StepStart(&g_lqpCtx);
    st = g_lpd->Dispatch->NdkCreateQp(g_lpd, g_lcq, g_lcq, NULL, 16, 16, 4, 4, 0,
                                      complete, &g_lqpCtx, &g_lqp);
    st = StepFinish(st, &g_lqpCtx, 10000);
    writeLog();
    if (NT_SUCCESS(st) && g_lqp == NULL) { g_lqp = (NDK_QP*)g_lqpCtx.object; }
    logf("L NdkCreateQp        = 0x%08X qp=%p", st, g_lqp);
    if (!NT_SUCCESS(st) || g_lqp == NULL) { return STATUS_UNSUCCESSFUL; }

    StepStart(&g_listenCtx);
    st = g_ladapter->Dispatch->NdkCreateListener(g_ladapter, connectEvent, NULL,
                                                 complete, &g_listenCtx, &g_listener);
    st = StepFinish(st, &g_listenCtx, 10000);
    writeLog();
    if (NT_SUCCESS(st) && g_listener == NULL) { g_listener = (NDK_LISTENER*)g_listenCtx.object; }
    logf("L NdkCreateListener  = 0x%08X listener=%p", st, g_listener);
    if (!NT_SUCCESS(st) || g_listener == NULL) { return STATUS_UNSUCCESSFUL; }

    st = lregisterRegion();
    writeLog();
    if (!NT_SUCCESS(st)) { return st; }

    // Bind the address that belongs to the LISTENER port (nvmeofk_target = .2).
    RtlZeroMemory(&la, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = RtlUshortByteSwap((USHORT)g_lport);
    la.sin_addr.S_un.S_addr = regDword(L"nvmeofk_target", NVMEOFK_DEFAULT_TARGET);

    StepStart(&g_listenCtx);
    st = g_listener->Dispatch->NdkListen(g_listener, (CONST PSOCKADDR)&la, sizeof(la),
                                         requestComplete, &g_listenCtx);
    st = StepFinish(st, &g_listenCtx, 20000);
    logf("L NdkListen(%u.%u.%u.%u:%u) = 0x%08X",
         ((PUCHAR)&la.sin_addr)[0], ((PUCHAR)&la.sin_addr)[1],
         ((PUCHAR)&la.sin_addr)[2], ((PUCHAR)&la.sin_addr)[3], g_lport, st);
    traceValue(L"nvmeofk_listen", (ULONG)st);
    writeLog();
    if (!NT_SUCCESS(st)) { return st; }

    RtlZeroMemory(abuf, sizeof(abuf));
    st = g_listener->Dispatch->NdkGetLocalAddress(g_listener, (PSOCKADDR)abuf, &alen);
    {
        SOCKADDR_IN* a = (SOCKADDR_IN*)abuf;
        logf("L NdkGetLocalAddress = 0x%08X len=%u family=%u port=%u addr=%u.%u.%u.%u",
             st, alen,
             (alen >= 2) ? (ULONG)a->sin_family : 0UL,
             (alen >= 4) ? (ULONG)RtlUshortByteSwap(a->sin_port) : 0UL,
             (alen >= 8) ? ((PUCHAR)&a->sin_addr)[0] : 0,
             (alen >= 8) ? ((PUCHAR)&a->sin_addr)[1] : 0,
             (alen >= 8) ? ((PUCHAR)&a->sin_addr)[2] : 0,
             (alen >= 8) ? ((PUCHAR)&a->sin_addr)[3] : 0);
    }
    return st;
}

// ---- the in-kernel nvmet stand-in: one capsule in, one response out --------
#define CTX_LRECV 0x2001
#define CTX_LSEND 0x2002
#define CTX_LREAD 0x2003

static BOOLEAN strEq(const char* a, const char* b)
{
    while (*a != 0 && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0) ? TRUE : FALSE;
}

static NTSTATUS responderPrepare(void)
{
    NDK_SGE sge;

    sge.VirtualAddress = (UCHAR*)g_lregion + NVMEOFK_CAP_OFF;
    sge.Length = NVMEOFK_CAPSULE_BYTES;
    sge.MemoryRegionToken = g_llkey;
    return g_lqp->Dispatch->NdkReceive(g_lqp, (PVOID)(ULONG_PTR)CTX_LRECV, &sge, 1);
}

static NTSTATUS responderServe(ULONG timeoutMs)
{
    UCHAR* cap = (UCHAR*)g_lregion + NVMEOFK_CAP_OFF;
    UCHAR* resp = (UCHAR*)g_lregion + NVMEOFK_RESP_OFF;
    ULONG got = 0;
    NTSTATUS st;

    st = lreap(CTX_LRECV, timeoutMs, &got);
    logf("L recv completion    = 0x%08X bytes=%u", st, got);
    if (!NT_SUCCESS(st)) { return st; }

    logf("L capsule: opc=0x%02X flags=0x%02X cid=%u fctype=0x%02X",
         cap[0], cap[1], *(USHORT*)(cap + 2), cap[4]);

    // Keyed SGL at capsule[24..39]: 8-byte address, 24-bit length, 4-byte rkey,
    // and 0x40 in the last byte.  Pull the Connect data out of the initiator's
    // memory with a real RDMA Read - that is the byte-level proof this is not
    // just two SENDs over a socket.
    {
        ULONGLONG addr = *(ULONGLONG*)(cap + 24);
        ULONG len = (ULONG)cap[32] | ((ULONG)cap[33] << 8) | ((ULONG)cap[34] << 16);
        UINT32 rkey = *(UINT32*)(cap + 35);
        logf("L SGL: addr=%p len=%u rkey=0x%08X type=0x%02X", (PVOID)(ULONG_PTR)addr, len, rkey, cap[39]);

        if (len > 0 && len <= NVMEOF_CONNECT_DATA_SIZE) {
            NDK_SGE sge;
            sge.VirtualAddress = (UCHAR*)g_lregion + NVMEOFK_CONNECT_OFF;
            sge.Length = len;
            sge.MemoryRegionToken = g_llkey;

            st = g_lqp->Dispatch->NdkRead(g_lqp, (PVOID)(ULONG_PTR)CTX_LREAD, &sge, 1,
                                          (UINT64)addr, rkey, 0);
            logf("L NdkRead(remote %p, %u bytes, rkey 0x%08X) = 0x%08X",
                 (PVOID)(ULONG_PTR)addr, len, rkey, st);
            if (NT_SUCCESS(st)) {
                got = 0;
                st = lreap(CTX_LREAD, timeoutMs, &got);
                logf("L RDMA read completion = 0x%08X bytes=%u", st, got);
            }
            if (!NT_SUCCESS(st)) {
                logf("!! the target could not read the initiator's Connect data (0xC0000005 = remote access denied)");
                return st;
            }
            {
                char* cd = (char*)g_lregion + NVMEOFK_CONNECT_OFF;
                logf("L connect data: hostid=%02X%02X%02X%02X cntlid=0x%04X subnqn='%s' hostnqn='%s'",
                     (UCHAR)cd[0], (UCHAR)cd[1], (UCHAR)cd[2], (UCHAR)cd[3],
                     *(USHORT*)(cd + 16), cd + 256, cd + 512);
                logf("L subnqn matches this project's subsystem: %s", strEq(cd + 256, kSubsysNqn) ? "YES" : "NO");
            }
        }
    }

    // 16-byte RDMA response: result in dwords 0..3, status word at byte 14.
    RtlZeroMemory(resp, NVMEOFK_RESPONSE_BYTES);
    if (cap[4] == NVMEOF_FCTYPE_CONNECT) {
        *(ULONG*)resp = 1;                                  // cntlid
        logf("L response: Fabrics Connect -> cntlid=1, status=0x0000");
    } else if (cap[4] == NVMEOF_FCTYPE_PROP_GET) {
        *(ULONG*)resp = 0x0000FFFF;                         // a plausible CAP (MQES = 65535)
        logf("L response: Property Get -> CAP=0x0000FFFF, status=0x0000");
    } else {
        logf("L response: fctype 0x%02X is not one this stand-in knows, answering status=0 anyway", cap[4]);
    }

    {
        NDK_SGE sge;
        sge.VirtualAddress = resp;
        sge.Length = NVMEOFK_RESPONSE_BYTES;
        sge.MemoryRegionToken = g_llkey;
        st = g_lqp->Dispatch->NdkSend(g_lqp, (PVOID)(ULONG_PTR)CTX_LSEND, &sge, 1, 0);
        logf("L NdkSend(response)  = 0x%08X", st);
        if (!NT_SUCCESS(st)) { return st; }
        got = 0;
        st = lreap(CTX_LSEND, timeoutMs, &got);
        logf("L send completion    = 0x%08X bytes=%u", st, got);
    }
    return st;
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
    remote.sin_port = RtlUshortByteSwap((USHORT)g_lport);
    // nvmeofk_target / nvmeofk_local are REG_DWORDs holding addresses in NETWORK byte
    // order, so the peer can be changed without rebuilding the driver.
    remote.sin_addr.S_un.S_addr = regDword(L"nvmeofk_target", NVMEOFK_DEFAULT_TARGET);
    local.sin_addr.S_un.S_addr  = regDword(L"nvmeofk_local", 0);

    logf("connecting to %u.%u.%u.%u:%u (local %u.%u.%u.%u)",
         ((PUCHAR)&remote.sin_addr)[0], ((PUCHAR)&remote.sin_addr)[1],
         ((PUCHAR)&remote.sin_addr)[2], ((PUCHAR)&remote.sin_addr)[3], g_lport,
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
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkConnect submit  = 0x%08X%s", st,
         (st == STATUS_PENDING) ? " (pending: the CM is working on it)" : "");
    if (st != STATUS_PENDING && !NT_SUCCESS(st)) {
        logf("!! NdkConnect was refused straight away: 0x%08X (0xC0000236 = connection refused, i.e. no listener)", st);
        traceValue(L"nvmeofk_connect", (ULONG)st);
        closeConnectorNow();
        return st;
    }

    // ---- loopback only: answer our own connection request ------------------
    if (g_loopback) {
        LARGE_INTEGER t;
        NTSTATUS sw;

        t.QuadPart = -200000000LL;      // 20 s
        sw = KeWaitForSingleObject(&g_acceptEvent, Executive, KernelMode, FALSE, &t);
        logf("L connect event    = 0x%08X events=%u incoming=%p", sw, g_acceptEvents, g_incoming);
        if (sw == STATUS_TIMEOUT || g_incoming == NULL) {
            logf("!! no connection request reached the listener within 20 s");
            closeConnectorNow();
            return STATUS_IO_TIMEOUT;
        }
        g_accepted = g_incoming;

        // What the initiator put in the RDMA-CM REQ - read on the incoming connector
        // before accepting, i.e. this is the private data from the other port of the
        // same card.
        {
            ULONG inLimit = 0, outLimit = 0, plen = sizeof(peerData);
            NTSTATUS sp = g_accepted->Dispatch->NdkGetConnectionData(g_accepted, &inLimit, &outLimit,
                                                                     peerData, &plen);
            logf("L NdkGetConnectionData = 0x%08X inbound=%u outbound=%u len=%u", sp, inLimit, outLimit, plen);
            if (NT_SUCCESS(sp) && plen >= 32) {
                logf("L peer private data: recfmt=%u qid=%u hrqsize=%u hsqsize=%u cntlid=%u",
                     *(USHORT*)(peerData + 0), *(USHORT*)(peerData + 2), *(USHORT*)(peerData + 4),
                     *(USHORT*)(peerData + 6), *(USHORT*)(peerData + 8));
            }
        }

        {
            UCHAR lpd[32];
            fillPrivateData(lpd, 0, NVMEOFK_SQ_DEPTH, 0);
            // A marker the initiator must find in NdkGetConnectionData, byte for byte.
            RtlCopyMemory(lpd + 16, "NVMET-LISTENER01", 16);
            StepStart(&g_acceptCtx);
            st = g_accepted->Dispatch->NdkAccept(g_accepted, g_lqp, 1, 1, lpd, sizeof(lpd),
                                                 NULL, NULL, requestComplete, &g_acceptCtx);
            logf("L NdkAccept submit = 0x%08X (qp %p)", st, g_lqp);
            writeLog();
            if (st != STATUS_PENDING && !NT_SUCCESS(st)) {
                logf("!! NdkAccept failed 0x%08X", st);
                closeConnectorNow();
                return st;
            }
        }
    }

    st = StepFinish(st, &g_connectCtx, 20000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkConnect         = 0x%08X (0xC0000236 = connection refused)", st);
    traceValue(L"nvmeofk_connect", (ULONG)st);
    if (!NT_SUCCESS(st)) {
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
    if (NT_SUCCESS(st) && peerDataLen >= 32) {
        logf("peer private data: recfmt=%u qid=%u hrqsize=%u hsqsize=%u cntlid=%u",
             *(USHORT*)(peerData + 0), *(USHORT*)(peerData + 2), *(USHORT*)(peerData + 4),
             *(USHORT*)(peerData + 6), *(USHORT*)(peerData + 8));
        if (g_loopback) {
            logf("peer private data marker: %s", RtlEqualMemory(peerData + 16, "NVMET-LISTENER01", 16)
                 ? "MATCHES 'NVMET-LISTENER01' byte for byte" : "does NOT match the listener's marker");
        }
    }

    StepStart(&g_connectCtx);
    st = g_connector->Dispatch->NdkCompleteConnect(g_connector, NULL, NULL, requestComplete, &g_connectCtx);
    st = StepFinish(st, &g_connectCtx, 20000);
    writeLog();   // incremental: if the next step corrupts something, the evidence is already on disk
    logf("NdkCompleteConnect = 0x%08X", st);
    if (!NT_SUCCESS(st)) {
        closeConnectorNow();
        return st;
    }

    if (g_loopback) {
        st = StepFinish(STATUS_PENDING, &g_acceptCtx, 20000);
        logf("L NdkAccept        = 0x%08X%s", st,
             NT_SUCCESS(st) ? " (the same card accepted its own connection)" : "");
        traceValue(L"nvmeofk_accept", (ULONG)st);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }
    return STATUS_SUCCESS;
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

static NTSTATUS postResponseRecv(void)
{
    NDK_SGE sge;

    sge.VirtualAddress = (UCHAR*)g_region + NVMEOFK_RESP_OFF;
    sge.Length = NVMEOFK_RESPONSE_BYTES;
    sge.MemoryRegionToken = g_lkey;
    return g_qp->Dispatch->NdkReceive(g_qp, (PVOID)(ULONG_PTR)(PVOID)(ULONG_PTR)CTX_RECV, &sge, 1);
}

static NTSTATUS sendCapsule(const UCHAR* capsule, ULONG timeoutMs)
{
    NTSTATUS st;
    NDK_SGE sge;

    RtlCopyMemory((UCHAR*)g_region + NVMEOFK_CAP_OFF, capsule, NVMEOFK_CAPSULE_BYTES);
    sge.VirtualAddress = (UCHAR*)g_region + NVMEOFK_CAP_OFF;
    sge.Length = NVMEOFK_CAPSULE_BYTES;
    sge.MemoryRegionToken = g_lkey;
    st = g_qp->Dispatch->NdkSend(g_qp, (PVOID)(ULONG_PTR)CTX_SEND, &sge, 1, 0);
    if (!NT_SUCCESS(st)) { logf("NdkSend failed 0x%08X", st); return st; }

    st = reap(CTX_SEND, timeoutMs, NULL);
    if (!NT_SUCCESS(st)) { logf("send completion 0x%08X", st); }
    return st;
}

static NTSTATUS waitResponse(UCHAR* responseOut, ULONG timeoutMs)
{
    NTSTATUS st = reap(CTX_RECV, timeoutMs, NULL);
    if (!NT_SUCCESS(st)) { logf("response completion 0x%08X", st); return st; }
    if (responseOut != NULL) {
        RtlCopyMemory(responseOut, (UCHAR*)g_region + NVMEOFK_RESP_OFF, NVMEOFK_RESPONSE_BYTES);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS submitCapsule(const UCHAR* capsule, UCHAR* responseOut, ULONG timeoutMs)
{
    NTSTATUS st;

    RtlZeroMemory((UCHAR*)g_region + NVMEOFK_RESP_OFF, NVMEOFK_RESPONSE_BYTES);

    // 1. post the receive for the 16-byte response
    st = postResponseRecv();
    if (!NT_SUCCESS(st)) { logf("NdkReceive failed 0x%08X", st); return st; }

    // 2. loopback: arm the in-kernel responder BEFORE the capsule leaves, otherwise
    //    the request could arrive at a QP with no receive posted.
    if (g_loopback) {
        st = responderPrepare();
        if (!NT_SUCCESS(st)) { logf("L NdkReceive(post) failed 0x%08X", st); return st; }
    }

    // 3. send the 64-byte capsule
    st = sendCapsule(capsule, timeoutMs);
    if (!NT_SUCCESS(st)) { return st; }

    // 4. loopback: be the target too - receive the capsule, RDMA-read the Connect
    //    data, answer with a 16-byte response.
    if (g_loopback) {
        st = responderServe(timeoutMs);
        if (!NT_SUCCESS(st)) { logf("responder failed 0x%08X", st); return st; }
    }

    return waitResponse(responseOut, timeoutMs);
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
static NVMEOFK_ASYNC g_closeCtx[12];

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
    (void)g_connector->Dispatch->NdkCloseConnector(g_connector, closeDone1, &g_closeCtx[0]);
    (void)StepFinish(STATUS_PENDING, &g_closeCtx[0], 10000);
    logf("connector closed after the failed connect (this is what stops the delayed 0x139)");
    g_connector = NULL;
}

static BOOLEAN g_tornDown = FALSE;
static BOOLEAN g_regionSafeToFree = FALSE;   // set only when the MR really deregistered
static BOOLEAN g_mrRegistered     = FALSE;   // mirrors the register call result for logging


static void NvmeofkTeardown(void)
{
    // Close in reverse order of creation, each with its OWN context (reusing one
    // context five times in a row is what corrupted the wait list in the first place),
    // each with the one-parameter NDK_FN_CLOSE_COMPLETION that ndkpi.h actually declares,
    // and each waited for.  This replaces the old "leak everything on purpose" path:
    // leaking was only ever safe because D0 connected successfully and never repeated.
    if (g_tornDown) {
        return;
    }
    g_tornDown = TRUE;

    if (g_connector != NULL) {
        StepStart(&g_closeCtx[0]);
        (void)g_connector->Dispatch->NdkCloseConnector(g_connector, closeDone1, &g_closeCtx[0]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[0], 10000);
        logf("teardown: connector closed");
        g_connector = NULL;
    }
    if (g_qp != NULL) {
        StepStart(&g_closeCtx[1]);
        (void)g_qp->Dispatch->NdkCloseQp(g_qp, closeDone1, &g_closeCtx[1]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[1], 10000);
        logf("teardown: qp closed");
        g_qp = NULL;
    }
    if (g_cq != NULL) {
        StepStart(&g_closeCtx[2]);
        (void)g_cq->Dispatch->NdkCloseCq(g_cq, closeDone1, &g_closeCtx[2]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[2], 10000);
        logf("teardown: cq closed");
        g_cq = NULL;
    }
    if (g_mr != NULL) {
        StepStart(&g_closeCtx[3]);
        (void)g_mr->Dispatch->NdkCloseMr(g_mr, closeDone1, &g_closeCtx[3]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[3], 10000);
        logf("teardown: mr closed");
        g_mr = NULL;
    }
    if (g_pd != NULL) {
        StepStart(&g_closeCtx[4]);
        (void)g_pd->Dispatch->NdkClosePd(g_pd, closeDone1, &g_closeCtx[4]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[4], 10000);
        logf("teardown: pd closed");
        g_pd = NULL;
    }
    writeLog();
    logf("teardown complete: every NDK object was closed, nothing leaked");
    writeLog();
}

// ---------------------------------------------------------------------------
//  D3 teardown: BOTH ends live in this driver, so both are closed here - and
//  unlike the old path, the adapters go back too.  Leaking one adapter per
//  load is how the provider got poisoned after about ten runs (KERNEL-NVMEOF-
//  STATUS.md), and a test that needs a reboot after every attempt costs hours.
// ---------------------------------------------------------------------------
static void LoopTeardown(void)
{
    if (g_loopTornDown) {
        return;
    }
    g_loopTornDown = TRUE;

    if (g_accepted != NULL) {
        StepStart(&g_closeCtx[0]);
        (void)g_accepted->Dispatch->NdkCloseConnector(g_accepted, closeDone1, &g_closeCtx[0]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[0], 10000);
        logf("teardown: accepted connector closed");
        g_accepted = NULL;
        g_incoming = NULL;
    }
    if (g_connector != NULL) {
        StepStart(&g_closeCtx[1]);
        (void)g_connector->Dispatch->NdkCloseConnector(g_connector, closeDone1, &g_closeCtx[1]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[1], 10000);
        logf("teardown: initiator connector closed");
        g_connector = NULL;
    }
    if (g_listener != NULL) {
        StepStart(&g_closeCtx[2]);
        (void)g_listener->Dispatch->NdkCloseListener(g_listener, closeDone1, &g_closeCtx[2]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[2], 10000);
        logf("teardown: listener closed");
        g_listener = NULL;
    }
    if (g_lqp != NULL) {
        StepStart(&g_closeCtx[3]);
        (void)g_lqp->Dispatch->NdkCloseQp(g_lqp, closeDone1, &g_closeCtx[3]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[3], 10000);
        logf("teardown: listener qp closed");
        g_lqp = NULL;
    }
    if (g_qp != NULL) {
        StepStart(&g_closeCtx[4]);
        (void)g_qp->Dispatch->NdkCloseQp(g_qp, closeDone1, &g_closeCtx[4]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[4], 10000);
        logf("teardown: initiator qp closed");
        g_qp = NULL;
    }
    if (g_lcq != NULL) {
        StepStart(&g_closeCtx[5]);
        (void)g_lcq->Dispatch->NdkCloseCq(g_lcq, closeDone1, &g_closeCtx[5]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[5], 10000);
        logf("teardown: listener cq closed");
        g_lcq = NULL;
    }
    if (g_cq != NULL) {
        StepStart(&g_closeCtx[6]);
        (void)g_cq->Dispatch->NdkCloseCq(g_cq, closeDone1, &g_closeCtx[6]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[6], 10000);
        logf("teardown: initiator cq closed");
        g_cq = NULL;
    }
    if (g_lmr != NULL) {
        StepStart(&g_closeCtx[7]);
        (void)g_lmr->Dispatch->NdkCloseMr(g_lmr, closeDone1, &g_closeCtx[7]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[7], 10000);
        logf("teardown: listener mr closed");
        g_lmr = NULL;
    }
    if (g_mr != NULL) {
        StepStart(&g_closeCtx[8]);
        (void)g_mr->Dispatch->NdkCloseMr(g_mr, closeDone1, &g_closeCtx[8]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[8], 10000);
        logf("teardown: initiator mr closed");
        g_mr = NULL;
    }
    if (g_lpd != NULL) {
        StepStart(&g_closeCtx[9]);
        (void)g_lpd->Dispatch->NdkClosePd(g_lpd, closeDone1, &g_closeCtx[9]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[9], 10000);
        logf("teardown: listener pd closed");
        g_lpd = NULL;
    }
    if (g_pd != NULL) {
        StepStart(&g_closeCtx[10]);
        (void)g_pd->Dispatch->NdkClosePd(g_pd, closeDone1, &g_closeCtx[10]);
        (void)StepFinish(STATUS_PENDING, &g_closeCtx[10], 10000);
        logf("teardown: initiator pd closed");
        g_pd = NULL;
    }

    // Only now can the buffers go: every MR that described them is closed.
    if (g_lmdl != NULL) { MmUnlockPages(g_lmdl); IoFreeMdl(g_lmdl); g_lmdl = NULL; }
    if (g_lregion != NULL) { ExFreePoolWithTag(g_lregion, NVMEOFK_TAG); g_lregion = NULL; }
    if (g_mdl != NULL) { MmUnlockPages(g_mdl); IoFreeMdl(g_mdl); g_mdl = NULL; }
    if (g_region != NULL) { ExFreePoolWithTag(g_region, NVMEOFK_TAG); g_region = NULL; }

    if (g_ladapter != NULL) {
        g_ndk.WskCloseNdkAdapter(g_wskProvider.Client, g_ladapter);
        logf("teardown: listener adapter closed (ifIndex %u)", g_listenIf);
        g_ladapter = NULL;
    }
    if (g_adapter != NULL) {
        g_ndk.WskCloseNdkAdapter(g_wskProvider.Client, g_adapter);
        logf("teardown: initiator adapter closed");
        g_adapter = NULL;
    }

    writeLog();
    logf("teardown complete: listener, accepted connection and initiator all closed; adapters returned");
    writeLog();
}

// History, kept short because it cost fourteen bugchecks to learn:
//
//   The version of this file before the rewrite above did NOT close anything, on the
//   theory that D0 - the kernel driver in this project that really did complete an RDMA
//   read on this hardware - never closed an NDK object either and never bugchecked.
//
//   That reasoning was wrong in one specific way.  D0 connected SUCCESSFULLY and its
//   connection ended cleanly, so nothing was left behind.  This driver also had to
//   survive a connection that FAILED, and a failed connection leaves RDMA-CM state
//   inside mlx4eth63.  About ninety seconds later that state trips
//   KERNEL_SECURITY_CHECK_FAILURE (0x139, P1 = 0xa, FAST_FAIL_INCORRECT_STACK), which
//   is uninspectable precisely because the stack it reports cannot be unwound.  Every
//   delayed bugcheck in this project had that shape.
//
//   So: close the connector the moment a connect fails (closeConnectorNow, above), and
//   close everything on the way out (this function).  Both use closeDone1, whose
//   signature matches NDK_FN_CLOSE_COMPLETION - one Context, no status - because the
//   old two-parameter callback was a type mismatch against every close method.


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

    // Deliberately nothing that touches the registry.  On 2026-10-17 a DriverUnload that
    // wrote to the hive wedged this machine: the SCM waited for the unload and the write
    // never returned, no bugcheck and no dump, and only a reboot recovered it.  NvmeofkTeardown
    // calls writeLog, which is a ZwSetValueKey, so it must not run from here.  The driver is
    // being unloaded; the NDK objects it leaked are the process's problem anyway, and the
    // log that matters was written during the run.
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    NTSTATUS st;

    UNREFERENCED_PARAMETER(registryPath);
    driver->DriverUnload = nvmeofkUnload;

    g_listenIf = regDword(L"nvmeofk_lifindex", 0);
    g_lport    = regDword(L"nvmeofk_lport", NVMEOFK_DEFAULT_PORT);
    g_lconnect = (regDword(L"nvmeofk_lconnect", 0) != 0);
    g_loopback = g_lconnect;

    // ---- D3: listener on one port of the card, initiator on the other ------
    if (regDword(L"nvmeofk_dolisten", 0) != 0) {
        logf("=== nvmeofk D3: NDK listener and same-card loopback ===");
        logf("listener ifIndex=%u port=%u; initiator ifIndex=%u; connect=%s",
             g_listenIf, g_lport,
             regDword(L"nvmeofk_ifindex", 0),
             g_lconnect ? "YES" : "no (listener only)");

        st = wskInit();
        if (!NT_SUCCESS(st)) {
            logf("wskInit failed 0x%08X", st);
            writeLog();
            return STATUS_SUCCESS;
        }
        st = createListenerSide();
        if (!NT_SUCCESS(st)) {
            logf("createListenerSide failed 0x%08X", st);
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }
        if (!g_lconnect) {
            logf("listener-only run: nvmeofk_lconnect is 0, so nothing is connected to it");
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }

        st = openAdapter();
        if (!NT_SUCCESS(st)) {
            logf("initiator adapter failed 0x%08X", st);
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }
        st = createObjects();
        if (!NT_SUCCESS(st)) {
            logf("createObjects failed 0x%08X", st);
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }
        st = registerRegion();
        if (!NT_SUCCESS(st)) {
            logf("registerRegion failed 0x%08X", st);
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }
        st = connectToTarget();
        if (!NT_SUCCESS(st)) {
            logf("connectToTarget failed 0x%08X", st);
            LoopTeardown();
            writeLog();
            return STATUS_SUCCESS;
        }
        st = nvmeofRun();
        logf("nvmeofRun = 0x%08X%s", st, NT_SUCCESS(st) ? "  (NVME-OF OVER SAME-CARD RDMA: OK)" : "");
        LoopTeardown();
        writeLog();
        return STATUS_SUCCESS;
    }

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
}
