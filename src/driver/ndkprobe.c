// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  D0 step 1: can a kernel driver open an ND adapter at all?
//
//  WHY THIS FILE EXISTS.  Everything in this repository so far talks to the NIC through
//  NDSPI - the USER-mode NetworkDirect interface (IND2Adapter, IND2QueuePair, ...).  The
//  kernel equivalent is NDKPI, and it is a completely different interface with a different
//  way in.  This probe answers the one question that decides whether the kernel route (D1,
//  a StorPort miniport) exists at all, and it answers it in one load: either an NDK adapter
//  comes back or it does not.
//
//  THE WAY IN, read out of the WDK headers rather than guessed (they are all in
//  C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km):
//
//    ndkpi.h     - the PROVIDER side: NDK_FN_* dispatch tables, NDK_ADAPTER, NDK_CQ, NDK_QP,
//                  NDK_MR, NDK_SGE...  It contains NO client entry point, which is why the
//                  first attempt to find "NdkOpenAdapter" found nothing.
//    ndisNDK.h   - the miniport side: NDIS_NDK_PROVIDER_CHARACTERISTICS, whose
//                  OpenNDKAdapterHandler is what a NIC driver registers.
//    wskndk.h    - THE CLIENT SIDE: NDK (kernel-mode RDMA) is exposed as an EXTENSION of
//                  WSK (Winsock Kernel) - WSK_PROVIDER_NDK_DISPATCH { WskOpenNdkAdapter,
//                  WskCloseNdkAdapter }, fetched with the control code
//                  WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH = ((ULONG)'NDKD').
//
//  So the sequence is: register as a WSK client, capture the provider NPI, ask it for the
//  'NDKD' extension, then WskOpenNdkAdapter(Client, NdkVersion, IfIndex, &adapter).  This is
//  the same path SMB Direct takes in the kernel.
//
//  WHY IT WRITES A FILE INSTEAD OF DbgPrint.  DbgPrint goes nowhere without a kernel
//  debugger attached, and "attach WinDbg" is not something this machine can be asked to do
//  casually.  The driver therefore writes its findings to a plain file with ZwWriteFile, which
//  PowerShell can read.  Kernel file I/O at PASSIVE_LEVEL in DriverEntry is ordinary work.
//
//  Build (no MSBuild integration needed, see DRIVER-D0.md 7):
//    cl /nologo /c /kernel /GS- /W4 /D_AMD64_ /I"<kits>\Include\10.0.26100.0\km" ^
//       /I"<kits>\Include\10.0.26100.0\shared" ndkprobe.c /Fo:ndkprobe.obj
//    link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
//       /LIBPATH:"<kits>\Lib\10.0.26100.0\km\x64" ndkprobe.obj ntoskrnl.lib hal.lib ^
//       /OUT:ndkprobe.sys
// ---------------------------------------------------------------------------
#define NTSTRSAFE_LIB      // use ntstrsafe.lib's implementation, not an inlined CRT vsnprintf
#include <ntddk.h>
#include <ntstrsafe.h>
#include <ndkpi.h>
#include <wsk.h>
#include <wskndk.h>

// PROBE_LEVEL 1 = register, capture the WSK provider and ask for the NDK dispatch.  Nothing is
// created, no callback is handed to anybody: the smallest thing that can still answer "does this
// machine let a kernel driver reach NDKPI at all".
// PROBE_LEVEL 2 adds the adapter query and the NDK object creation (Pd, Cqs, Qp, Listener,
// Connector).  That is the level that hands callbacks to the provider, which is why it is not the
// one to try first.
#ifndef PROBE_LEVEL
#define PROBE_LEVEL 2
#endif

#define PROBE_LOG_PATH L"\\??\\C:\\ndprobe-result.txt"

// The interfaces to try.  These are the two CX3 Pro ports this project measured on
// (192.168.100.2 = ifIndex 36, 192.168.100.3 = ifIndex 41); a probe that cannot be told
// which adapter to use is not a probe.
static const ULONG kIfIndexes[] = { 36, 41 };

// The NDK versions to try, newest first.  The struct's exact field names are in ndkpi.h; if
// the provider wants an older interface it will say so, and that answer is worth having.
static const USHORT kMajorVersions[] = { 2, 1 };

static char     g_log[8192];
static size_t   g_len = 0;

static void logf(const char* fmt, ...) {
    if (g_len >= sizeof(g_log) - 2) return;
    va_list args;
    va_start(args, fmt);
    RtlStringCbVPrintfA(g_log + g_len, sizeof(g_log) - g_len, fmt, args);
    va_end(args);
    g_len = strlen(g_log);
}

// One write at the end, not a line per step: fewer moving parts, and the whole picture
// arrives together even if a later step faults.
static void writeLog(void) {
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, PROBE_LOG_PATH);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) { DbgPrint("[ndkprobe] cannot open the log: 0x%08X\n", st); return; }
    ZwWriteFile(h, NULL, NULL, NULL, &iosb, g_log, (ULONG)g_len, NULL, NULL);
    ZwClose(h);
    DbgPrint("[ndkprobe] wrote %s\n", "C:\\ndprobe-result.txt");
}

static WSK_REGISTRATION            g_registration;
static WSK_CLIENT_NPI               g_client;
static WSK_CLIENT_DISPATCH          g_clientDispatch;
static WSK_PROVIDER_NPI             g_provider;
static WSK_PROVIDER_NDK_DISPATCH    g_ndk;    // the STRUCT, not a pointer: Microsoft's own sample (NdkWrapper.c) passes sizeof(struct) and the provider validates that size - passing sizeof(pointer) is what returned STATUS_INVALID_PARAMETER
static NDK_ADAPTER*                 g_adapter;

static void cleanup(void);          // defined below; NdProbeUnload is above it on purpose

static void NdProbeUnload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    cleanup();          // idempotent: the adapter is closed once, then the NPI released
    DbgPrint("[ndkprobe] unloaded\n");
}

// ---------------------------------------------------------------------------
//  NDK object creation, in the shape Microsoft's own sample uses (NdkWrapper.c in the
//  published SDN tree).  Every NDK create can complete asynchronously, so each one gets a
//  KEVENT plus a completion callback and is turned into a synchronous call.  This is not
//  ceremony: a create that returns STATUS_PENDING and is never waited on leaves a driver
//  that loads, prints nothing, and looks exactly like a hang.
// ---------------------------------------------------------------------------
typedef struct _PROBE_ASYNC {
    KEVENT            Event;
    NTSTATUS          Status;
    NDK_OBJECT_HEADER* Object;
} PROBE_ASYNC;

static VOID ProbeCreateCompletion(PVOID context, NTSTATUS status, NDK_OBJECT_HEADER* obj) {
    PROBE_ASYNC* c = (PROBE_ASYNC*)context;
    c->Status = status;
    c->Object = obj;
    KeSetEvent(&c->Event, IO_NO_INCREMENT, FALSE);
}

// NDKPI declares the close completion routines _In_ (not optional), so passing NULL is asking for
// trouble; Microsoft's own sample carries a do-nothing callback for exactly this.  The closes in
// cleanup() use it.
static VOID ProbeDoNothing(PVOID context) { UNREFERENCED_PARAMETER(context); }

static VOID ProbeStartCreate(PROBE_ASYNC* c) {
    KeInitializeEvent(&c->Event, NotificationEvent, FALSE);
    c->Status = STATUS_SUCCESS;
    c->Object = NULL;
}

// Waits only when the provider said PENDING; otherwise the returned status is the answer.
static NTSTATUS ProbeFinishCreate(NTSTATUS status, PROBE_ASYNC* c) {
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&c->Event, Executive, KernelMode, FALSE, NULL);
        return c->Status;
    }
    return status;
}

static NDK_ADAPTER*     g_adapter;
static NDK_PD*          g_pd;
static NDK_CQ*          g_recvCq;
static NDK_CQ*          g_sendCq;
static NDK_QP*          g_qp;
static NDK_LISTENER*    g_listener;
static NDK_CONNECTOR*   g_connector;

// Steps 1 and 2 of D0: query what the adapter can do, then create the objects a queue pair
// needs.  This is where an NDKPI implementation that is present but unusable shows itself:
// opening an adapter can succeed while creating a CQ or a QP fails.
static void probeAdapterObjects(void) {
    NTSTATUS st;

    NDK_ADAPTER_INFO info;
    ULONG infoSize = sizeof(info);
    RtlZeroMemory(&info, sizeof(info));
    st = g_adapter->Dispatch->NdkQueryAdapterInfo(g_adapter, &info, &infoSize);
    logf("NdkQueryAdapterInfo                = 0x%08X  (%u bytes of %u)\r\n",
         st, (unsigned)infoSize, (unsigned)sizeof(info));
    if (NT_SUCCESS(st)) {
        logf("  provider %u.%u  vendor 0x%04X device 0x%04X\r\n",
             info.Version.Major, info.Version.Minor, info.VendorId, info.DeviceId);
        logf("  MaxRegistrationSize=%llu MiB  MaxWindowSize=%llu MiB  MaxTransferLength=%u KiB\r\n",
             (unsigned long long)(info.MaxRegistrationSize >> 20),
             (unsigned long long)(info.MaxWindowSize >> 20),
             info.MaxTransferLength / 1024);
        logf("  MaxInboundReadLimit=%u  MaxOutboundReadLimit=%u  FRMRPageCount=%u\r\n",
             info.MaxInboundReadLimit, info.MaxOutboundReadLimit, info.FRMRPageCount);
        logf("  MaxReadRequestSge=%u  MaxInitiatorRequestSge=%u  MaxReceiveRequestSge=%u\r\n",
             info.MaxReadRequestSge, info.MaxInitiatorRequestSge, info.MaxReceiveRequestSge);
        logf("  MaxCqDepth=%u  MaxInitiatorQueueDepth=%u  MaxReceiveQueueDepth=%u\r\n",
             info.MaxCqDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveQueueDepth);
    }

    PROBE_ASYNC c;

    ProbeStartCreate(&c);
    st = g_adapter->Dispatch->NdkCreatePd(g_adapter, ProbeCreateCompletion, &c, &g_pd);
    st = ProbeFinishCreate(st, &c);
    if (NT_SUCCESS(st) && !g_pd) g_pd = (NDK_PD*)c.Object;
    logf("NdkCreatePd                        = 0x%08X  pd=%p\r\n", st, g_pd);

    ProbeStartCreate(&c);
    st = g_adapter->Dispatch->NdkCreateCq(g_adapter, 64, NULL, NULL, NULL,
                                          ProbeCreateCompletion, &c, &g_recvCq);
    st = ProbeFinishCreate(st, &c);
    if (NT_SUCCESS(st) && !g_recvCq) g_recvCq = (NDK_CQ*)c.Object;
    logf("NdkCreateCq (receive, depth 64)     = 0x%08X  cq=%p\r\n", st, g_recvCq);

    ProbeStartCreate(&c);
    st = g_adapter->Dispatch->NdkCreateCq(g_adapter, 64, NULL, NULL, NULL,
                                          ProbeCreateCompletion, &c, &g_sendCq);
    st = ProbeFinishCreate(st, &c);
    if (NT_SUCCESS(st) && !g_sendCq) g_sendCq = (NDK_CQ*)c.Object;
    logf("NdkCreateCq (send, depth 64)        = 0x%08X  cq=%p\r\n", st, g_sendCq);

    if (g_pd && g_recvCq && g_sendCq) {
        ProbeStartCreate(&c);
        st = g_pd->Dispatch->NdkCreateQp(g_pd, g_recvCq, g_sendCq, NULL,
                                         16, 16, 4, 4, 0,
                                         ProbeCreateCompletion, &c, &g_qp);
        st = ProbeFinishCreate(st, &c);
        if (NT_SUCCESS(st) && !g_qp) g_qp = (NDK_QP*)c.Object;
        logf("NdkCreateQp (16 recv / 16 send)     = 0x%08X  qp=%p\r\n", st, g_qp);
    } else {
        logf("NdkCreateQp                         = skipped (needs pd and both cqs)\r\n");
    }

    if (g_adapter && g_qp) {
        ProbeStartCreate(&c);
        st = g_adapter->Dispatch->NdkCreateListener(g_adapter, NULL, NULL,
                                                    ProbeCreateCompletion, &c, &g_listener);
        st = ProbeFinishCreate(st, &c);
        if (NT_SUCCESS(st) && !g_listener) g_listener = (NDK_LISTENER*)c.Object;
        logf("NdkCreateListener                   = 0x%08X  listener=%p\r\n", st, g_listener);

        ProbeStartCreate(&c);
        st = g_adapter->Dispatch->NdkCreateConnector(g_adapter, ProbeCreateCompletion, &c, &g_connector);
        st = ProbeFinishCreate(st, &c);
        if (NT_SUCCESS(st) && !g_connector) g_connector = (NDK_CONNECTOR*)c.Object;
        logf("NdkCreateConnector                  = 0x%08X  connector=%p\r\n", st, g_connector);
    }
}

static void cleanup(void) {
#if PROBE_LEVEL >= 2
    if (g_listener) { g_listener->Dispatch->NdkCloseListener(&g_listener->Header, ProbeDoNothing, NULL); g_listener = NULL; }
    if (g_connector) { g_connector->Dispatch->NdkCloseConnector(&g_connector->Header, ProbeDoNothing, NULL); g_connector = NULL; }
    if (g_qp) { g_qp->Dispatch->NdkCloseQp(&g_qp->Header, ProbeDoNothing, NULL); g_qp = NULL; }
    if (g_recvCq) { g_recvCq->Dispatch->NdkCloseCq(&g_recvCq->Header, ProbeDoNothing, NULL); g_recvCq = NULL; }
    if (g_sendCq) { g_sendCq->Dispatch->NdkCloseCq(&g_sendCq->Header, ProbeDoNothing, NULL); g_sendCq = NULL; }
    if (g_pd) { g_pd->Dispatch->NdkClosePd(&g_pd->Header, ProbeDoNothing, NULL); g_pd = NULL; }
#endif
    if (g_ndk.WskOpenNdkAdapter && g_adapter) {
        g_ndk.WskCloseNdkAdapter(g_provider.Client, g_adapter);
        g_adapter = NULL;
    }
    if (g_provider.Client) {
        WskReleaseProviderNPI(&g_registration);
        g_provider.Client = NULL;
    }
    WskDeregister(&g_registration);
}

static void probe(void) {
    logf("ndkprobe: D0 step 1 - can a kernel driver open an ND adapter?\r\n\r\n");

    RtlZeroMemory(&g_clientDispatch, sizeof(g_clientDispatch));
    g_clientDispatch.Version = MAKE_WSK_VERSION(1, 0);   // wsk.h:65
    g_client.ClientContext = NULL;
    g_client.Dispatch      = &g_clientDispatch;

    NTSTATUS st = WskRegister(&g_client, &g_registration);
    logf("WskRegister                        = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st)) return;

    // 0xFFFFFFFF = wait indefinitely: the ND provider is loaded on demand, and the first
    // load on a cold machine is exactly the case a short timeout would turn into a false
    // "not supported".
    st = WskCaptureProviderNPI(&g_registration, 0xFFFFFFFF, &g_provider);
    logf("WskCaptureProviderNPI              = 0x%08X", st);
    if (!NT_SUCCESS(st)) { logf("   <- no WSK provider at all\r\n"); return; }
    logf("   client=%p dispatch=%p\r\n", g_provider.Client, g_provider.Dispatch);
    if (!g_provider.Dispatch) { logf("provider dispatch is NULL\r\n"); return; }

    // OutputSizeReturned must NOT be NULL: wsk.h documents that for the query form of
    // WskControlClient ("Irp must be NULL and pOutputSize must be Non-NULL"), and passing
    // NULL is what produced STATUS_INVALID_PARAMETER on the first run - a parameter error
    // being read as "not supported" would have ended this investigation one step too early.
    RtlZeroMemory(&g_ndk, sizeof(g_ndk));
    st = g_provider.Dispatch->WskControlClient(
             g_provider.Client,
             WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH,      // ((ULONG)'NDKD')
             0, NULL,
             sizeof(g_ndk), &g_ndk,
             NULL, NULL);
    logf("WskControlClient('NDKD')           = 0x%08X   Open=%p Close=%p\r\n",
         st, g_ndk.WskOpenNdkAdapter, g_ndk.WskCloseNdkAdapter);
    if (!NT_SUCCESS(st) || !g_ndk.WskOpenNdkAdapter) {
        logf("\r\nANSWER: this machine has a WSK provider but it does NOT expose the NDK\r\n"
             "        extension.  WskOpenNdkAdapter is therefore unreachable and the kernel\r\n"
             "        route cannot start here.  That is a D0 answer, not a failure.\r\n");
        return;
    }

    for (unsigned v = 0; v < sizeof(kMajorVersions) / sizeof(kMajorVersions[0]) && !g_adapter; v++) {
        for (unsigned i = 0; i < sizeof(kIfIndexes) / sizeof(kIfIndexes[0]) && !g_adapter; i++) {
            NDK_VERSION ver;
            RtlZeroMemory(&ver, sizeof(ver));
            ver.Major = kMajorVersions[v];
            ver.Minor = 0;
            NDK_ADAPTER* adapter = NULL;
            st = g_ndk.WskOpenNdkAdapter(g_provider.Client, ver, (NET_IFINDEX)kIfIndexes[i], &adapter);
            logf("WskOpenNdkAdapter(v%u.%u, if%u)%-6s = 0x%08X   adapter=%p\r\n",
                 ver.Major, ver.Minor, kIfIndexes[i], "", st, adapter);
            if (NT_SUCCESS(st) && adapter) { g_adapter = adapter; break; }
        }
    }

    if (g_adapter) {
#if PROBE_LEVEL >= 2
        probeAdapterObjects();
#endif
        logf("\r\nANSWER: an NDK adapter was OPENED through WSK.  D0 step 1 succeeds, and the\r\n"
             "        next steps (create CQ/QP, connect, one RDMA Read compared byte for byte)\r\n"
             "        are worth building.\r\n");
    } else {
        logf("\r\nANSWER: the NDK extension exists but no adapter could be opened on those\r\n"
             "        interfaces.  Read the status codes above before concluding anything: a\r\n"
             "        version mismatch and a missing ND provider look different here.\r\n");
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);
    driver->DriverUnload = NdProbeUnload;   // so the probe can be stopped and deleted
    RtlZeroMemory(g_log, sizeof(g_log));
    g_len = 0;
    __try {
        probe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("\r\nEXCEPTION 0x%08X during the probe\r\n", GetExceptionCode());
    }
    writeLog();
    cleanup();
    return STATUS_SUCCESS;
}
