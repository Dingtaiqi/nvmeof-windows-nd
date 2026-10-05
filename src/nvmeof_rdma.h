// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
// nvmeof_rdma.h - NetworkDirect transport for the NVMe-oF implementation.
//
// One Device (adapter + registered region) can carry several Queues (one queue
// pair each).  That split is not cosmetic: NVMe-oF/RDMA gives every submission
// queue its own queue pair, which is the single biggest structural difference
// from the TCP transport, where all queues multiplex one connection.  An
// implementation that keeps one QP cannot express the protocol.
//
// Every rule in here was paid for in stag_smoketest.cpp and f1_bringup.cpp -
// see DESIGN.md section 8.  The ones that bite hardest, repeated because they
// have each cost a debugging cycle:
//
//   * NdStartup() before any NdOpenAdapter, or it fails ND_DEVICE_NOT_READY.
//   * outboundReadLimit must be non-zero or this side cannot issue RDMA Read
//     at all, and the failure is a raw 0xC000003E that appears in no header.
//   * Every SGE buffer must lie inside the region named by its token.  A stack
//     buffer with the region's token fails with ND_ACCESS_VIOLATION (on Send)
//     or 0xC000003E (on Read/Write).  Use region() + an offset.  This has been
//     got wrong three times now.
//   * A two-sided Send needs its Receive posted first, or the send times out
//     and the *peer* is the side that reports the failure.
//   * ND_TIMEOUT (0x00000102) and ND_PENDING (0x00000103) are success-severity,
//     so SUCCEEDED() reports a timed-out wait as a pass.  Use ndOk().
//   * RDMA Write completions report BytesTransferred == 0 on this provider.
//     Never validate a write by its byte count; validate the destination.
//
// GetPrivateData ordering (target: between GetConnectionRequest and Accept;
// initiator: between Connect and CompleteConnect) is not enforced here.  Every
// suite in this project now uses private data - it is not optional in practice:
// nvmet rejects a Connect that carries none - so this ordering is load-bearing.
// See nvmeof_rdma_fill_req()/nvmeof_rdma_check_req() in nvmeof_wire.h for the
// field semantics a real target enforces.

#ifndef NVMEOF_RDMA_H
#define NVMEOF_RDMA_H

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
#include <stdint.h>
#include <string.h>

// The wire definitions (status codes, the Connect private-data layout, the SGL
// helpers) are what makes acceptChecked() below able to validate a connection the
// way a Linux target does.
#include "nvmeof_wire.h"

namespace nvmeof {

// Completing a read requires the peer to be allowed to have reads outstanding
// against us.  Zero here silently disables RDMA Read entirely.
static const ULONG kReadLimit   = 16;

// Publish the token UNSWAPPED instead of through ndWireKey().  This exists to answer one question
// with one variable: the swapped form is measured-good for a Linux target's RDMA READ of our
// connect data (that is why fabrics Connect passes), yet every command that needs the target to
// RDMA WRITE into our buffer fails.  If publishing the raw token makes the writes land while
// breaking Connect's read, the quirk applies to READ transactions only and the fix must choose the
// key form per transaction direction.  Flipped by -rawwkey; default off so nothing changes silently.
static bool g_publishRawRkey = false;

// ===========================================================================
//  The rkey this driver puts on the wire is byte-swapped - compensate here
// ===========================================================================
//
// WinOF 5.50 on this ConnectX-3 exchanges the 32-bit rkey of an RDMA READ
// transaction in the opposite byte order from every other implementation, BOTH when
// it issues the request and when it validates one it receives - so it is completely
// invisible between two peers that share the driver, and fatal against any peer that
// does not.  Measured, not inferred:
//
//   * our initiator's Connect against Linux nvmet: the target's RDMA READ of our
//     1024-byte connect data failed "remote access error (10)" until the token we
//     published was byte-swapped, then it succeeded;
//   * our target's read of a Linux host's connect data: ND_ACCESS_VIOLATION until
//     the key we passed to Read() was byte-swapped, then it succeeded;
//   * CX3-to-CX3 reads of the same buffers, keys and lengths work either way, which
//     is why this survived every self-test in this project.
//
// So: swap on both sides of the boundary.  A token we PUBLISH becomes the swapped
// value, and a token we CONSUME (parsed out of a peer's SGL) is swapped back before
// it is handed to the provider.  With that, both a Linux peer and our own peer work.
// The remote ADDRESS is not affected - the same experiments show it goes on the wire
// unchanged.
static inline uint32_t ndWireKey(uint32_t token) {
    return ((token & 0x000000ffu) << 24) | ((token & 0x0000ff00u) << 8) |
           ((token & 0x00ff0000u) >> 8)  | ((token & 0xff000000u) >> 24);
}
static const ULONG kCqDepth     = 256;
static const ULONG kQpDepth     = 256;
static const DWORD kWaitMs      = 5000;

static inline bool ndOk(HRESULT hr) {
    return SUCCEEDED(hr) && hr != ND_TIMEOUT && hr != ND_PENDING;
}

static inline const char* ndStr(HRESULT hr, char* buf, size_t n) {
    const char* name =
        hr == ND_SUCCESS          ? "ND_SUCCESS" :
        hr == ND_TIMEOUT          ? "ND_TIMEOUT" :
        hr == ND_PENDING          ? "ND_PENDING" :
        hr == ND_CANCELED         ? "ND_CANCELED" :
        hr == ND_IO_TIMEOUT       ? "ND_IO_TIMEOUT" :
        hr == ND_ACCESS_VIOLATION ? "ND_ACCESS_VIOLATION" :
        hr == ND_BUFFER_OVERFLOW  ? "ND_BUFFER_OVERFLOW" :
        hr == ND_INVALID_DEVICE_STATE ? "ND_INVALID_DEVICE_STATE" :
        hr == ND_CONNECTION_REFUSED   ? "ND_CONNECTION_REFUSED" :
        hr == ND_CONNECTION_ACTIVE    ? "ND_CONNECTION_ACTIVE" : "";
    sprintf_s(buf, n, "0x%08X%s%s", (unsigned)hr, *name ? " " : "", name);
    return buf;
}

// ---------------------------------------------------------------------------
//  Device: one adapter plus one registered region shared by every queue.
// ---------------------------------------------------------------------------
struct Device {
    IND2Adapter*      adapter = nullptr;
    IND2MemoryRegion* mr = nullptr;
    HANDLE            ovFile = INVALID_HANDLE_VALUE;
    HANDLE            event = nullptr;
    OVERLAPPED        ov = {};      // registrations (used by Device::open)
    uint8_t*          reg = nullptr;
    size_t            regSize = 0;
    uint32_t          rkey = 0;      // remote token for the whole region

    uint8_t* region(size_t off) { return reg + off; }

    bool open(const struct sockaddr_in& local, size_t regionBytes) {
        regSize = regionBytes;
        HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&local,
                                   sizeof(local), (VOID**)&adapter);
        if (FAILED(hr)) { printf("NdOpenAdapter failed 0x%08X\n", (unsigned)hr); return false; }
        if (FAILED(hr = adapter->CreateOverlappedFile(&ovFile))) {
            printf("CreateOverlappedFile 0x%08X\n", (unsigned)hr); return false;
        }
        // Registration limits are not advisory: a region larger than
        // MaxRegistrationSize can be accepted by Register() and then have every
        // operation on it fail, which presents as "the peer's Receive was
        // cancelled" rather than as a registration error.  Report the ceiling
        // next to what we asked for so that failure mode is self-diagnosing.
        {
            ND2_ADAPTER_INFO info = {};
            info.InfoVersion = ND_VERSION_2;
            ULONG sz = sizeof(info);
            if (SUCCEEDED(adapter->Query(&info, &sz))) {
                printf("  adapter: maxRegistration=%llu MiB maxWindow=%llu MiB "
                       "maxTransfer=%u MiB\n",
                       (unsigned long long)(info.MaxRegistrationSize >> 20),
                       (unsigned long long)(info.MaxWindowSize >> 20),
                       (unsigned)(info.MaxTransferLength >> 20));
                if (regionBytes > info.MaxRegistrationSize) {
                    printf("  FATAL: asked to register %zu MiB but the adapter caps "
                           "a single registration at %llu MiB\n",
                           regionBytes >> 20,
                           (unsigned long long)(info.MaxRegistrationSize >> 20));
                    return false;
                }
            }
        }
        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        ov.hEvent = event;
        hr = adapter->CreateMemoryRegion(IID_IND2MemoryRegion, ovFile, (VOID**)&mr);
        if (FAILED(hr)) { printf("CreateMemoryRegion 0x%08X\n", (unsigned)hr); return false; }

        reg = (uint8_t*)VirtualAlloc(nullptr, regSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!reg) { printf("VirtualAlloc(%zu) failed\n", regSize); return false; }
        memset(reg, 0, regSize);
        // EXPERIMENT RESULT: RDMA_READ_SINK is restored - removing it changed nothing
        // about the inbound-write failure (that was an MTU mismatch), and the STag
        // smoke test established that this flag is what allows a region to be the
        // sink of an RDMA read, which our own target's reads rely on.
        hr = mr->Register(reg, regSize,
                          ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                          ND_MR_FLAG_ALLOW_REMOTE_READ |
                          ND_MR_FLAG_ALLOW_REMOTE_WRITE |
                          ND_MR_FLAG_RDMA_READ_SINK, &ov);
        if (hr == ND_PENDING) hr = waitOverlapped(mr, kWaitMs);
        if (!ndOk(hr)) { char b[64]; printf("Register %s\n", ndStr(hr, b, sizeof(b))); return false; }
        // The token a peer uses: see the note on ndWireKey above.
        const uint32_t rawToken = mr->GetRemoteToken();
        rkey = g_publishRawRkey ? rawToken : ndWireKey(rawToken);
        printf("    [key] raw=0x%08X wire=0x%08X published=0x%08X (%s)\n",
               rawToken, ndWireKey(rawToken), rkey, g_publishRawRkey ? "RAW (-rawwkey)" : "swapped");
        return true;
    }

    HRESULT waitOverlapped(IND2Overlapped* obj, DWORD ms) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            HRESULT hr = obj->GetOverlappedResult(&ov, FALSE);
            if (hr != ND_PENDING) return hr;
            if (GetTickCount64() - t0 >= ms) {
                obj->CancelOverlappedRequests();
                obj->GetOverlappedResult(&ov, FALSE);
                return ND_TIMEOUT;
            }
            SwitchToThread();
        }
    }

    // Every queue is torn down before the region is released: a QP still
    // reading the region while it is dereigstered is a use-after-free the
    // provider cannot protect against.
    void close(struct Queue* adminQ, struct Queue* ioQ, int nIo);

    ~Device() { /* callers must close queues first; see close() */ }
};

// ---------------------------------------------------------------------------
//  Queue: one QP with its own completion queue and its own connection.
// ---------------------------------------------------------------------------
struct Queue {
    Device*              dev = nullptr;
    IND2CompletionQueue* cq = nullptr;
    IND2Connector*       conn = nullptr;
    IND2QueuePair*       qp = nullptr;
    OVERLAPPED           ov = {};
    HANDLE               event = nullptr;
    uint16_t             qid = 0;
    uint64_t             posted = 0;    // commands sent/received on this queue

    bool create(Device* d, uint16_t queueId) {
        dev = d; qid = queueId;
        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        ov.hEvent = event;
        HRESULT hr = dev->adapter->CreateCompletionQueue(IID_IND2CompletionQueue, dev->ovFile,
                                                         kCqDepth, 0, 0, (VOID**)&cq);
        if (FAILED(hr)) { printf("CreateCompletionQueue 0x%08X\n", (unsigned)hr); return false; }
        hr = dev->adapter->CreateConnector(IID_IND2Connector, dev->ovFile, (VOID**)&conn);
        if (FAILED(hr)) { printf("CreateConnector 0x%08X\n", (unsigned)hr); return false; }
        // maxInitiatorSge = 1 means every request this QP posts has exactly one
        // SGE - which is true here and is why the capsule and its payload use
        // separate registered buffers rather than one scatter list.
        hr = dev->adapter->CreateQueuePair(IID_IND2QueuePair, cq, cq, nullptr,
                                           kQpDepth, kQpDepth, 1, 1, 0, (VOID**)&qp);
        if (FAILED(hr)) { printf("CreateQueuePair 0x%08X\n", (unsigned)hr); return false; }
        return true;
    }

    void destroy() {
        if (conn) { conn->Disconnect(&ov); waitOverlapped(conn, 1500); }
        if (qp) { qp->Release(); qp = nullptr; }
        if (cq) { cq->Release(); cq = nullptr; }
        if (conn) { conn->Release(); conn = nullptr; }
        if (event) { CloseHandle(event); event = nullptr; }
    }

    // ---- target side: the connection handshake a real target performs ----
    //
    // Read the peer's RDMA-CM private data, validate it exactly as nvmet does,
    // then Accept - sending the same 32-byte reply nvmet sends - or Reject with
    // the status the reference would use.
    //
    // WHY EVERY TARGET IN THIS PROJECT GOES THROUGH HERE
    // This project shipped two interoperability bugs that no test could see,
    // because both of our endpoints agreed with each other and neither agreed
    // with the world: three initiators sent NO private data at all (nvmet rejects
    // that with NVME_RDMA_CM_INVALID_LEN) and two sent hsqsize one too large
    // (nvmet rejects that with NVME_RDMA_CM_INVALID_HSQSIZE).  Every target here
    // accepted them, because none of them looked.  A target that is more
    // forgiving than the world is a target that hides the bug until a real host
    // arrives.
    //
    // The caller MUST have armed its Receive already: Accept is what releases the
    // peer's CompleteConnect, so a Receive posted afterwards races a peer that is
    // already entitled to send (DESIGN 8.10(4)).
    //
    // Returns 0 = accepted, 1 = rejected (the peer was told why), -1 = the
    // Accept itself failed.
    int acceptChecked(uint16_t rqDepth, uint16_t* claimedQid, uint16_t* rejStatus) {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        ULONG len = sizeof(buf);
        HRESULT hr = conn->GetPrivateData(buf, &len);

        nvmeof_rdma_request_pd req;
        memcpy(&req, buf, sizeof(req));
        uint16_t peerQid = 0;
        uint16_t sts = FAILED(hr) ? (uint16_t)NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH
                                  : nvmeof_rdma_check_req(&req, len, &peerQid);
        if (claimedQid) *claimedQid = peerQid;
        printf("  [conn] private data: hr=0x%08X len=%u recfmt=%u qid=%u hrqsize=%u hsqsize=%u\n",
               (unsigned)hr, len, req.recfmt, req.qid, req.hrqsize, req.hsqsize);

        if (FAILED(hr) || sts != 0) {
            nvmeof_rdma_reject_pd rej;
            rej.recfmt = 0;
            rej.sts = sts ? sts : (uint16_t)NVMEOF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH;
            printf("  [conn] rejecting with nvme_rdma status %u\n", rej.sts);
            conn->Reject(&rej, sizeof(rej));
            if (rejStatus) *rejStatus = rej.sts;
            return 1;
        }

        nvmeof_rdma_accept_pd acc;
        acc.recfmt = 0;
        acc.crqsize = rqDepth;
        hr = conn->Accept(qp, kReadLimit, kReadLimit, &acc, sizeof(acc), &ov);
        if (hr == ND_PENDING) hr = waitOverlapped(conn, kWaitMs);
        if (!ndOk(hr)) return -1;
        printf("  [conn] accepted; advertised crqsize=%u\n", acc.crqsize);
        return 0;
    }

    // Replace the connector, keeping the queue pair and completion queue.
    //
    // A connector is effectively single-use for one connection attempt.  After a
    // Reject, a refused Connect, or a completed connection it cannot be handed to
    // another GetConnectionRequest or Connect, and the failure is not a clear
    // "this connector is spent": the initiator gets ND_CONNECTION_ACTIVE on its
    // *next* Connect, and the target's listener call simply fails.  Both were
    // observed here.  Swapping in a fresh connector is cheaper than reasoning
    // about which internal states happen to be reusable.
    bool newConnector() {
        if (conn) { conn->Release(); conn = nullptr; }
        HRESULT hr = dev->adapter->CreateConnector(IID_IND2Connector, dev->ovFile, (VOID**)&conn);
        if (FAILED(hr)) { printf("CreateConnector 0x%08X\n", (unsigned)hr); conn = nullptr; return false; }
        return true;
    }

    HRESULT waitOverlapped(IND2Overlapped* obj, DWORD ms) {
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            HRESULT hr = obj->GetOverlappedResult(&ov, FALSE);
            if (hr != ND_PENDING) return hr;
            if (GetTickCount64() - t0 >= ms) {
                obj->CancelOverlappedRequests();
                obj->GetOverlappedResult(&ov, FALSE);
                return ND_TIMEOUT;
            }
            SwitchToThread();
        }
    }

    // ---- completions that did not match the context a caller asked for ----
    //
    // They are STASHED here, not dropped, and this is a fix rather than a tidy-up.  reap() used to
    // read a non-matching completion out of the CQ and throw it away, which is fatal to any caller
    // that reaps two different contexts on one queue: submitCommand waits for the capsule-send
    // completion (CTX_CAP) and then for the response completion (CTX_RESP), so when the response
    // arrives first it was consumed and discarded by the first reap and the second waited forever.
    // Against a Linux nvmet target that is exactly the observed shape - the FIRST admin command
    // passes and every later one times out.  The target side of this library already queues instead
    // of dropping ("They are queued, not dropped", f5_interop.cpp:2420); this makes the host side agree.
    static const size_t kStashSlots = 64;
    ND2_RESULT stash[kStashSlots] = {};
    size_t     stashCount = 0;
    unsigned   stashHits  = 0;      // how often an out-of-order completion was saved rather than lost
    unsigned   stashLost  = 0;      // non-zero means the stash overflowed - that IS a defect

    // Reap one completion, optionally requiring a context.  A wildcard reap is
    // available but every use of it is a place a stray CQE can be swallowed,
    // so callers that use it must say why.
    //
    // The yield here was suspected of causing the 12-22 ms completion delay measured
    // on the serial path (see DESIGN 8.67) and was replaced with a pause-spin: the
    // delay did not change (33.8 ms median before, 33.8 ms after), so the yield is
    // NOT the cause and the original is kept.  Recorded because "we tried the obvious
    // thing and it was not it" is worth as much as a fix.
    HRESULT reap(void* expect, ND2_RESULT* out, DWORD ms) {
        if (!cq) return ND_UNSUCCESSFUL;
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            // A stashed completion that matches is the answer, and it is checked FIRST: it may have
            // arrived before this call was even made.
            for (size_t i = 0; i < stashCount; i++) {
                ND2_RESULT s = stash[i];
                if (!expect || s.RequestContext == expect) {
                    for (size_t k = i + 1; k < stashCount; k++) stash[k - 1] = stash[k];
                    stashCount--;
                    if (out) *out = s;
                    return s.Status;
                }
            }
            ND2_RESULT r = {};
            if (cq->GetResults(&r, 1) == 1) {
                if (expect && r.RequestContext != expect) {
                    if (stashCount < kStashSlots) {
                        stash[stashCount++] = r;
                        stashHits++;
                    } else {
                        stashLost++;
                        printf("    (queue %u: stash FULL (%u lost) ctx=%p type=%d st=0x%08X)\n",
                               qid, stashLost, r.RequestContext, (int)r.RequestType, (unsigned)r.Status);
                    }
                    continue;
                }
                if (out) *out = r;
                return r.Status;
            }
            if (GetTickCount64() - t0 >= ms) return ND_TIMEOUT;
            SwitchToThread();
        }
    }
    // Non-blocking variant, for an event loop that polls several queues.
    bool poll(ND2_RESULT* out) {
        if (!cq) return false;
        return cq->GetResults(out, 1) == 1;
    }

    // One non-blocking check on an outstanding overlapped request.  Returns
    // ND_PENDING while it is still outstanding.
    //
    // This is deliberately not waitOverlapped(ms=0): that would treat "not done
    // yet" as a timeout and call CancelOverlappedRequests, destroying the very
    // request being polled.  A caller that needs to keep servicing other queues
    // while a connection request is outstanding needs this instead - a target
    // that blocks in GetConnectionRequest stops answering the admin queue, and
    // the host then sees every subsequent admin command time out.
    HRESULT pollOverlapped(IND2Overlapped* obj) {
        if (!obj) return ND_UNSUCCESSFUL;
        return obj->GetOverlappedResult(&ov, FALSE);
    }

    // ---- two-sided ----
    bool postReceive(void* buf, ULONG len, void* ctx) {
        ND2_SGE s = {};
        s.Buffer = buf; s.BufferLength = len; s.MemoryRegionToken = dev->mr->GetLocalToken();
        return ndOk(qp->Receive(ctx, &s, 1));
    }
    bool send(const void* buf, ULONG len, void* ctx) {
        ND2_SGE s = {};
        s.Buffer = (void*)buf; s.BufferLength = len; s.MemoryRegionToken = dev->mr->GetLocalToken();
        return ndOk(qp->Send(ctx, &s, 1, 0));
    }
    // ---- one-sided ----
    //
    // remoteKey arrives from a peer's SGL descriptor, i.e. it is a WIRE token, and
    // this driver wants it byte-swapped (see ndWireKey).  Every caller therefore
    // passes the key exactly as it came off the wire.
    bool read(void* local, ULONG len, uint64_t remoteAddr, uint32_t remoteKey, void* ctx) {
        ND2_SGE s = {};
        s.Buffer = local; s.BufferLength = len; s.MemoryRegionToken = dev->mr->GetLocalToken();
        return ndOk(qp->Read(ctx, &s, 1, remoteAddr, ndWireKey(remoteKey), 0));
    }
    bool write(const void* local, ULONG len, uint64_t remoteAddr, uint32_t remoteKey, void* ctx) {
        ND2_SGE s = {};
        s.Buffer = (void*)local; s.BufferLength = len; s.MemoryRegionToken = dev->mr->GetLocalToken();
        return ndOk(qp->Write(ctx, &s, 1, remoteAddr, ndWireKey(remoteKey), 0));
    }
};

// Device::close needs Queue to be complete, so it is defined here rather than
// inline above.
inline void Device::close(Queue* adminQ, Queue* ioQ, int nIo) {
    if (adminQ) adminQ->destroy();
    for (int i = 0; i < nIo; i++) if (ioQ) ioQ[i].destroy();
    if (mr) { mr->Deregister(&ov); waitOverlapped(mr, 1500); mr->Release(); mr = nullptr; }
    if (ovFile != INVALID_HANDLE_VALUE) { CloseHandle(ovFile); ovFile = INVALID_HANDLE_VALUE; }
    if (adapter) { adapter->Release(); adapter = nullptr; }
    if (event) { CloseHandle(event); event = nullptr; }
    if (reg) { VirtualFree(reg, 0, MEM_RELEASE); reg = nullptr; }
}

} // namespace nvmeof

#endif // NVMEOF_RDMA_H
