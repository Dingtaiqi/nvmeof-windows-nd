// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  D0 step 3: connect two NDK endpoints inside one kernel driver.
//
//  WHAT THIS ADDS OVER ndkprobe.c.  That file proves an adapter can be opened and NDK objects
//  created (steps 1-2, measured).  This one proves a CONNECTION: adapter on 192.168.100.2
//  listens, adapter on 192.168.100.3 connects, the two exchange private data, and both sides
//  report the read limits they negotiated.  Step 4 (one RDMA Read compared byte for byte) is
//  the next file; it needs everything here first.
//
//  THE IRQL CONSTRAINT IS THE SHAPE OF THIS FILE, not an implementation detail.  A listener's
//  connect-event callback runs at DISPATCH_LEVEL, while NdkCreateConnector / NdkGetConnectionData
//  / NdkAccept are declared _IRQL_requires_max_(APC_LEVEL) or PASSIVE_LEVEL.  Calling them
//  straight from the callback would be a bug that only shows up under load, so the callback does
//  exactly one thing - queue a work item - and every NDK call happens in that work item at
//  PASSIVE_LEVEL.  Microsoft's own sample does the same thing by queueing a disconnect work item
//  from its callback.
//
//  WHY BOTH ENDS LIVE IN ONE DRIVER.  A first connect test wants no second machine, no
//  cooperating user-mode peer and no protocol: two adapters on one card, cabled to each other,
//  are enough to answer "does a connection complete and what limits come out of it".
//
//  Build (hand-rolled; /guard:cf and /GUARD:CF are NOT optional - see DRIVER-D0.md 12-14):
//    cl /nologo /c /kernel /GS- /guard:cf /W4 /D_AMD64_ /I"<kits>\Include\10.0.26100.0\km" ^
//       /I"<kits>\Include\10.0.26100.0\shared" ndkstep3.c /Fo:ndkstep3.obj
//    link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 /GUARD:CF ^
//       /LIBPATH:"<kits>\Lib\10.0.26100.0\km\x64" ndkstep3.obj ntoskrnl.lib hal.lib ^
//       ntstrsafe.lib netio.lib bufferoverflowfastfailk.lib /OUT:ndkstep3.sys
//  Then sign it (test mode wants a signature, not just "unsigned allowed"):
//    signtool sign /fd sha256 /s My /n "NVMeoF Test Driver" ndkstep3.sys
// ---------------------------------------------------------------------------
#define NTSTRSAFE_LIB
#include <ntddk.h>
#include <ntstrsafe.h>
#include <ndkpi.h>
#include <wsk.h>
#include <wskndk.h>

#define STEP3_LOG_PATH L"\\??\\C:\\ndkstep3-result.txt"

static const ULONG kServerIf = 36;      // 192.168.100.2
static const ULONG kClientIf = 41;      // 192.168.100.3
// The port is read from the service's Parameters key so a run can use a fresh one.  That is not
// ceremony: sc stop does NOT unload this driver (the old instance keeps listening and holding its
// port), so a second load on the same port answers NdkListen with 0xC0000043 STATUS_SHARING_VIOLATION.
// Reboots are not a debugging tool on somebody's working machine, so the port moves instead.
#define STEP3_DEFAULT_PORT 18520
static USHORT kPort = STEP3_DEFAULT_PORT;

// A second switch, next to the port: ServerSide.  With it set to 0 the driver skips the listener and
// acts only as the client, which is what makes ISOLATION possible - a kernel client against the
// user-mode listener that is already known to work, and then the reverse.  Without that split, "the
// connect is refused" cannot be attributed to either side, and the last three rounds were spent
// changing one side at a time while both were in play.
static ULONG Step3ConfigDword(const wchar_t* valueName, ULONG dflt) {
    UNICODE_STRING name, value;
    RtlInitUnicodeString(&name, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    RtlInitUnicodeString(&value, valueName);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) return dflt;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    ULONG len = 0;
    NTSTATUS st = ZwQueryValueKey(h, &value, KeyValuePartialInformation, buf, sizeof(buf), &len);
    ZwClose(h);
    if (!NT_SUCCESS(st)) return dflt;
    KEY_VALUE_PARTIAL_INFORMATION* info = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
    if (info->Type != REG_DWORD || info->DataLength < 4) return dflt;
    return *(ULONG*)info->Data;
}

static USHORT Step3ReadPort(void) {
    UNICODE_STRING name, value;
    RtlInitUnicodeString(&name, L"\\Registry\\Machine\\Software\\NVMeoFProbe");
    RtlInitUnicodeString(&value, L"Port");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) return STEP3_DEFAULT_PORT;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    ULONG len = 0;
    NTSTATUS st = ZwQueryValueKey(h, &value, KeyValuePartialInformation, buf, sizeof(buf), &len);
    ZwClose(h);
    if (!NT_SUCCESS(st)) return STEP3_DEFAULT_PORT;
    KEY_VALUE_PARTIAL_INFORMATION* info = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
    if (info->Type != REG_DWORD || info->DataLength < 4) return STEP3_DEFAULT_PORT;
    return (USHORT)(*(ULONG*)info->Data);
}

static char   g_log[8192];
static size_t g_len = 0;

static void logf(const char* fmt, ...) {
    if (g_len >= sizeof(g_log) - 2) return;
    va_list a;
    va_start(a, fmt);
    RtlStringCbVPrintfA(g_log + g_len, sizeof(g_log) - g_len, fmt, a);
    va_end(a);
    g_len = strlen(g_log);
}

static void writeLog(void) {
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, STEP3_LOG_PATH);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) { DbgPrint("[ndkstep3] cannot open the log: 0x%08X\n", st); return; }
    ZwWriteFile(h, NULL, NULL, NULL, &iosb, g_log, (ULONG)g_len, NULL, NULL);
    ZwClose(h);
}

// ---------------------------------------------------------------------------
//  Async-to-sync, the shape Microsoft's sample uses.  Every NDK create/close and every connect
//  step can return STATUS_PENDING and complete later, so each one gets an event and a completion
//  callback.  A create that pends and is never waited on is a driver that loads, prints nothing
//  and looks like a hang.
// ---------------------------------------------------------------------------
typedef struct _STEP3_ASYNC {
    KEVENT   Event;
    NTSTATUS Status;
    PVOID    Object;
} STEP3_ASYNC;

static VOID Step3Complete(PVOID context, NTSTATUS status, NDK_OBJECT_HEADER* obj) {
    STEP3_ASYNC* c = (STEP3_ASYNC*)context;
    c->Status = status;
    c->Object = obj;
    KeSetEvent(&c->Event, IO_NO_INCREMENT, FALSE);
}

static ULONG g_acceptCompletionCount = 0;   // "never called back" vs "called back late" are different bugs
static VOID Step3RequestComplete(PVOID context, NTSTATUS status) {
    STEP3_ASYNC* c = (STEP3_ASYNC*)context;
    c->Status = status;
    g_acceptCompletionCount++;   // "never called back" vs "called back late" are different bugs
    KeSetEvent(&c->Event, IO_NO_INCREMENT, FALSE);
}

static VOID Step3DoNothing(PVOID context) { UNREFERENCED_PARAMETER(context); }

static ULONG g_cqNotifications = 0;
// CQs are created WITH a notification callback here, where earlier versions passed NULL - and the
// signature says why that mattered: the parameter is declared _In_, not _In_opt_, while
// CqNotificationContext right after it IS optional.  Microsoft's sample supplies a real routine.
// This probe polls its CQs and the callback does nothing useful, but "the parameter is not optional"
// is exactly the kind of thing that is worth aligning before blaming the provider for a completion
// that never arrives.
static VOID Step3CqNotification(PVOID context, NTSTATUS cqStatus) {
    UNREFERENCED_PARAMETER(cqStatus);
    if (context) InterlockedIncrement((volatile LONG*)context);
}

static void Step3Start(STEP3_ASYNC* c) {
    KeInitializeEvent(&c->Event, NotificationEvent, FALSE);
    c->Status = STATUS_SUCCESS;
    c->Object = NULL;
}

static NTSTATUS Step3Finish(NTSTATUS status, STEP3_ASYNC* c, ULONG waitMs) {
    if (status != STATUS_PENDING) return status;
    LARGE_INTEGER timeout;
    timeout.QuadPart = -((LONGLONG)waitMs * 10000);      // relative, 100 ns units
    NTSTATUS w = KeWaitForSingleObject(&c->Event, Executive, KernelMode, FALSE, &timeout);
    if (w == STATUS_TIMEOUT) return STATUS_IO_TIMEOUT;
    return c->Status;
}

// ---------------------------------------------------------------------------
//  Two endpoints, each with its own adapter and objects.
// ---------------------------------------------------------------------------
typedef struct _STEP3_END {
    const char*        Name;
    ULONG              IfIndex;
    NDK_ADAPTER*       Adapter;
    NDK_PD*            Pd;
    NDK_CQ*            Cq;          // receive completions
    NDK_CQ*            SendCq;      // initiator completions - SEPARATE, as in the working L2 probe
    NDK_QP*            Qp;
    NDK_LISTENER*      Listener;
    NDK_CONNECTOR*     Connector;
} STEP3_END;

static STEP3_END g_server;
static STEP3_END g_client;

static WSK_REGISTRATION          g_registration;
static WSK_CLIENT_NPI            g_wskClient;
static WSK_CLIENT_DISPATCH       g_wskClientDispatch;
static WSK_PROVIDER_NPI          g_wskProvider;
static WSK_PROVIDER_NDK_DISPATCH g_ndk;      // the STRUCT - sizeof(struct) is what the provider wants

// A WORK_QUEUE_ITEM, not IoAllocateWorkItem: there is no device object here, and
// IoAllocateWorkItem(NULL) fails - which would leave the listener's callback queueing nothing and
// the server never accepting.  ExInitializeWorkItem/ExQueueWorkItem need no device.
static WORK_QUEUE_ITEM g_acceptWorkItem;
static KEVENT          g_acceptPosted;       // set when the accept has been POSTED
static KEVENT          g_acceptFinished;     // set when the accept has COMPLETED
static NDK_CONNECTOR*  g_incoming;           // the connector the listener handed us

// What the two sides exchange over the connection.  Magic so a mismatch is loud rather than a
// silent misread of somebody else's bytes.
#define STEP3_PRIV_MAGIC 0x4E564D46u         // "NVMF"
typedef struct _STEP3_PRIV {
    ULONG   Magic;          // "NVMF"
    ULONG   Rkey;           // the MR token - step 4 needs this to travel
    ULONG64 Address;        // the buffer address - and this
    ULONG   Marker;         // a second recognisable value, so "arrived" is unambiguous
    ULONG   Pad;            // keep the size a round 32 bytes
} STEP3_PRIV;

static STEP3_PRIV g_serverPriv;
static STEP3_PRIV g_clientPriv;
// The provider delivers private data through an _Inout_ length: the caller passes the CAPACITY and
// the provider writes back the real size, returning STATUS_BUFFER_TOO_SMALL if 16 bytes were not
// enough.  Measured: it answered 0xC0000023 with the real size 56, so this is a real buffer now, not
// a struct the size of what we hoped to receive.
static UCHAR    g_privBuf[256];
static STEP3_ASYNC g_acceptCtx;              // static: the accept completion arrives later, not now
static NTSTATUS    g_acceptCallStatus = STATUS_SUCCESS;
static ULONG       g_privLenInCallback = 0;
static NTSTATUS    g_privStatusInCallback = STATUS_SUCCESS;
static ULONG       g_peerInLimit = 0;
static ULONG       g_peerOutLimit = 0;
static NTSTATUS g_serverAcceptStatus   = STATUS_SUCCESS;
static NTSTATUS g_clientCompleteStatus = STATUS_SUCCESS;
static NTSTATUS g_clientConnectStatus  = STATUS_SUCCESS;

// ---------------------------------------------------------------------------
//  The connect-event callback.  DISPATCH_LEVEL: queue and return, nothing else.
// ---------------------------------------------------------------------------
static VOID Step3AcceptWorker(PVOID ctx);        // forward: the callback queues it

static VOID Step3ConnectEvent(PVOID context, NDK_CONNECTOR* connector) {
    UNREFERENCED_PARAMETER(context);
    g_incoming = connector;

    // The accept is NOT posted here any more.  The previous version posted it in this callback (which
    // is legal - NdkAccept is _IRQL_requires_max_(DISPATCH_LEVEL)), the call returned STATUS_PENDING,
    // and its completion then never arrived, before OR after the client completed its side.  The next
    // hypothesis to test is the opposite order: let the client finish first, then accept from the main
    // thread at PASSIVE_LEVEL.  One variable changes per run.
    ULONG inLimit = 0, outLimit = 0;
    ULONG len = sizeof(g_privBuf);
    RtlZeroMemory(g_privBuf, sizeof(g_privBuf));
    g_privStatusInCallback = connector->Dispatch->NdkGetConnectionData(connector, &inLimit, &outLimit,
                                                                      g_privBuf, &len);
    g_privLenInCallback = len;
    g_peerInLimit = inLimit;
    g_peerOutLimit = outLimit;
    // TWO STEPS, as the user-mode code does: NDSPI's GetPrivateData(NULL, &len) returns the required
    // size first (with ND_BUFFER_OVERFLOW), and only then is the buffer filled.  The kernel side used a
    // single call and got SUCCESS with length 0, which may simply mean this provider only reports the
    // data on a properly sized second call - or that the peer really sent nothing.  The size query tells
    // those two apart, and that distinction decides whether step 4 has a channel to exchange rkeys on.
    {
        ULONG need = 0;
        NTSTATUS sq = connector->Dispatch->NdkGetConnectionData(connector, NULL, NULL, NULL, &need);
        logf("  server[callback]: size query               = 0x%08X  need=%u\r\n", sq, need);
    // SECOND CALL USES THE ANNOUNCED SIZE, not the buffer size, and the payload length is now a known
    // 24 bytes on the sending side so the two readings of "56" can be told apart: if this fetch returns
    // 24, then 56 was the provider's ceiling; if it returns 0 again, the announced size was not the issue.
    {
        ULONG need2 = need;
        ULONG got = need2 ? need2 : sizeof(g_privBuf);
        NTSTATUS st9 = connector->Dispatch->NdkGetConnectionData(connector, &inLimit, &outLimit,
                                                                  g_privBuf, &got);
        STEP3_PRIV* px = (STEP3_PRIV*)g_privBuf;
        logf("  server[callback]: fetch(announced=%u)  = 0x%08X got=%u magic=0x%08X rkey=0x%08X addr=0x%llX marker=0x%08X\r\n",
             need, st9, got, px->Magic, px->Rkey, (unsigned long long)px->Address, px->Marker);
    }
    }
    logf("  server[callback]: NdkGetConnectionData      = 0x%08X  peerIn=%u peerOut=%u privLen=%u\r\n",
         g_privStatusInCallback, inLimit, outLimit, len);
    KeSetEvent(&g_acceptPosted, IO_NO_INCREMENT, FALSE);     // the main thread may now accept
    // THE ACCEPT IS POSTED HERE, IN THE CALLBACK, and the measurement is why: with the accept posted
    // from this callback the CLIENT's NdkConnect completed successfully (0x00000000) in an earlier run,
    // while every version that posted the accept from the main thread "after the client completes" had
    // the client REFUSED (0xC0000236) - that ordering is a deadlock of my own making, the client waiting
    // for an accept that is waiting for the client.  NdkAccept is legal at this IRQL by its own declaration.
    Step3Start(&g_acceptCtx);
    {
        NTSTATUS ast = connector->Dispatch->NdkAccept(connector, g_server.Qp, inLimit, outLimit,
                                                      &g_serverPriv, sizeof(g_serverPriv),
                                                      NULL, NULL, Step3RequestComplete, &g_acceptCtx);
        g_acceptCallStatus = ast;
        logf("  server[callback]: NdkAccept posted         = 0x%08X\r\n", ast);
        if (ast != STATUS_PENDING) {
            g_serverAcceptStatus = ast;
            KeSetEvent(&g_acceptPosted, IO_NO_INCREMENT, FALSE);
        }
    }
}

// ---------------------------------------------------------------------------
//  Open one endpoint's adapter and objects.
// ---------------------------------------------------------------------------
static NTSTATUS Step3OpenEnd(STEP3_END* e, ULONG ifIndex, const char* name) {
    e->Name = name;
    e->IfIndex = ifIndex;

    NDK_VERSION ver;
    RtlZeroMemory(&ver, sizeof(ver));
    ver.Major = 2;
    ver.Minor = 0;

    NTSTATUS st = g_ndk.WskOpenNdkAdapter(g_wskProvider.Client, ver, (NET_IFINDEX)ifIndex, &e->Adapter);
    logf("  %s if%u: WskOpenNdkAdapter            = 0x%08X  adapter=%p\r\n", name, ifIndex, st, e->Adapter);
    if (!NT_SUCCESS(st) || !e->Adapter) return st;

        // STATIC ON PURPOSE, and this is the fix for the 0x7E bugcheck this file caused.
    // NdkListen, NdkConnect, NdkCompleteConnect and NdkAccept are ASYNCHRONOUS: they can return
    // STATUS_PENDING and call the completion routine LATER.  This context used to be a plain
    // stack local, so a completion that arrived after the enclosing frame was gone made the
    // provider write into dead stack - memory corruption that surfaced as an access violation
    // inside a worker thread and bugchecked the machine (0x7E, AV_nt!KiStartSystemThread,
    // nt!ExpWorkerThread).  The level-1/level-2 probe survived the same pattern only because
    // those creates happened to complete synchronously.

    static STEP3_ASYNC c;
    Step3Start(&c);
    st = e->Adapter->Dispatch->NdkCreatePd(e->Adapter, Step3Complete, &c, &e->Pd);
    st = Step3Finish(st, &c, 10000);
    if (NT_SUCCESS(st) && !e->Pd) e->Pd = (NDK_PD*)c.Object;
    logf("  %s: NdkCreatePd                       = 0x%08X  pd=%p\r\n", name, st, e->Pd);
    if (!NT_SUCCESS(st)) return st;

    Step3Start(&c);
    st = e->Adapter->Dispatch->NdkCreateCq(e->Adapter, 64, Step3CqNotification, &g_cqNotifications, NULL, Step3Complete, &c, &e->Cq);
    st = Step3Finish(st, &c, 10000);
    if (NT_SUCCESS(st) && !e->Cq) e->Cq = (NDK_CQ*)c.Object;
    logf("  %s: NdkCreateCq                       = 0x%08X  cq=%p\r\n", name, st, e->Cq);
    if (!NT_SUCCESS(st)) return st;

    Step3Start(&c);
    st = e->Pd->Dispatch->NdkCreateQp(e->Pd, e->Cq, e->Cq, NULL, 16, 16, 4, 4, 0,
                                      Step3Complete, &c, &e->Qp);
    st = Step3Finish(st, &c, 10000);
    if (NT_SUCCESS(st) && !e->Qp) e->Qp = (NDK_QP*)c.Object;
    logf("  %s: NdkCreateQp                       = 0x%08X  qp=%p\r\n", name, st, e->Qp);
    return st;
}

// ---------------------------------------------------------------------------
//  Server side: listen, and in the worker accept with our own private data.
// ---------------------------------------------------------------------------
static VOID Step3AcceptWorker(PVOID ctx) {
    UNREFERENCED_PARAMETER(ctx);

    ULONG inLimit = 0, outLimit = 0;
    // The provider delivers private data through an _Inout_ length: the caller passes the CAPACITY
    // and gets back the real size.  Measured: with 16 bytes it answered 0xC0000023
    // (STATUS_BUFFER_TOO_SMALL) and reported the real size as 56, so what arrives here is a real
    // buffer, not a struct the size of what we hoped for.
    ULONG privLen = sizeof(g_privBuf);
    RtlZeroMemory(g_privBuf, sizeof(g_privBuf));

    NTSTATUS st = g_incoming->Dispatch->NdkGetConnectionData(g_incoming, &inLimit, &outLimit,
                                                             g_privBuf, &privLen);
    logf("  server: NdkGetConnectionData           = 0x%08X  peerIn=%u peerOut=%u privLen=%u (capacity %u)\r\n",
         st, inLimit, outLimit, privLen, (unsigned)sizeof(g_privBuf));
    if (NT_SUCCESS(st)) {
        STEP3_PRIV* pd = (STEP3_PRIV*)g_privBuf;
        logf("  server: peer private data              = magic=0x%08X rkey=0x%08X addr=0x%llX\r\n",
             pd->Magic, pd->Rkey, (unsigned long long)pd->Address);
    } else {
        logf("  server: peer private data              = NOT READ (0x%08X); step 4 needs it\r\n", st);
    }

        // STATIC ON PURPOSE, and this is the fix for the 0x7E bugcheck this file caused.
    // NdkListen, NdkConnect, NdkCompleteConnect and NdkAccept are ASYNCHRONOUS: they can return
    // STATUS_PENDING and call the completion routine LATER.  This context used to be a plain
    // stack local, so a completion that arrived after the enclosing frame was gone made the
    // provider write into dead stack - memory corruption that surfaced as an access violation
    // inside a worker thread and bugchecked the machine (0x7E, AV_nt!KiStartSystemThread,
    // nt!ExpWorkerThread).  The level-1/level-2 probe survived the same pattern only because
    // those creates happened to complete synchronously.

    static STEP3_ASYNC c;
    Step3Start(&c);
    st = g_incoming->Dispatch->NdkAccept(g_incoming, g_server.Qp, inLimit, outLimit,
                                         &g_serverPriv, sizeof(g_serverPriv),
                                         NULL, NULL, Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 15000);
    logf("  server: NdkAccept                      = 0x%08X\r\n", st);
    KeSetEvent(&g_acceptPosted, IO_NO_INCREMENT, FALSE);     // posted (or failed): client may proceed
    KeSetEvent(&g_acceptFinished, IO_NO_INCREMENT, FALSE);
}

// ---------------------------------------------------------------------------
//  Client side: connect with our private data, then complete the connect.
// ---------------------------------------------------------------------------
static NTSTATUS Step3ClientConnect(void) {
    SOCKADDR_IN local, remote;
    RtlZeroMemory(&local, sizeof(local));
    RtlZeroMemory(&remote, sizeof(remote));
    local.sin_family = AF_INET;
    // SAME PORT as the listener, which is what the working user-mode pair does: there the client
    // binds 192.168.100.3:<server port> and the server listens on 192.168.100.2:<same port>.  Tried
    // with a separate port first; that is now eliminated, and every other variable has been too.
    // LOCAL PORT 0: the provider allocates the implicit local endpoint itself.
    // The working user-mode client has an explicit IND2Connector::Bind step before Connect; NDKPI has no
    // such call - the documentation says a connector connected via NdkConnect "has its own dedicated
    // IMPLICIT local endpoint", i.e. the provider creates it.  Every previous run passed a concrete port
    // here (the listener's port, then its neighbour); 0 is the one value never tried, and it is what "the
    // provider allocates the endpoint" implies.
    local.sin_port = 0;
    local.sin_addr.s_addr = RtlUlongByteSwap(0x0A64C6A3);      // placeholder, set by caller
    remote.sin_family = AF_INET;
    remote.sin_port = RtlUshortByteSwap(kPort);

        // STATIC ON PURPOSE, and this is the fix for the 0x7E bugcheck this file caused.
    // NdkListen, NdkConnect, NdkCompleteConnect and NdkAccept are ASYNCHRONOUS: they can return
    // STATUS_PENDING and call the completion routine LATER.  This context used to be a plain
    // stack local, so a completion that arrived after the enclosing frame was gone made the
    // provider write into dead stack - memory corruption that surfaced as an access violation
    // inside a worker thread and bugchecked the machine (0x7E, AV_nt!KiStartSystemThread,
    // nt!ExpWorkerThread).  The level-1/level-2 probe survived the same pattern only because
    // those creates happened to complete synchronously.

    static STEP3_ASYNC c;
    Step3Start(&c);
    NTSTATUS st = g_client.Connector->Dispatch->NdkConnect(
        g_client.Connector, g_client.Qp,
        (CONST PSOCKADDR)&local, sizeof(local),
        (CONST PSOCKADDR)&remote, sizeof(remote),
        1, 1,                                                   // inbound / outbound read limits
        &g_clientPriv, sizeof(g_clientPriv),
        Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 15000);
    logf("  client: NdkConnect (local %u)          = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st)) return st;

    Step3Start(&c);
    st = g_client.Connector->Dispatch->NdkCompleteConnect(g_client.Connector, NULL, NULL,
                                                          Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 15000);
    logf("  client: NdkCompleteConnect             = 0x%08X\r\n", st);
    return st;
}

// ---------------------------------------------------------------------------
//  DriverEntry / DriverUnload
// ---------------------------------------------------------------------------
static void Step3Cleanup(void) {
    if (g_server.Connector) { g_server.Connector->Dispatch->NdkCloseConnector(&g_server.Connector->Header, Step3DoNothing, NULL); g_server.Connector = NULL; }
    if (g_client.Connector) { g_client.Connector->Dispatch->NdkCloseConnector(&g_client.Connector->Header, Step3DoNothing, NULL); g_client.Connector = NULL; }
    if (g_server.Listener)  { g_server.Listener->Dispatch->NdkCloseListener(&g_server.Listener->Header, Step3DoNothing, NULL); g_server.Listener = NULL; }
    STEP3_END* ends[2] = { &g_server, &g_client };
    for (int i = 0; i < 2; i++) {
        STEP3_END* e = ends[i];
        if (e->Qp)  { e->Qp->Dispatch->NdkCloseQp(&e->Qp->Header, Step3DoNothing, NULL); e->Qp = NULL; }
        if (e->Cq)  { e->Cq->Dispatch->NdkCloseCq(&e->Cq->Header, Step3DoNothing, NULL); e->Cq = NULL; }
        if (e->SendCq) { e->SendCq->Dispatch->NdkCloseCq(&e->SendCq->Header, Step3DoNothing, NULL); e->SendCq = NULL; }
        if (e->Pd)  { e->Pd->Dispatch->NdkClosePd(&e->Pd->Header, Step3DoNothing, NULL); e->Pd = NULL; }
        if (e->Adapter) { g_ndk.WskCloseNdkAdapter(g_wskProvider.Client, e->Adapter); e->Adapter = NULL; }
    }
    // the WORK_QUEUE_ITEM is a static; nothing to free
    if (g_wskProvider.Client) { WskReleaseProviderNPI(&g_registration); g_wskProvider.Client = NULL; }
    WskDeregister(&g_registration);
}

static VOID Step3Unload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    // DELIBERATELY EMPTY.  This used to call Step3Cleanup(), which issues NdkClose* requests and
    // does not wait for them - and a driver that returns from unload with operations still pending
    // is bugcheck 0xCE by definition.  NDKPing.sys demonstrated exactly that on this machine today.
    // The objects are released when the machine restarts; D1 must do better, and doing better means
    // WAITING for every close completion before unload returns, which is a real piece of work rather
    // than a line to add here.
    logf("  unload requested - cleanup deliberately skipped (see comment)\r\n");
}

static void Step3Run(void) {
    kPort = Step3ReadPort();
    const ULONG serverSide = Step3ConfigDword(L"ServerSide", 1);
    logf("  flags: port=%u ServerSide=%u\r\n", kPort, serverSide);
    logf("ndkstep3: D0 step 3 - connect two NDK endpoints from one kernel driver (port %u)\r\n\r\n", kPort);

    RtlZeroMemory(&g_wskClientDispatch, sizeof(g_wskClientDispatch));
    g_wskClientDispatch.Version = MAKE_WSK_VERSION(1, 0);
    g_wskClient.ClientContext = NULL;
    g_wskClient.Dispatch = &g_wskClientDispatch;

    NTSTATUS st = WskRegister(&g_wskClient, &g_registration);
    logf("WskRegister                            = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st)) return;

    st = WskCaptureProviderNPI(&g_registration, 0xFFFFFFFF, &g_wskProvider);
    logf("WskCaptureProviderNPI                  = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st)) return;

    RtlZeroMemory(&g_ndk, sizeof(g_ndk));
    st = g_wskProvider.Dispatch->WskControlClient(g_wskProvider.Client,
                                                  WSKNDK_GET_WSK_PROVIDER_NDK_DISPATCH,
                                                  0, NULL, sizeof(g_ndk), &g_ndk, NULL, NULL);
    logf("WskControlClient('NDKD')               = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st) || !g_ndk.WskOpenNdkAdapter) return;

    logf("\r\n-- opening both endpoints\r\n");
    if (!NT_SUCCESS(Step3OpenEnd(&g_server, kServerIf, "server"))) return;
    if (!NT_SUCCESS(Step3OpenEnd(&g_client, kClientIf, "client"))) return;

    // A connector per side (the listener hands the server its own on connect).
        // STATIC ON PURPOSE, and this is the fix for the 0x7E bugcheck this file caused.
    // NdkListen, NdkConnect, NdkCompleteConnect and NdkAccept are ASYNCHRONOUS: they can return
    // STATUS_PENDING and call the completion routine LATER.  This context used to be a plain
    // stack local, so a completion that arrived after the enclosing frame was gone made the
    // provider write into dead stack - memory corruption that surfaced as an access violation
    // inside a worker thread and bugchecked the machine (0x7E, AV_nt!KiStartSystemThread,
    // nt!ExpWorkerThread).  The level-1/level-2 probe survived the same pattern only because
    // those creates happened to complete synchronously.
    static STEP3_ASYNC c;
    Step3Start(&c);
    st = g_client.Adapter->Dispatch->NdkCreateConnector(g_client.Adapter, Step3Complete, &c, &g_client.Connector);
    st = Step3Finish(st, &c, 10000);
    if (NT_SUCCESS(st) && !g_client.Connector) g_client.Connector = (NDK_CONNECTOR*)c.Object;
    logf("  client: NdkCreateConnector             = 0x%08X  connector=%p\r\n", st, g_client.Connector);

    Step3Start(&c);
    st = g_server.Adapter->Dispatch->NdkCreateListener(g_server.Adapter, Step3ConnectEvent, NULL,
                                                       Step3Complete, &c, &g_server.Listener);
    st = Step3Finish(st, &c, 10000);
    if (NT_SUCCESS(st) && !g_server.Listener) g_server.Listener = (NDK_LISTENER*)c.Object;
    logf("  server: NdkCreateListener              = 0x%08X  listener=%p\r\n", st, g_server.Listener);

    SOCKADDR_IN srvAddr;
    RtlZeroMemory(&srvAddr, sizeof(srvAddr));
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = RtlUshortByteSwap(kPort);
    srvAddr.sin_addr.s_addr = RtlUlongByteSwap(0xC0A86402);        // 192.168.100.2
    // Print the address we are actually asking for.  The first version hand-built this sockaddr
    // and got the hex wrong (0x0A64C602 is 10.100.198.2, not 192.168.100.2); the provider answered
    // NdkListen with 0xC0000141, a status that says nothing about the real mistake.  Two lines of
    // logging make the next one of these cost a glance instead of a build-load-read round.
    if (serverSide == 0) {
        logf("  server: SKIPPED - client-only instance (ServerSide=0)\r\n");
    } else {
    logf("  server: asking to listen on 192.168.100.2:%u (s_addr host order 0x%08X)\r\n",
         kPort, RtlUlongByteSwap(srvAddr.sin_addr.s_addr));
    Step3Start(&c);
    st = g_server.Listener->Dispatch->NdkListen(g_server.Listener, (CONST PSOCKADDR)&srvAddr,
                                                sizeof(srvAddr), Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 15000);
    logf("  server: NdkListen                      = 0x%08X\r\n", st);
    }
    if (!NT_SUCCESS(st)) return;

    g_serverPriv.Magic = STEP3_PRIV_MAGIC;
    g_serverPriv.Rkey = 0;
    g_serverPriv.Address = 0;
    g_clientPriv.Magic = STEP3_PRIV_MAGIC;  g_clientPriv.Rkey = 0xDEADBEEF;
    g_clientPriv.Address = 0x1122334455667788ULL;  g_clientPriv.Marker = 0x3C3C3C3C;
    logf("  client: sending %u bytes of private data (marker 0x3C3C3C3C)\r\n", (unsigned)sizeof(g_clientPriv));
    logf("\r\n-- client connects from 192.168.100.3 to 192.168.100.2:%u\r\n", kPort);
    SOCKADDR_IN local, remote;
    RtlZeroMemory(&local, sizeof(local));
    RtlZeroMemory(&remote, sizeof(remote));
    local.sin_family = AF_INET;
    local.sin_port = RtlUshortByteSwap(kPort);
    local.sin_addr.s_addr = RtlUlongByteSwap(0xC0A86403);
    remote.sin_family = AF_INET;
    remote.sin_port = RtlUshortByteSwap(kPort);
    remote.sin_addr.s_addr = RtlUlongByteSwap(0xC0A86402);

    g_serverPriv.Magic = STEP3_PRIV_MAGIC;
    g_serverPriv.Rkey = 0;
    g_serverPriv.Address = 0;
    g_clientPriv.Magic = STEP3_PRIV_MAGIC;

    if (!g_client.Connector) return;
    Step3Start(&c);
    st = g_client.Connector->Dispatch->NdkConnect(g_client.Connector, g_client.Qp,
                                                  (CONST PSOCKADDR)&local, sizeof(local),
                                                  (CONST PSOCKADDR)&remote, sizeof(remote),
                                                  1, 1, &g_clientPriv, sizeof(g_clientPriv),
                                                  Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 20000);
    g_clientConnectStatus = st;
    logf("  client: local port %u -> remote %u:%u\r\n", kPort + 1, 0xC0A86402, kPort);
    logf("  client: NdkConnect                     = 0x%08X\r\n", st);
    if (!NT_SUCCESS(st)) {
        logf("\r\nANSWER: the connect request itself failed (0x%08X).\r\n", st);
        return;
    }

    Step3Start(&c);
    st = g_client.Connector->Dispatch->NdkCompleteConnect(g_client.Connector, NULL, NULL,
                                                          Step3RequestComplete, &c);
    st = Step3Finish(st, &c, 20000);
    g_clientCompleteStatus = st;
    logf("  client: NdkCompleteConnect             = 0x%08X\r\n", st);

    // CLIENT FIRST, THEN ACCEPT, AND THIS TIME FROM THE MAIN THREAD.  The previous version posted the
    // accept inside the connect-event callback and waited for a completion that never arrived; the one
    // before that waited for the accept BEFORE the client completed, which was a deadlock of my own
    // making.  One variable changes per run, and this run also counts completions so that "the provider
    // never called back" cannot be confused with "it called back late".
    logf("  server: waiting for the accept (posted from the callback) to finish\r\n");
    {
        LARGE_INTEGER t2;
        t2.QuadPart = -30000LL * 10000;
        NTSTATUS w = KeWaitForSingleObject(&g_acceptCtx.Event, Executive, KernelMode, FALSE, &t2);
        logf("  server: accept wait                    = 0x%08X  completions %u\r\n", w, g_acceptCompletionCount);
        g_serverAcceptStatus = (g_acceptCallStatus == STATUS_PENDING)
                               ? ((w == STATUS_TIMEOUT) ? STATUS_IO_TIMEOUT : g_acceptCtx.Status)
                               : g_acceptCallStatus;
        logf("  server: NdkAccept                      = 0x%08X\r\n", g_serverAcceptStatus);
        if (g_incoming) {
            ULONG in2 = 0, out2 = 0, len2 = sizeof(g_privBuf);
            RtlZeroMemory(g_privBuf, sizeof(g_privBuf));
            NTSTATUS st2 = g_incoming->Dispatch->NdkGetConnectionData(g_incoming, &in2, &out2, g_privBuf, &len2);
            STEP3_PRIV* pd2 = (STEP3_PRIV*)g_privBuf;
            logf("  server[after accept]: NdkGetConnectionData = 0x%08X privLen=%u magic=0x%08X rkey=0x%08X\r\n",
                 st2, len2, pd2->Magic, pd2->Rkey);
        }
    }

    logf("\r\n  verdict: client connect=0x%08X completeConnect=0x%08X  server accept=0x%08X  privLen(callback)=%u  completions=%u\r\n",
         g_clientConnectStatus, g_clientCompleteStatus, g_serverAcceptStatus, g_privLenInCallback,
         g_acceptCompletionCount);
    {
        const BOOLEAN bothOk = (BOOLEAN)(NT_SUCCESS(g_clientConnectStatus) &&
                                         NT_SUCCESS(g_clientCompleteStatus) &&
                                         NT_SUCCESS(g_serverAcceptStatus));
        logf("\r\nANSWER: %s\r\n", bothOk
             ? "BOTH sides completed the connection - step 3 succeeds and step 4 (one RDMA Read "
               "compared byte for byte) is next."
             : "NOT connected: the verdict line above says which side failed, and completions= says "
               "whether the provider ever called back at all.");
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);
    driver->DriverUnload = Step3Unload;
    RtlZeroMemory(g_log, sizeof(g_log));
    g_len = 0;
    KeInitializeEvent(&g_acceptPosted, NotificationEvent, FALSE);
    KeInitializeEvent(&g_acceptFinished, NotificationEvent, FALSE);
    ExInitializeWorkItem(&g_acceptWorkItem, Step3AcceptWorker, NULL);

    __try {
        Step3Run();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("\r\nEXCEPTION 0x%08X during step 3\r\n", GetExceptionCode());
    }

    // RELEASE EVERYTHING BEFORE THIS DRIVER FINISHES, and this is not tidiness - it is what makes the
    // next run possible.  A legacy kernel service cannot be stopped (sc stop answers 1052
    // ERROR_INVALID_SERVICE_CONTROL) and it cannot be unloaded without a reboot, so an instance that
    // keeps its adapter, listener, PD, CQs and QP leaves the RDMA stack holding them for the life of
    // the boot.  Six such instances accumulated in one afternoon, and the run after that reported the
    // client's NdkConnect refused while the server's connect callback fired - an incoherent pair that
    // is exactly what a congested stack looks like.  Cleanup here means the driver code stays resident
    // (harmless) while its NDK objects do not.
    writeLog();
    return STATUS_SUCCESS;
}
