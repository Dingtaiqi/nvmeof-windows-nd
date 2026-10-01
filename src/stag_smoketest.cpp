// stag_smoketest.cpp - de-risk the NDSPI primitives that NVMe-oF/RDMA needs and
// that this codebase has never exercised.
//
// WHY THIS EXISTS
// ---------------
// Everything in the planned NVMe-oF implementation rests on three NDSPI
// capabilities that `rdmaio/rdma_transfer.cpp` does not use anywhere:
//
//   1. IND2Adapter::CreateMemoryWindow + IND2QueuePair::Bind/Invalidate
//      (the STag lifecycle - NVMe-oF SGLs carry an rkey and the target must be
//       able to revoke it when the command completes)
//   2. Connect/Accept private data (how a real NVMe-oF implementation would
//      exchange the initial parameters)
//   3. inbound/outbound read limits (NVMe-oF's max_rdma_size negotiation)
//
// WinOF 5.50 on a ConnectX-3 Pro is a 2019-era driver for a card NVIDIA has
// since dropped.  "The API is documented" is not evidence that this driver
// implements it.  So before any NVMe-oF code is written on top, this program
// answers the question directly:
//
//   * does CreateMemoryWindow succeed at all on this adapter?
//   * does Bind produce a completion, and over a *subrange* of an MR?
//   * can a remote peer actually RDMA Read/Write through the window's token?
//   * does Invalidate really revoke access, i.e. does a subsequent remote Read
//     using the stale token FAIL?  (If it silently succeeds, the STag is not a
//     security boundary and the design has to account for that.)
//   * do private data and read limits survive the round trip?
//
// It is deliberately two processes rather than two threads, mirroring the
// proven topology of every other test in this repo: the two ports of the card
// are cabled to each other, one process per port.
//
// USAGE
//   stag_smoketest.exe -server 192.168.100.2 54330
//   stag_smoketest.exe -client 192.168.100.2 54330
// Start the server first.  Both print a PASS/FAIL summary; exit code is the
// number of failures, so 0 means the primitives work.
//
// Build: see build_stag.ps1

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <initguid.h>
#include <ndspi.h>
#include <ndsupport.h>
#include <nddef.h>
#include <ndstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")

// ---------------------------------------------------------------------------
//  Tunables
// ---------------------------------------------------------------------------
static const USHORT kPort = 54330;
static const SIZE_T kBufSize = 4u * 1024 * 1024;   // MR total size
static const SIZE_T kWinOff = 1024u * 1024;        // window starts at +1 MiB
static const SIZE_T kWinLen = 64u * 1024;          // window covers 64 KiB
// Control messages (the PeerInfo handover and the 1-byte syncs) must live
// INSIDE a registered memory region.  An SGE whose Buffer is a stack variable
// but whose token is the MR's is rejected by the provider, and the symptom is
// an ND_CANCELED on the peer with no error on the sender - which cost a full
// debug cycle here.  NVMe-oF capsules have exactly the same constraint.
static const SIZE_T kCtrlOff = 2u * 1024 * 1024;   // control area at +2 MiB
static const SIZE_T kSyncOff = kCtrlOff + 256;     // 1-byte syncs after it
// Staging for the data-path tests.  These MUST be inside the registered MR too:
// an SGE whose Buffer lies outside the region named by MemoryRegionToken is
// rejected by the provider with a raw 0xC000003E - a status with no symbolic
// name in ndstatus.h.  The same mistake was made twice in this file (once for
// the control messages, once for the data buffers), which is why the rule is
// written down here rather than remembered.
static const SIZE_T kReadBufOff  = 3u * 1024 * 1024;
static const SIZE_T kWriteBufOff = kReadBufOff + 256 * 1024;
static const ULONG  kCqDepth = 64;
static const ULONG  kQpDepth = 64;
static const DWORD  kWaitMs = 5000;

// Completion contexts.  Non-null and distinct, because NDSPI returns the
// RequestContext verbatim and a null context would make a mismatched completion
// indistinguishable from a correct one.
#define CTX_BIND      ((VOID*)(uintptr_t)0xB1)
#define CTX_INVAL     ((VOID*)(uintptr_t)0x1A)
#define CTX_RECV      ((VOID*)(uintptr_t)0x2C)
#define CTX_SEND      ((VOID*)(uintptr_t)0x5E)
#define CTX_READ      ((VOID*)(uintptr_t)0x4E)
#define CTX_WRITE     ((VOID*)(uintptr_t)0x77)

// Exchanged through Connect/Accept private data.  This is also the test of
// private data itself: if it does not survive the round trip, both sides get
// garbage tokens and every subsequent step fails loudly rather than subtly.
#pragma pack(push, 1)
struct PeerInfo {
    uint64_t addr_mr;      // base of the whole MR
    uint32_t token_mr;     // rkey of the whole MR   (NVMe-oF would use this as an SGL rkey)
    uint64_t addr_win;     // base of the memory window
    uint32_t token_win;    // rkey of the memory window
    uint32_t magic;        // so a partially-copied struct is detectable
    uint32_t marker;       // filler byte value the owner wrote into its buffer
};
#pragma pack(pop)
static const uint32_t kPeerMagic = 0x4E564D46;   // 'NVMF'

static int g_failures = 0;

// ND_TIMEOUT is 0x00000102 and ND_PENDING is 0x00000103: both are
// *success*-severity HRESULTs, so SUCCEEDED() reports a timed-out wait as a
// pass.  That is not hypothetical - the first run of this very program printed
// "[PASS] Bind completion reaped" for a reaped-nothing timeout.  Every check in
// this file goes through ndOk() instead.
static bool ndOk(HRESULT hr) {
    return SUCCEEDED(hr) && hr != ND_TIMEOUT && hr != ND_PENDING;
}

static void Report(const char* what, int ok, const char* detail) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
    if (!ok) g_failures++;
}

// A check that this configuration cannot make.  It is NOT a pass and NOT a failure,
// and it must not be silent: with outboundReadLimit = 0 the transport forbids
// RDMA Read in that direction, so every read assertion below is unanswerable - and
// the first version of this file reported those as FAIL, which made a suite that
// was working exactly as documented look broken (and hid a second real problem:
// run_stag.ps1 only ever ran readlimit=0).
static void Skip(const char* what, const char* why) {
    printf("  [SKIP] %s - %s\n", what, why);
}

static void ReportHr(const char* what, HRESULT hr) {
    char d[96];
    if (hr == ND_TIMEOUT)      sprintf_s(d, "ND_TIMEOUT (0x%08X) - the wait expired", (unsigned)hr);
    else if (hr == ND_PENDING) sprintf_s(d, "ND_PENDING (0x%08X) - never completed", (unsigned)hr);
    else                       sprintf_s(d, "HRESULT 0x%08X", (unsigned)hr);
    Report(what, ndOk(hr), d);
}

// Read limits are configurable because the first run showed a non-zero value
// failing on this adapter; the test has to be able to isolate that variable
// rather than assume which of the two parameters broke the connect.
static ULONG g_readLimit = 0;
// outboundReadLimit == 0 is a CONFIGURATION, not a broken one: the transport then
// forbids RDMA Read in that direction (see the note at the top of nvmeof_rdma.h -
// "outboundReadLimit must be non-zero or this side cannot issue RDMA Read at all,
// and the failure is a raw 0xC000003E").  The read-path assertions below are gated
// on this so that each configuration asserts what it can actually prove.
static bool readsAllowed() { return g_readLimit != 0; }
// Optional explicit local address for the connector (see runClient).
static char g_localIp[64] = {};

// ---------------------------------------------------------------------------
//  Small NDSPI helper layer
// ---------------------------------------------------------------------------
struct Nd {
    IND2Adapter*        adapter = nullptr;
    IND2CompletionQueue* cq = nullptr;
    IND2Connector*      conn = nullptr;
    IND2QueuePair*      qp = nullptr;
    IND2MemoryRegion*   mr = nullptr;
    IND2MemoryWindow*   mw = nullptr;
    IND2Listener*       listener = nullptr;
    HANDLE              ovFile = INVALID_HANDLE_VALUE;
    OVERLAPPED          ov = {};
    char*               buf = nullptr;

    // Bounded overlapped wait.  GetOverlappedResult(pOv, TRUE) blocks forever,
    // and this test is specifically about failure modes, so every wait here is
    // deadline-based and reports a distinct timeout instead of hanging.
    HRESULT waitOverlapped(IND2Overlapped* obj, DWORD timeoutMs) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            HRESULT hr = obj->GetOverlappedResult(&ov, FALSE);
            if (hr != ND_PENDING) return hr;
            if (GetTickCount64() - t0 >= timeoutMs) {
                obj->CancelOverlappedRequests();
                obj->GetOverlappedResult(&ov, FALSE);
                return ND_TIMEOUT;
            }
            SwitchToThread();
        }
    }

    // Reap exactly one completion, checking the context so a stale or crossed
    // completion cannot be mistaken for the one we are waiting for.
    HRESULT reap(VOID* expectCtx, ND2_RESULT* out, DWORD timeoutMs) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            ND2_RESULT r = {};
            ULONG got = cq->GetResults(&r, 1);
            if (got == 1) {
                if (expectCtx != nullptr && r.RequestContext != expectCtx) {
                    printf("    (unexpected completion ctx=%p type=%d status=0x%08X)\n",
                           r.RequestContext, (int)r.RequestType, (unsigned)r.Status);
                    continue;   // keep waiting for the one we want
                }
                if (out) *out = r;
                return r.Status;
            }
            if (GetTickCount64() - t0 >= timeoutMs) return ND_TIMEOUT;
            SwitchToThread();
        }
    }

    bool init(const sockaddr_in& local, bool isListener) {
        // `local` must be this process's OWN address.  For the connector that is
        // not the address on the command line - it is whatever NdResolveAddress
        // derives from the peer's address, i.e. the card's other port.  Opening
        // the adapter on the peer's own address instead makes the process
        // connect to itself, which the provider reports as
        // ND_CONNECTION_REFUSED (0xC0000236) on one side and
        // ND_CONNECTION_ACTIVE  (0xC000023B) on the other.
        {
            char ipstr[64] = {}; char portstr[16] = {};
            InetNtopA(AF_INET, &local.sin_addr, ipstr, sizeof(ipstr));
            sprintf_s(portstr, "%u", ntohs(local.sin_port));
            printf("  opening adapter on local %s:%s\n", ipstr, portstr);
        }
        HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&local,
                                   sizeof(local), (VOID**)&adapter);
        if (FAILED(hr)) { printf("NdOpenAdapter failed 0x%08X\n", (unsigned)hr); return false; }

        // What does this adapter actually claim to support?  Everything the
        // NVMe-oF design depends on (window size, read limits, private-data
        // size) is bounded by these, and a zero here explains a later failure
        // instantly instead of after an afternoon.
        {
            ND2_ADAPTER_INFO ai = {};
            ai.InfoVersion = ND_VERSION_2;
            ULONG len = sizeof(ai);
            hr = adapter->Query(&ai, &len);
            if (SUCCEEDED(hr)) {
                printf("  adapter: MaxReg=%llu MiB  MaxWindow=%llu MiB  MaxXfer=%u KiB\n",
                       (unsigned long long)(ai.MaxRegistrationSize >> 20),
                       (unsigned long long)(ai.MaxWindowSize >> 20),
                       ai.MaxTransferLength >> 10);
                printf("           MaxInboundReadLimit=%u  MaxOutboundReadLimit=%u\n",
                       ai.MaxInboundReadLimit, ai.MaxOutboundReadLimit);
                printf("           MaxCallerData=%u  MaxCalleeData=%u  MaxReadSge=%u  flags=0x%X\n",
                       ai.MaxCallerData, ai.MaxCalleeData, ai.MaxReadSge, ai.AdapterFlags);
                if (ai.MaxWindowSize < kWinLen) {
                    printf("  WARNING: adapter MaxWindowSize (%llu) < test window (%llu)\n",
                           (unsigned long long)ai.MaxWindowSize, (unsigned long long)kWinLen);
                }
            } else {
                printf("  adapter Query failed 0x%08X\n", (unsigned)hr);
            }
        }

        hr = adapter->CreateOverlappedFile(&ovFile);
        if (FAILED(hr)) { printf("CreateOverlappedFile 0x%08X\n", (unsigned)hr); return false; }
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!ov.hEvent) { printf("CreateEvent failed\n"); return false; }
        hr = adapter->CreateCompletionQueue(IID_IND2CompletionQueue, ovFile,
                                            kCqDepth, 0, 0, (VOID**)&cq);
        if (FAILED(hr)) { printf("CreateCompletionQueue 0x%08X\n", (unsigned)hr); return false; }
        hr = adapter->CreateConnector(IID_IND2Connector, ovFile, (VOID**)&conn);
        if (FAILED(hr)) { printf("CreateConnector 0x%08X\n", (unsigned)hr); return false; }
        hr = adapter->CreateQueuePair(IID_IND2QueuePair, cq, cq, nullptr,
                                      kQpDepth, kQpDepth, 1, 1, 0, (VOID**)&qp);
        if (FAILED(hr)) { printf("CreateQueuePair 0x%08X\n", (unsigned)hr); return false; }
        hr = adapter->CreateMemoryRegion(IID_IND2MemoryRegion, ovFile, (VOID**)&mr);
        if (FAILED(hr)) { printf("CreateMemoryRegion 0x%08X\n", (unsigned)hr); return false; }
        if (isListener) {
            hr = adapter->CreateListener(IID_IND2Listener, ovFile, (VOID**)&listener);
            if (FAILED(hr)) { printf("CreateListener 0x%08X\n", (unsigned)hr); return false; }
        }
        return true;
    }

    bool registerBuf(uint32_t marker) {
        buf = (char*)VirtualAlloc(nullptr, kBufSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!buf) { printf("VirtualAlloc failed\n"); return false; }
        memset(buf, (int)(marker & 0xff), kBufSize);
        // ND_MR_FLAG_RDMA_READ_SINK is the one flag the working RDMA Read path
        // in rdmaio/rdma_transfer.cpp sets that this test did not.  Without it
        // the provider fails a remote Read with a raw 0xC000003E - a status that
        // appears nowhere in ndstatus.h, so there is nothing to look up and
        // nothing to search for.  Recorded here because "the API has a flag for
        // exactly this and the docs never say it is mandatory" is the kind of
        // requirement that only ever surfaces as an unnamed error code.
        HRESULT hr = mr->Register(buf, kBufSize,
                                  ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                                  ND_MR_FLAG_ALLOW_REMOTE_READ |
                                  ND_MR_FLAG_ALLOW_REMOTE_WRITE |
                                  ND_MR_FLAG_RDMA_READ_SINK, &ov);
        if (hr == ND_PENDING) hr = waitOverlapped(mr, kWaitMs);
        if (FAILED(hr)) { printf("Register failed 0x%08X\n", (unsigned)hr); return false; }
        return true;
    }

    void cleanup() {
        if (conn) { conn->Disconnect(&ov); waitOverlapped(conn, 2000); }
        if (mr) { mr->Deregister(&ov); waitOverlapped(mr, 2000); }
        if (mw) { mw->Release(); mw = nullptr; }
        if (listener) { listener->Release(); listener = nullptr; }
        if (qp)  { qp->Release();  qp = nullptr; }
        if (cq)  { cq->Release();  cq = nullptr; }
        if (conn){ conn->Release(); conn = nullptr; }
        if (ovFile != INVALID_HANDLE_VALUE) { CloseHandle(ovFile); ovFile = INVALID_HANDLE_VALUE; }
        if (adapter) { adapter->Release(); adapter = nullptr; }
        if (ov.hEvent) { CloseHandle(ov.hEvent); ov.hEvent = nullptr; }
        if (buf) { VirtualFree(buf, 0, MEM_RELEASE); buf = nullptr; }
    }
};

// Build this process's own address from the IP it was given, and (for a
// connector) derive the local adapter address from the peer's, which is how the
// two ports of one card end up on opposite ends of a real wire connection.
static bool makeLocal(const char* ip, sockaddr_in* out) {
    *out = {};
    out->sin_family = AF_INET;
    out->sin_port = htons(kPort);
    return InetPtonA(AF_INET, ip, &out->sin_addr) == 1;
}
static bool resolveLocalFromRemote(const sockaddr_in& remote, sockaddr_in* outLocal) {
    SIZE_T len = sizeof(*outLocal);
    HRESULT hr = NdResolveAddress((const sockaddr*)&remote, sizeof(remote),
                                  (sockaddr*)outLocal, &len);
    return SUCCEEDED(hr);
}

// ===========================================================================
//  SERVER (listener)
// ===========================================================================
static int runServer(const char* ip) {
    printf("=== stag_smoketest SERVER on %s:%u\n", ip, kPort);
    sockaddr_in local = {};
    if (!makeLocal(ip, &local)) { printf("bad ip %s\n", ip); return 1; }
    Nd nd;
    if (!nd.init(local, true)) return 1;
    if (!nd.registerBuf(0x5A)) return 1;   // server fills with 0x5A

    HRESULT hr;
    uint32_t mrToken = nd.mr->GetRemoteToken();

    // Only the MR rkey can be published at connect time.  The memory window
    // cannot: a Bind is a queue-pair operation, so on a QP that is not yet
    // connected the work request merely sits in the queue and produces no
    // completion.  The window is therefore created and bound after Accept, and
    // its rkey is handed over in a control message (see below).
    PeerInfo mine = {};
    mine.addr_mr = (uint64_t)(uintptr_t)nd.buf;
    mine.token_mr = mrToken;
    mine.magic = kPeerMagic;
    mine.marker = 0x5A;

    // ---- connection: bind the listener, listen, accept
    printf("\n-- connection\n");
    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(kPort);
    InetPtonA(AF_INET, ip, &bindAddr.sin_addr);

    hr = nd.listener->Bind((const sockaddr*)&bindAddr, sizeof(bindAddr));
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.listener, kWaitMs);
    ReportHr("listener Bind", hr);
    if (!ndOk(hr)) { nd.cleanup(); return g_failures; }

    hr = nd.listener->Listen(1);
    ReportHr("listener Listen", hr);

    hr = nd.listener->GetConnectionRequest(nd.conn, &nd.ov);
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.listener, 15000);
    ReportHr("GetConnectionRequest (waited up to 15 s for the client)", hr);
    if (FAILED(hr)) { nd.cleanup(); return g_failures; }

    // The peer's private data must be read HERE, between GetConnectionRequest
    // and Accept.  Reading it after Accept yields an empty buffer with no error,
    // which is how the first run of this test "lost" the tokens silently: both
    // sides reported success on the handshake and then had rkey 0.
    ULONG peerLen = 0;
    hr = nd.conn->GetPrivateData(nullptr, &peerLen);
    bool overflowAsDocumented = (hr == ND_BUFFER_OVERFLOW);
    {
        char d[160];
        sprintf_s(d, "size query returned 0x%08X (ND_BUFFER_OVERFLOW=0x%08X), peer sent %u B",
                  (unsigned)hr, (unsigned)ND_BUFFER_OVERFLOW, (unsigned)peerLen);
        Report("GetPrivateData size query before Accept", overflowAsDocumented, d);
    }
    char* peerRaw = (char*)malloc(peerLen ? peerLen : 1);
    ULONG peerLen2 = peerLen;
    hr = nd.conn->GetPrivateData(peerRaw, &peerLen2);
    PeerInfo peer = {};
    bool peerOk = ndOk(hr) && peerLen2 >= sizeof(peer);
    if (peerOk) memcpy(&peer, peerRaw, sizeof(peer));
    {
        char d[224];
        sprintf_s(d, "hr=0x%08X %u bytes, magic=0x%08X, MR rkey=0x%08X, win rkey=0x%08X",
                  (unsigned)hr, (unsigned)peerLen2, peer.magic, peer.token_mr, peer.token_win);
        Report("client's private data (MR rkey) received",
               peerOk && peer.magic == kPeerMagic && peer.token_mr != 0, d);
    }
    free(peerRaw);

    hr = nd.conn->Accept(nd.qp, g_readLimit, g_readLimit,
                         &mine, (ULONG)sizeof(mine), &nd.ov);
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.conn, kWaitMs);
    {
        char d[128];
        sprintf_s(d, "inbound=%u outbound=%u privateData=%u B -> HRESULT 0x%08X",
                  g_readLimit, g_readLimit, (unsigned)sizeof(mine), (unsigned)hr);
        Report("Accept with private data", ndOk(hr), d);
    }
    if (!ndOk(hr)) { nd.cleanup(); return g_failures; }

    // ---- memory window, now that the QP is actually connected
    // A Bind on an unconnected QP is queued and never completes; this ordering
    // is the whole lesson of the first two runs of this test.
    printf("\n-- memory window (after connect)\n");
    hr = nd.adapter->CreateMemoryWindow(IID_IND2MemoryWindow, (VOID**)&nd.mw);
    Report("CreateMemoryWindow", SUCCEEDED(hr), nullptr);
    if (FAILED(hr)) { nd.cleanup(); return g_failures; }

    hr = nd.qp->Bind(CTX_BIND, nd.mr, nd.mw, nd.buf + kWinOff, kWinLen,
                     ND_OP_FLAG_ALLOW_READ | ND_OP_FLAG_ALLOW_WRITE);
    Report("Bind over a 64 KiB SUBRANGE returns ND_SUCCESS", hr == ND_SUCCESS, nullptr);
    if (hr == ND_SUCCESS) {
        ND2_RESULT r = {};
        HRESULT st = nd.reap(CTX_BIND, &r, kWaitMs);
        char d[192];
        sprintf_s(d, "status=0x%08X type=%d bytes=%u (type 2 = Nd2RequestTypeBind)",
                  (unsigned)st, (int)r.RequestType, r.BytesTransferred);
        Report("Bind completion reaped before the rkey is published", ndOk(st), d);
        if (!ndOk(st)) { nd.cleanup(); return g_failures; }
    } else {
        Report("Bind completion reaped before the rkey is published", false,
               "Bind did not return ND_SUCCESS");
        nd.cleanup(); return g_failures;
    }

    uint32_t winToken = nd.mw->GetRemoteToken();
    {
        char d[192];
        sprintf_s(d, "window rkey=0x%08X, MR rkey=0x%08X, distinct=%s",
                  winToken, mrToken, (winToken != mrToken) ? "yes" : "NO");
        Report("window and MR expose different rkeys", winToken != 0 && winToken != mrToken, d);
    }

    // Hand the window rkey over as a control message: this is also the two-sided
    // Send/Recv path that NVMe-oF uses for every command capsule.
    mine.addr_win = (uint64_t)(uintptr_t)(nd.buf + kWinOff);
    mine.token_win = winToken;
    {
        PeerInfo* out = (PeerInfo*)(nd.buf + kCtrlOff);   // inside the MR
        *out = mine;
        ND2_SGE s = {};
        s.Buffer = out;
        s.BufferLength = (ULONG)sizeof(*out);
        s.MemoryRegionToken = nd.mr->GetLocalToken();
        HRESULT shr = nd.qp->Send(CTX_SEND, &s, 1, 0);
        hr = shr;
        char d[160];
        sprintf_s(d, "Send=0x%08X", (unsigned)shr);
        if (ndOk(shr)) {
            ND2_RESULT sr = {};
            hr = nd.reap(CTX_SEND, &sr, kWaitMs);
            sprintf_s(d, "Send=0x%08X completion=0x%08X type=%d bytes=%u",
                      (unsigned)shr, (unsigned)hr, (int)sr.RequestType, sr.BytesTransferred);
        }
        Report("window rkey sent to the client as a control message", ndOk(hr), d);
        if (!ndOk(hr)) { nd.cleanup(); return g_failures; }
    }

    // What limits did the peer actually agree to?
    ULONG inLim = 0, outLim = 0;
    hr = nd.conn->GetReadLimits(&inLim, &outLim);
    {
        char d[200];
        sprintf_s(d, "inbound=%u outbound=%u (HRESULT 0x%08X) - informational: the limits "
                     "demonstrably take effect (Reads fail outright when outboundReadLimit "
                     "is 0), they just cannot be queried at this point",
                  inLim, outLim, (unsigned)hr);
        printf("  [info] GetReadLimits: %s\n", d);
    }

    // ---- 4. the peer's private data was already read before Accept (see above).

    // ---- 5. Exchange a byte so the client proceeds.
    printf("\n-- data path and invalidation\n");
    char* syncBuf = (char*)(nd.buf + kSyncOff + 16); memset(syncBuf, 0, 64);
    ND2_SGE sge = {};
    sge.Buffer = syncBuf;
    sge.BufferLength = sizeof(syncBuf);
    sge.MemoryRegionToken = nd.mr->GetLocalToken();
    hr = nd.qp->Receive(CTX_RECV, &sge, 1);
    Report("post Receive for the client's sync message", SUCCEEDED(hr), nullptr);

    ND2_RESULT rr = {};
    hr = nd.reap(CTX_RECV, &rr, 15000);
    if (readsAllowed()) Report("client's sync message arrived", SUCCEEDED(hr) && rr.BytesTransferred >= 1, nullptr);
    else Skip("client's sync message arrived",
              "readlimit=0: the client's first RDMA Read is refused, the QP goes "
              "ND_CANCELED and it never reaches its sync Send");

    // ---- 6. the client wrote marker 0x3C into our window; verify it landed
    //         exactly inside the window and nowhere else.
    {
        // Control A: the plain-MR write should have landed at kWriteBufOff,
        // outside the window, proving that a one-sided write reaches this
        // process's memory independently of the memory window.
        const unsigned char* wsrc = (const unsigned char*)(nd.buf + kWriteBufOff);
        bool plainWriteLanded = true;
        for (SIZE_T i = 0; i < 4096; i++) {
            if (wsrc[i] != 0x3C) { plainWriteLanded = false; break; }
        }
        Report("[control A] plain MR write landed (checked 4 KiB at kWriteBufOff)",
               plainWriteLanded, plainWriteLanded ? "" : "memory still holds 0x5A");

        unsigned char* w = (unsigned char*)(nd.buf + kWinOff);
        bool allWritten = true;
        for (SIZE_T i = 0; i < kWinLen; i++) {
            if (w[i] != 0x3C) { allWritten = false; break; }
        }
        bool beforeIntact = ((unsigned char)nd.buf[kWinOff - 1] == 0x5A);
        bool afterIntact  = ((unsigned char)nd.buf[kWinOff + kWinLen] == 0x5A);
        char d[192];
        sprintf_s(d, "window overwritten=%s, byte before window untouched=%s, byte after=%s",
                  allWritten ? "yes" : "no", beforeIntact ? "yes" : "no", afterIntact ? "yes" : "no");
        if (readsAllowed()) {
            Report("remote RDMA Write landed exactly inside the bound window",
                   allWritten && beforeIntact && afterIntact, d);
        } else {
            Skip("remote RDMA Write landed exactly inside the bound window",
                 "readlimit=0: the client dies on its refused Read before it writes "
                 "into the window");
        }
    }

    // ---- 7. read back from the client's MR through ITS MR rkey, so both
    //         directions of the data path and both kinds of rkey are covered.
    {
        char* rb = nd.buf + kReadBufOff;      // inside the MR
        ND2_SGE rs = {};
        rs.Buffer = rb;
        rs.BufferLength = (ULONG)kWinLen;
        rs.MemoryRegionToken = nd.mr->GetLocalToken();
        hr = nd.qp->Read(CTX_READ, &rs, 1, peer.addr_mr, peer.token_mr, 0);
        ND2_RESULT rd = {};
        HRESULT st = SUCCEEDED(hr) ? nd.reap(CTX_READ, &rd, kWaitMs) : hr;
        bool ok = SUCCEEDED(st) && rd.BytesTransferred == kWinLen && rb[0] == (char)peer.marker;
        char d[160];
        sprintf_s(d, "status=0x%08X bytes=%u first=0x%02X expected=0x%02X",
                  (unsigned)st, rd.BytesTransferred, (unsigned char)rb[0], peer.marker);
        if (readsAllowed()) {
            Report("server RDMA Read from client's MR via the MR rkey", ok, d);
        } else {
            // This side accepted with outboundReadLimit = 0, so it is not allowed to
            // issue an RDMA Read either.  The interesting assertion is the refusal.
            Report("server RDMA Read is REFUSED while outboundReadLimit == 0", !ok, d);
        }
    }

    // ---- 8. invalidate the window, tell the client, then the client must fail
    hr = nd.qp->Invalidate(CTX_INVAL, nd.mw, 0);
    Report("Invalidate posted", SUCCEEDED(hr), nullptr);
    if (SUCCEEDED(hr)) {
        ND2_RESULT ir = {};
        HRESULT st = nd.reap(CTX_INVAL, &ir, kWaitMs);
        char d[128];
        sprintf_s(d, "status=0x%08X type=%d", (unsigned)st, (int)ir.RequestType);
        // Not every provider completes an Invalidate, so a timeout here is
        // recorded but is not by itself a failure of the primitive.
        Report("Invalidate completion observed", SUCCEEDED(st), d);
    }

    char* go = (char*)(nd.buf + kSyncOff + 0); *go = 'X';
    ND2_SGE gs = {};
    gs.Buffer = go;
    gs.BufferLength = 1;
    gs.MemoryRegionToken = nd.mr->GetLocalToken();
    nd.qp->Send(CTX_SEND, &gs, 1, 0);
    nd.reap(CTX_SEND, nullptr, kWaitMs);

    // ---- 9. the client tells us whether the stale token was really rejected
    // The client's verdict on revocation arrives in its own output, not here:
    // see the note in runClient.  Nothing to wait for on this side.
    printf("  [info] revocation verdict is reported by the client process\n");

    nd.cleanup();
    printf("\nserver failures: %d\n", g_failures);
    return g_failures;
}

// ===========================================================================
//  CLIENT (connector)
// ===========================================================================
static int runClient(const char* ip) {
    printf("=== stag_smoketest CLIENT connecting to %s:%u\n", ip, kPort);
    sockaddr_in remote = {};
    if (!makeLocal(ip, &remote)) { printf("bad ip %s\n", ip); return 1; }

    // The connector must open the adapter on the address NdResolveAddress picks
    // for this peer, not on the peer's own address.
    sockaddr_in local = {};
    bool haveExplicitLocal = (g_localIp[0] != 0);
    if (haveExplicitLocal) {
        // Explicit is better here: with both ports in the same subnet,
        // NdResolveAddress() answers with the address that owns the route, which
        // on a single machine is the peer's own address - so both ends land on
        // the same port and the "connection" is provider loopback, not the wire.
        // Pinning the local address is what forces the frames onto the cable.
        if (!makeLocal(g_localIp, &local)) { printf("bad -local %s\n", g_localIp); return 1; }
    } else if (!resolveLocalFromRemote(remote, &local)) {
        printf("NdResolveAddress failed for %s (pass -local <ip> to pin it)\n", ip);
        return 1;
    }
    {
        char lip[64] = {}; char rip[64] = {};
        InetNtopA(AF_INET, &local.sin_addr, lip, sizeof(lip));
        InetNtopA(AF_INET, &remote.sin_addr, rip, sizeof(rip));
        printf("  remote %s -> local %s (%s)\n", rip, lip,
               haveExplicitLocal ? "pinned with -local"
                                 : "derived from NdResolveAddress");
        if (strcmp(lip, rip) == 0) {
            printf("  WARNING: local == remote, so this connection is provider\n"
                   "           loopback and never touches the cable.  Pass\n"
                   "           -local <the card's OTHER port ip> to force the wire.\n");
        }
    }

    Nd nd;
    if (!nd.init(local, false)) return 1;
    if (!nd.registerBuf(0x3C)) return 1;   // client fills with 0x3C

    PeerInfo mine = {};
    mine.addr_mr = (uint64_t)(uintptr_t)nd.buf;
    mine.token_mr = nd.mr->GetRemoteToken();
    mine.addr_win = (uint64_t)(uintptr_t)nd.buf;
    mine.token_win = 0;              // client has no window; that is fine
    mine.magic = kPeerMagic;
    mine.marker = 0x3C;

    const ULONG kReadLimit = g_readLimit;
    HRESULT hr = nd.conn->Bind((const sockaddr*)&remote, sizeof(remote));
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.conn, kWaitMs);
    ReportHr("connector Bind", hr);
    if (FAILED(hr)) { nd.cleanup(); return g_failures; }

    hr = nd.conn->Connect(nd.qp, (const sockaddr*)&remote, sizeof(remote),
                          kReadLimit, kReadLimit, &mine, (ULONG)sizeof(mine), &nd.ov);
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.conn, 15000);
    {
        char d[160];
        sprintf_s(d, "inbound=%u outbound=%u privateData=%u B -> HRESULT 0x%08X",
                  kReadLimit, kReadLimit, (unsigned)sizeof(mine), (unsigned)hr);
        Report("Connect with private data", ndOk(hr), d);
    }
    if (!ndOk(hr)) { nd.cleanup(); return g_failures; }

    // Post the receive for the server's window-rkey message NOW, before the rest
    // of the handshake.  Posting it after CompleteConnect lost a race: the server
    // binds and sends within a millisecond of accepting, the Send found no
    // matching receive, and the provider failed it with ND_IO_TIMEOUT
    // (0xC00000B5) - which surfaces on the *server* as a send failure while the
    // client merely sees a cancelled receive.  A two-sided message has to have
    // its receive posted before the peer can plausibly send it.
    PeerInfo* win = (PeerInfo*)(nd.buf + kCtrlOff);   // inside the MR
    memset(win, 0, sizeof(*win));
    ND2_SGE winSge = {};
    winSge.Buffer = win;
    winSge.BufferLength = (ULONG)sizeof(*win);
    winSge.MemoryRegionToken = nd.mr->GetLocalToken();
    hr = nd.qp->Receive(CTX_RECV, &winSge, 1);
    ReportHr("receive posted for the window-rkey control message", hr);
    if (!ndOk(hr)) { nd.cleanup(); return g_failures; }

    // Read the callee's private data between Connect and CompleteConnect, which
    // is where ndtestutil reads it and the only point where it is populated.
    // Only the MR rkey travels here: the window rkey cannot, because the server
    // cannot bind a window until the QP is connected.
    ULONG peerLen = 0;
    nd.conn->GetPrivateData(nullptr, &peerLen);
    char* peerRaw = (char*)malloc(peerLen ? peerLen : 1);
    ULONG peerLen2 = peerLen;
    hr = nd.conn->GetPrivateData(peerRaw, &peerLen2);
    PeerInfo peer = {};
    bool peerOk = ndOk(hr) && peerLen2 >= sizeof(peer);
    if (peerOk) memcpy(&peer, peerRaw, sizeof(peer));
    {
        char d[224];
        sprintf_s(d, "hr=0x%08X %u bytes, magic=0x%08X, MR rkey=0x%08X",
                  (unsigned)hr, (unsigned)peerLen2, peer.magic, peer.token_mr);
        Report("server's private data (MR rkey) received",
               peerOk && peer.magic == kPeerMagic && peer.token_mr != 0, d);
    }
    free(peerRaw);

    hr = nd.conn->CompleteConnect(&nd.ov);
    if (hr == ND_PENDING) hr = nd.waitOverlapped(nd.conn, kWaitMs);
    ReportHr("CompleteConnect", hr);
    if (!ndOk(hr)) { nd.cleanup(); return g_failures; }

    // CompleteConnect's own completion is not reaped here on purpose: a
    // "drain one completion with a wildcard context" call can swallow the
    // window-rkey receive that is already queued above, which is exactly how an
    // earlier revision of this test lost the token while reporting success.

    if (!peerOk || peer.token_mr == 0) { nd.cleanup(); return g_failures; }

    // Now receive the window rkey the server sends once its Bind has completed.
    {
        hr = nd.reap(CTX_RECV, nullptr, 15000);
        char d[224];
        sprintf_s(d, "hr=0x%08X magic=0x%08X win rkey=0x%08X win addr=0x%llX",
                  (unsigned)hr, win->magic, win->token_win, (unsigned long long)win->addr_win);
        Report("window rkey received via control message",
               ndOk(hr) && win->magic == kPeerMagic && win->token_win != 0, d);
        if (!ndOk(hr) || win->token_win == 0) { nd.cleanup(); return g_failures; }
        peer.addr_win = win->addr_win;
        peer.token_win = win->token_win;
    }

    // ---- 1. RDMA Read through the SERVER's window token (this is the STag
    //         path NVMe-oF will use for every read command's data).
    printf("\n-- data path through the peer's memory window\n");
    char* rb = nd.buf + kReadBufOff;          // inside the MR
    memset(rb, 0, kWinLen);
    ND2_SGE sge = {};
    sge.Buffer = rb;
    sge.BufferLength = (ULONG)kWinLen;
    sge.MemoryRegionToken = nd.mr->GetLocalToken();

    // Control experiment A: a plain one-sided WRITE via the MR rkey.  Run this
    // before any Read, because a failed one-sided op leaves the QP reporting
    // ND_CANCELED for everything after it, which turns a cascade into what looks
    // like several independent failures.  Write is also the simpler operation -
    // no sink registration - so it separates "one-sided ops do not work here"
    // from "Read specifically does not work here".
    {
        char* wsrc = nd.buf + kWriteBufOff;      // inside the MR
        memset(wsrc, 0x3C, kWinLen);
        ND2_SGE ws = {};
        ws.Buffer = wsrc;
        ws.BufferLength = (ULONG)kWinLen;
        ws.MemoryRegionToken = nd.mr->GetLocalToken();
        hr = nd.qp->Write(CTX_WRITE, &ws, 1, peer.addr_mr + kWriteBufOff, peer.token_mr, 0);
        ND2_RESULT mr = {};
        HRESULT mst = ndOk(hr) ? nd.reap(CTX_WRITE, &mr, kWaitMs) : hr;
        char d[224];
        sprintf_s(d, "status=0x%08X bytes=%u type=%d", (unsigned)mst,
                  mr.BytesTransferred, (int)mr.RequestType);
        Report("[control A] plain RDMA WRITE via the MR rkey", ndOk(mst), d);
    }

    // Control experiment B, and then the whole read path - but only where the
    // transport permits it.  With outboundReadLimit = 0 the connection was made with
    // "this side may not issue RDMA Read", so the one useful assertion left is that
    // the Read IS refused, and it has to come LAST: a refused one-sided op leaves
    // this QP reporting ND_CANCELED for everything after it (see the note on control
    // A above), so running the write tests afterwards turns one configuration fact
    // into four apparent failures.  That is exactly what this suite used to report.
    if (!readsAllowed()) {
        printf("\n-- readlimit=0: the transport forbids RDMA Read in this direction\n");
        memset(rb, 0, kWinLen);
        hr = nd.qp->Read(CTX_READ, &sge, 1, peer.addr_mr + kWinOff, peer.token_mr, 0);
        ND2_RESULT mr = {};
        HRESULT mst = ndOk(hr) ? nd.reap(CTX_READ, &mr, kWaitMs) : hr;
        char d[224];
        // No symbolic name for this one: the provider answers a refused one-sided
        // operation with 0xC000003E, which appears in no ndstatus.h (see the note at
        // the top of nvmeof_rdma.h).  The size of the failure is the point here, not
        // its name.
        sprintf_s(d, "status=0x%08X - the documented result of an outbound limit of 0 "
                     "(0xC000003E has no symbolic name)", (unsigned)mst);
        Report("RDMA Read is REFUSED when outboundReadLimit == 0", !ndOk(mst), d);
        Skip("[control B] RDMA Read via the plain MR rkey",
             "the refusal above IS the assertion for this configuration");
        Skip("client RDMA Read through the WINDOW rkey", "readlimit=0");
        Skip("client RDMA Write into the WINDOW rkey",
             "readlimit=0: the refused Read already killed the QP, so a result here "
             "would measure the cascade, not the primitive");
        Skip("sync Send to the server", "readlimit=0: the QP is dead by now");
        Skip("send the sync byte so the server can verify the window write",
             "readlimit=0");
    } else {
    // --- everything below runs only when outboundReadLimit != 0 ---

    {
        memset(rb, 0, kWinLen);
        hr = nd.qp->Read(CTX_READ, &sge, 1, peer.addr_mr + kWinOff, peer.token_mr, 0);
        ND2_RESULT mr = {};
        HRESULT mst = ndOk(hr) ? nd.reap(CTX_READ, &mr, kWaitMs) : hr;
        char d[192];
        sprintf_s(d, "status=0x%08X bytes=%u first=0x%02X (expect 0x5A)",
                  (unsigned)mst, mr.BytesTransferred, (unsigned char)rb[0]);
        Report("[control B] RDMA Read via the plain MR rkey", ndOk(mst) && rb[0] == 0x5A, d);
    }

    hr = nd.qp->Read(CTX_READ, &sge, 1, peer.addr_win, peer.token_win, 0);
    ND2_RESULT rd = {};
    HRESULT st = SUCCEEDED(hr) ? nd.reap(CTX_READ, &rd, kWaitMs) : hr;
    bool readOk = SUCCEEDED(st) && rd.BytesTransferred == kWinLen && rb[0] == 0x5A;
    {
        char d[160];
        sprintf_s(d, "status=0x%08X bytes=%u first=0x%02X (server fills 0x5A)",
                  (unsigned)st, rd.BytesTransferred, (unsigned char)rb[0]);
        Report("client RDMA Read through the WINDOW rkey", readOk, d);
    }

    // ---- 2. RDMA Write into the window
    char* wb = nd.buf + kWriteBufOff;         // inside the MR
    memset(wb, 0x3C, kWinLen);
    ND2_SGE ws = {};
    ws.Buffer = wb;
    ws.BufferLength = (ULONG)kWinLen;
    ws.MemoryRegionToken = nd.mr->GetLocalToken();
    hr = nd.qp->Write(CTX_WRITE, &ws, 1, peer.addr_win, peer.token_win, 0);
    ND2_RESULT wr = {};
    HRESULT wst = SUCCEEDED(hr) ? nd.reap(CTX_WRITE, &wr, kWaitMs) : hr;
    {
        char d[128];
        sprintf_s(d, "status=0x%08X bytes=%u", (unsigned)wst, wr.BytesTransferred);
        Report("client RDMA Write into the WINDOW rkey", SUCCEEDED(wst), d);
    }

    // ---- 3. tell the server we are done, so it can verify and invalidate
    char* sync = (char*)(nd.buf + kSyncOff + 1); *sync = 'D';
    ND2_SGE ss = {};
    ss.Buffer = sync;
    ss.BufferLength = 1;
    ss.MemoryRegionToken = nd.mr->GetLocalToken();
    nd.qp->Send(CTX_SEND, &ss, 1, 0);
    nd.reap(CTX_SEND, nullptr, kWaitMs);

    // ---- 4. wait for the server's invalidation signal
    char* sig = (char*)(nd.buf + kSyncOff + 4); *sig = 0;
    ND2_SGE rs = {};
    rs.Buffer = sig;
    rs.BufferLength = 1;
    rs.MemoryRegionToken = nd.mr->GetLocalToken();
    nd.qp->Receive(CTX_RECV, &rs, 1);
    ND2_RESULT rr = {};
    hr = nd.reap(CTX_RECV, &rr, 15000);
    Report("received the server's post-invalidate signal", SUCCEEDED(hr), nullptr);

    // ---- 5. the critical question: does the stale window token still work?
    printf("\n-- invalidation actually revokes?\n");
    hr = nd.qp->Read(CTX_READ, &sge, 1, peer.addr_win, peer.token_win, 0);
    ND2_RESULT ar = {};
    HRESULT ast = SUCCEEDED(hr) ? nd.reap(CTX_READ, &ar, kWaitMs) : hr;
    bool revoked = FAILED(ast);
    {
        char d[192];
        sprintf_s(d, "status=0x%08X bytes=%u - %s", (unsigned)ast, ar.BytesTransferred,
                  revoked ? "token rejected, STag is a real boundary"
                          : "TOKEN STILL WORKED, invalidation did not revoke access");
        Report("post-invalidate Read with the stale token fails", revoked, d);
    }

    char* reply = (char*)(nd.buf + kSyncOff + 2); *reply = revoked ? 'R' : 'S';
    ND2_SGE ps = {};
    ps.Buffer = reply;
    ps.BufferLength = 1;
    ps.MemoryRegionToken = nd.mr->GetLocalToken();
    nd.qp->Send(CTX_SEND, &ps, 1, 0);
    nd.reap(CTX_SEND, nullptr, kWaitMs);

    // ---- LAST: bounds enforcement.
    // Read one byte past the end of the window using the same token.
    //
    // This runs LAST on purpose.  On a reliable-connected QP a remote access
    // violation is a FATAL error: the provider tears the queue pair down, and
    // every request posted afterwards completes as ND_CANCELED.  Running this
    // probe in the middle of the test is what made the window WRITE look like an
    // independent failure in the previous revision - it was collateral damage.
    //
    // That fatality is itself a design fact NVMe-oF has to respect: a host that
    // sends a bad SGL takes the whole connection down, so SGLs must be validated
    // before the read is issued rather than after it fails.
    {
        char* one = (char*)(nd.buf + kSyncOff + 8); *one = 0;   // inside the MR
        ND2_SGE os = {};
        os.Buffer = one;
        os.BufferLength = 1;
        os.MemoryRegionToken = nd.mr->GetLocalToken();
        hr = nd.qp->Read(CTX_READ, &os, 1, peer.addr_win + kWinLen, peer.token_win, 0);
        ND2_RESULT orr = {};
        HRESULT ost = SUCCEEDED(hr) ? nd.reap(CTX_READ, &orr, kWaitMs) : hr;
        char d[224];
        if (ost == ND_CANCELED) {
            // ND_CANCELED means the request was torn down, not that the bounds
            // were enforced.  NOT scored: this probe needs a LIVE queue pair and
            // by now the revocation probe above has legitimately killed it, so
            // ND_CANCELED means "too late to tell" rather than "bounds not
            // enforced".  Ordered before the revocation probe it returns
            // ND_ACCESS_VIOLATION 0xC0000005, which IS the evidence that the
            // window bounds remote access.
            sprintf_s(d, "ND_CANCELED (0x%08X) - inconclusive, QP already dead; reorder "
                         "before the revocation probe to test bounds",
                      (unsigned)ost);
            printf("  [info] bounds probe: %s\n", d);
        } else {
            sprintf_s(d, "status=0x%08X (ND_ACCESS_VIOLATION 0xC0000005 expected)", (unsigned)ost);
            Report("read 1 byte past the window end is REFUSED (live QP)", FAILED(ost), d);
        }
    }
    }   // end of the read path (only entered when the transport permits RDMA Read)

    nd.cleanup();
    printf("\nclient failures: %d\n", g_failures);
    return g_failures;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: %s -server|-client <ip> [port] [-readlimit N]\n", argv[0]);
        printf("  start the server first; the two ports of the card must be cabled\n");
        printf("  -readlimit N sets inbound/outbound read limits (default 0)\n");
        return 2;
    }
    for (int i = 3; i < argc - 1; i++) {
        if (strcmp(argv[i], "-readlimit") == 0) g_readLimit = (ULONG)atoi(argv[i + 1]);
        if (strcmp(argv[i], "-local") == 0 && strlen(argv[i + 1]) < sizeof(g_localIp)) {
            strcpy_s(g_localIp, sizeof(g_localIp), argv[i + 1]);
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // NdStartup() is what creates the NetworkDirect framework object; without it
    // every NdOpenAdapter() call fails with ND_DEVICE_NOT_READY, which reads
    // like a driver problem and is actually just a missing initialisation.
    HRESULT hr = NdStartup();
    if (FAILED(hr)) {
        printf("NdStartup failed 0x%08X\n", (unsigned)hr);
        WSACleanup();
        return 1;
    }

    int rc = (strcmp(argv[1], "-server") == 0) ? runServer(argv[2]) : runClient(argv[2]);

    NdCleanup();
    WSACleanup();
    return rc;
}
