// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef NVMEOF_ISCSI_H
#define NVMEOF_ISCSI_H
// ===========================================================================
//  nvmeof_iscsi.h - a minimal iSCSI target, so a REAL disk can appear on Windows.
//
//  WHY THIS EXISTS.  NVMe-oF is network mapping and our stack already does it: every
//  block is fetched over RDMA on demand.  What Windows lacks is a kernel-mode
//  NVMe-oF initiator (Windows Server has one; client SKUs do not), and no user-mode
//  process can be handed to the volume stack as a disk.  Windows DOES have a
//  kernel-mode iSCSI initiator built in, so this file implements the other end of
//  that protocol: iSCSI in, NVMe-oF out.  The disk that appears in Disk Management
//  is live, of any size, with no local copy - the 1 TB stays on the Linux machine.
//
//  WHERE THE LAYOUTS COME FROM.  Not from memory.  Every field below was checked
//  against two things: RFC 7143 section 11, and a capture of the real Windows
//  initiator (tools_iscsi_capture.ps1).  The capture pinned four things a guess
//  would have got wrong:
//
//    * Login Request byte 1 is  T(bit 7) | reserved(6:4) | CSG(3:2) | NSG(1:0).
//      The observed 0x81 is T=1, CSG=0 (Security), NSG=1 (Operational) - which is
//      only self-consistent with CSG/NSG in the LOW five bits.
//    * Bytes 8-15 of a Login PDU are ISID(6) + TSIH(2), NOT a LUN: Windows sent
//      ISID=40 00 01 37 00 00 (its OUI) and TSIH=0.
//    * Bytes 20-21 of a Login Request are the CID: Windows sent 0x0001.
//    * In a SCSI Response the STATUS is byte 3 and the response code is byte 2.
//      The first version of this file had them the other way round - a mistake that
//      produces a disk that answers every command and is never usable.
//
//  WHAT IS DELIBERATELY NOT SUPPORTED, with the reason:
//    * CHAP authentication       - the session is loopback-only (listenLoopback)
//    * digests (header/data CRC) - negotiated to None; the transport is localhost
//    * multiple connections      - MaxConnections=1, ErrorRecoveryLevel=0
//    * immediate data on writes  - InitialR2T=Yes, so every write is R2T-driven and
//      one 64 KiB buffer serves every transfer instead of "whatever the initiator
//      decides to send unsolicited"
//
//  SUPPORTED (the set a Windows disk needs): Login (all three stages), SendTargets,
//  NOP, Logout, Task Management (answered), INQUIRY + VPD 0x00/0x80/0x83, TEST UNIT
//  READY, REQUEST SENSE, READ CAPACITY(10)/(16), REPORT LUNS, MODE SENSE(6)/(10),
//  START STOP UNIT, READ(10)/(16), WRITE(10)/(16) via R2T, SYNCHRONIZE CACHE.
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

// ---------------------------------------------------------------------------
//  Data-path diagnostics are OFF unless asked for, and that default is measured,
//  not a matter of taste.  With these traces enabled (and unbuffered stdout, see
//  main), a 256 KiB sequential read through this bridge costs 45.9 ms per command
//  = 5.4 MB/s.  With them off, the same read runs at 85.7 MB/s.  The traces are
//  what makes a stuck handshake debuggable, and they are worth 16x on a data path
//  only when something is broken - so: off by default, -iscsitrace to turn them on.
// ---------------------------------------------------------------------------
static bool g_iscsiTrace = false;
//  Aggregate data-path timing (-iscsitime), separate from the per-PDU trace.
//  The per-PDU trace is 16x too expensive to measure with - it perturbs what it
//  measures - but throughput alone cannot say WHERE the ~0.5 ms per Data-In PDU
//  goes: it is either this bridge's NVMe read plus socket write, or the Windows
//  initiator's receive path.  One line per SCSI command, three numbers, answers
//  it: total, NVMe-staging, and Data-In-send.  Cheap enough to leave on.
static bool g_iscsiTime = false;
#include <thread>

// ---------------------------------------------------------------------------
//  Opcodes (RFC 7143 section 11.2.1).  The 0x20 bit marks a response.
// ---------------------------------------------------------------------------
enum {
    ISCSI_OP_NOP_OUT       = 0x00, ISCSI_OP_SCSI_CMD      = 0x01,
    ISCSI_OP_TASK_MGMT_REQ = 0x02, ISCSI_OP_LOGIN_REQ     = 0x03,
    ISCSI_OP_TEXT_REQ      = 0x04, ISCSI_OP_DATA_OUT      = 0x05,
    ISCSI_OP_LOGOUT_REQ    = 0x06, ISCSI_OP_SNACK_REQ     = 0x10,
    ISCSI_OP_NOP_IN        = 0x20, ISCSI_OP_SCSI_RSP      = 0x21,
    ISCSI_OP_TASK_MGMT_RSP = 0x22, ISCSI_OP_LOGIN_RSP     = 0x23,
    ISCSI_OP_TEXT_RSP      = 0x24, ISCSI_OP_DATA_IN       = 0x25,
    ISCSI_OP_LOGOUT_RSP    = 0x26, ISCSI_OP_R2T           = 0x31,
    ISCSI_OP_REJECT        = 0x3F
};

// Login stages: Security(0) -> Operational(1) -> FullFeature(3).  Stage 2 is not
// defined by the RFC, which is why the values are 0, 1 and 3.
enum {
    ISCSI_STAGE_SECURITY   = 0,
    ISCSI_STAGE_OPERATIONAL = 1,
    ISCSI_STAGE_FULL_FEATURE = 3
};

// SCSI status (SAM-5) and sense keys (SPC-4).
enum {
    SCSI_STATUS_GOOD           = 0x00,
    SCSI_STATUS_CHECK_COND     = 0x02,
    SCSI_SENSE_NO_SENSE        = 0x00,
    SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
    SCSI_SENSE_DATA_PROTECT    = 0x07
};
enum { SCSI_ASC_INVALID_OPCODE = 0x20, SCSI_ASC_LBA_OUT_OF_RANGE = 0x21,
       SCSI_ASC_INVALID_FIELD   = 0x24, SCSI_ASC_WRITE_PROTECTED  = 0x27 };

// ---------------------------------------------------------------------------
//  Big-endian access.  Everything on an iSCSI wire is network order, and reading it
//  little-endian yields values that look plausible and fail far from the cause.
// ---------------------------------------------------------------------------
static inline uint16_t iscsi_rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t iscsi_rd24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static inline uint32_t iscsi_rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t iscsi_rd64(const uint8_t* p) {
    return ((uint64_t)iscsi_rd32(p) << 32) | iscsi_rd32(p + 4);
}
static inline void iscsi_wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void iscsi_wr24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static inline void iscsi_wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void iscsi_wr64(uint8_t* p, uint64_t v) {
    iscsi_wr32(p, (uint32_t)(v >> 32)); iscsi_wr32(p + 4, (uint32_t)v);
}

// ---------------------------------------------------------------------------
//  The BHS: 48 bytes in every PDU.  Accessors are named after the FIELD, so a call
//  site reads like the RFC table rather than like an offset.
// ---------------------------------------------------------------------------
struct IscsiBhs {
    uint8_t raw[48];

    uint8_t  opcode()     const { return raw[0] & 0x3F; }
    bool     immediate()  const { return (raw[0] & 0x40) != 0; }
    uint8_t  ahsLen()     const { return raw[4]; }
    uint32_t dataLen()    const { return iscsi_rd24(raw + 5); }
    uint64_t lun()        const { return iscsi_rd64(raw + 8); }
    uint32_t itt()        const { return iscsi_rd32(raw + 16); }

    // Login (request and response share these)
    bool     loginT()     const { return (raw[1] & 0x80) != 0; }
    uint8_t  loginCsg()   const { return (raw[1] >> 2) & 0x03; }
    uint8_t  loginNsg()   const { return raw[1] & 0x03; }
    uint8_t  loginVerMax()const { return raw[2]; }
    uint8_t  loginVerMin()const { return raw[3]; }
    uint32_t loginCid()   const { return iscsi_rd16(raw + 20); }
    // Login Response only
    uint8_t  loginStatusClass()  const { return raw[36]; }
    uint8_t  loginStatusDetail() const { return raw[37]; }

    // SCSI Command
    bool     scsiRead()   const { return (raw[1] & 0x80) != 0; }
    bool     scsiWrite()  const { return (raw[1] & 0x40) != 0; }
    uint32_t edtl()       const { return iscsi_rd32(raw + 20); }
    uint32_t cmdSn()      const { return iscsi_rd32(raw + 24); }
    uint32_t expStatSn()  const { return iscsi_rd32(raw + 28); }
    const uint8_t* cdb()  const { return raw + 32; }

    // Data-In / Data-Out / R2T
    uint32_t ttt()        const { return iscsi_rd32(raw + 20); }
    uint32_t dataSn()     const { return iscsi_rd32(raw + 36); }
    uint32_t bufferOff()  const { return iscsi_rd32(raw + 40); }
    uint32_t r2tSn()      const { return iscsi_rd32(raw + 36); }
    uint32_t desiredLen() const { return iscsi_rd32(raw + 44); }
    bool     fBit()       const { return (raw[1] & 0x80) != 0; }
    bool     sBit()       const { return (raw[1] & 0x08) != 0; }

    // Text
    bool     textF()      const { return (raw[1] & 0x80) != 0; }
    bool     textC()      const { return (raw[1] & 0x40) != 0; }
};

// ---------------------------------------------------------------------------
//  Text parameters (RFC 7143 section 6.1): "Key=Value" entries, each terminated by
//  NUL, and the whole data segment of a Login/Text PDU.
// ---------------------------------------------------------------------------
struct IscsiText {
    std::vector<std::pair<std::string, std::string>> kv;

    void parse(const uint8_t* data, uint32_t len) {
        kv.clear();
        uint32_t i = 0;
        while (i < len) {
            uint32_t j = i;
            while (j < len && data[j] != 0) j++;
            std::string e((const char*)data + i, j - i);
            i = j + 1;
            if (e.empty()) continue;
            size_t eq = e.find('=');
            if (eq == std::string::npos) kv.push_back({ e, "" });
            else kv.push_back({ e.substr(0, eq), e.substr(eq + 1) });
        }
    }
    std::string get(const char* key, const char* dflt = "") const {
        for (auto& p : kv) if (p.first == key) return p.second;
        return dflt;
    }
    std::string build() const {
        std::string s;
        for (auto& p : kv) { s += p.first; s += '='; s += p.second; s += '\0'; }
        return s;
    }
    // A text data segment is a NUL-terminated key list, and there are exactly three
    // rules - all three measured against the real Windows initiator, each after
    // getting it wrong in a different way:
    //
    //   1. the TEXT LENGTH INCLUDES the final NUL terminator, and
    //   2. the PDU is padded to 4 bytes with zeros that are NOT part of that length,
    //   3. so DataSegmentLength = text bytes including its terminator, excluding pad.
    //
    // Failure modes seen, in order, while pinning this down:
    //   * length 15 for "AuthMethod=None" and 15 bytes sent -> no terminator at all,
    //     stream desynchronised, connection closed with no message;
    //   * length 108 for a 106-byte text plus 2 pad zeros -> Windows parsed the two
    //     zeros as empty keys, answered "=NotUnderstood" twice, and re-sent the same
    //     Login Request (3 keys + 3 pad -> three reports, 0 keys + 4 pad -> four:
    //     the count matched the padding byte for byte);
    //   * length 15 for a 16-byte text (terminator stripped) -> "Authentication
    //     Failure", because the key list arrived without its terminator.
    struct TextSegment {
        std::string wire;    // what goes on the wire: 4-byte aligned
        uint32_t    length;  // what goes in DataSegmentLength
    };
    static TextSegment makeText(std::string text) {
        TextSegment t = { std::string(), 0 };
        if (text.empty()) return t;                 // no keys: send nothing at all
        if (text.back() != '\0') text.push_back('\0');
        t.length = (uint32_t)text.size();           // the terminator counts
        while (text.size() % 4) text.push_back('\0');  // the padding does not
        t.wire = text;
        return t;
    }
};

// ---------------------------------------------------------------------------
//  What the iSCSI target needs from whatever is behind it.
// ---------------------------------------------------------------------------
class IscsiBackend {
public:
    virtual ~IscsiBackend() {}
    virtual uint64_t blocks() const = 0;              // namespace size, 512 B blocks
    virtual uint32_t blockSize() const { return 512; }
    virtual const char* serial() const = 0;           // VPD 0x80 / 0x83
    virtual const char* model() const = 0;
    virtual bool writable() const = 0;
    // Largest NVMe transfer this backend can stage in one command, in bytes; 0 means
    // "no preference, keep the iSCSI chunk size".  It exists so the target can size
    // an NVMe READ independently of the Data-In PDU size: the two limits come from
    // different places (the initiator's MaxRecvDataSegmentLength versus the backend's
    // registered staging buffer), and pinning them together is what made a 1 MiB READ
    // cost sixteen submit-and-wait round trips instead of four (DESIGN 8.57).
    virtual uint32_t maxTransfer() const { return 0; }
    virtual bool read(uint64_t lba, uint32_t nblocks, void* buf) = 0;
    virtual bool write(uint64_t lba, uint32_t nblocks, const void* buf) = 0;
    virtual bool flush() = 0;
    // Called between commands.  The NVMe backend uses it to keep the far end's
    // keep-alive timer fed: a long iSCSI session can easily outlive KATO, and a
    // Linux nvmet that stops hearing from us tears the controller down mid-session.
    virtual void tick() {}
};

// ---------------------------------------------------------------------------
//  The target engine: one connection, one command at a time.
//
//  Serialising commands is not laziness - the backend is an NVMe-oF queue pair,
//  which is not thread-safe - but it does mean queue depth 1 behind Windows' own
//  pipelining, and that is worth stating rather than hiding.
// ---------------------------------------------------------------------------
class IscsiTarget {
public:
    IscsiTarget() : listenSock(INVALID_SOCKET), conn(INVALID_SOCKET),
                    loggedIn(false),
                    discovery(false), nextTtt(1), nextTsih(1), cmds(0), bytesIn(0), bytesOut(0) {
        // StatSN starts at a value the initiator cannot predict, exactly as LIO does
        // (measured: 0xD8742C2D on one connection, 0x1B9FA59F on the next).  This was
        // a real difference against the reference: our Login Response was a byte-for-
        // byte match to LIO's except for this field and the alias text, and the
        // reference never starts its status counter at 1.
        uint32_t seed = (uint32_t)(GetTickCount64() * 2654435761ull);
        statSn = seed | 1u;         // never zero: 0 is the "not set" value on the wire
        expCmdSn = 1;               // as LIO: MaxCmdSN == ExpCmdSN during login
        maxCmdSn = 1;
    }
    ~IscsiTarget() { stop(); }

    // Bind to LOOPBACK only.  This is a safety rail, not a default: nvmet has no
    // read-only namespace attribute, so anything that can reach this port can write
    // to the disk behind it.  Exposing it on a LAN interface would hand the user's
    // 1 TB disk to the network.
    bool listenLoopback(uint16_t port, const char* iqn, uint32_t chunk,
                        const char* bindIp = nullptr) {
        targetIqn = iqn;
        maxChunk = chunk ? chunk : 65536;
        listenPort = port;
        bindAddr = bindIp ? bindIp : "127.0.0.1";
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return false;
        BOOL yes = TRUE;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr(bindAddr.c_str());
        if (bind(listenSock, (sockaddr*)&a, sizeof(a)) != 0) {
            printf("  [iscsi] bind 127.0.0.1:%u failed (%d)\n", port, WSAGetLastError());
            return false;
        }
        if (::listen(listenSock, 4) != 0) return false;
        printf("  [iscsi] listening on %s:%u, IQN %s, %u-byte chunks\n",
               bindAddr.c_str(), port, targetIqn.c_str(), maxChunk);
        return true;
    }

    void stop() {
        if (conn != INVALID_SOCKET) { closesocket(conn); conn = INVALID_SOCKET; }
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
    }

    // ONE CONNECTION PER THREAD, and this is not an optimisation - the serial
    // version was a deadlock, measured: Windows opened its discovery connection
    // while the previous session was still open, the server never accepted it (it
    // was parked in the first session's recv), and Windows - which was waiting for
    // the discovery answer before completing the logon - timed out.  The symptom on
    // the wire was "the initiator asks for a Normal session, we answer, nothing
    // follows", because the PDU the initiator was really blocked on was sitting
    // unread in the listen backlog.  One stalled session must never wedge the
    // target; every connection gets its own object and its own thread.
    IscsiTarget* cloneForConnection(SOCKET s, const char* who) {
        IscsiTarget* t = new IscsiTarget(*this);
        // The listener is shared and owned by the parent: the clone must not close
        // it when it dies.
        t->listenSock = INVALID_SOCKET;
        t->conn = s;
        t->peer = who;
        return t;
    }

    // The bytes behind every session are ONE NVMe-oF queue pair.  Serialising
    // commands is not laziness - a queue pair has a single command queue, and two
    // iSCSI sessions interleaving submissions on it would corrupt each other's
    // completion matching.  The lock is shared by all clones, so the backend stays
    // strict while the protocol handling runs concurrently.
    static std::mutex& backendMutex() { static std::mutex m; return m; }

    int run(IscsiBackend& be, std::vector<uint8_t>&) {
        for (;;) {
            sockaddr_in from = {};
            int fl = sizeof(from);
            SOCKET s = accept(listenSock, (sockaddr*)&from, &fl);
            if (s == INVALID_SOCKET) return 0;
            // TCP_NODELAY, and this one line is worth ~250x on the data path.
            //
            // Every PDU goes out as two writes: the 48-byte BHS, then the payload.
            // With Nagle enabled the payload goes first and the *next* PDU's 48-byte
            // header - a partial segment - is then held back until the previous data
            // is acknowledged, which on Windows costs the delayed-ACK timer (~40 ms).
            // Measured before the fix, streaming sequential reads through this bridge:
            //
            //    1 MiB blocks   2.6 commands/s   387 ms/command   2.6 MB/s
            //    256 KiB       10.4              96 ms
            //    64 KiB        19.6              51 ms
            //    8 KiB         21.8              46 ms   <- ~45 ms floor per command
            //
            // The floor is the delayed ACK.  The extra ~21 ms per 64 KiB chunk in the
            // 1 MiB case is the same stall once per Data-In PDU, sixteen of them:
            // 45 + 16*21 = 387 ms.  The transfer looked bandwidth-limited and was
            // latency-limited instead - 2.6 MB/s on a stack that does 1.2 GB/s.
            //
            // iSCSI is a request/response protocol built on small headers and assumes
            // interactive traffic; every real implementation disables Nagle.  None of
            // the small-command tests noticed, because one command per second is
            // exactly what Nagle is designed not to hurt.  A streaming test found it
            // in the first sixty seconds.
            BOOL nodelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            char who[64];
            sprintf_s(who, "%s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            printf("\n  [iscsi] session from %s\n", who);
            IscsiTarget* t = cloneForConnection(s, who);
            std::thread([t, &be]() {
                std::vector<uint8_t> scratch;      // per connection, never shared
                bool ok = t->session(be, scratch);
                printf("  [iscsi] session ended (%s) %s: %ld command(s), %llu bytes in, "
                       "%llu bytes out\n", t->peer.c_str(), ok ? "logout" : "connection closed", t->cmds,
                       (unsigned long long)t->bytesIn, (unsigned long long)t->bytesOut);
                t->stop();
                delete t;
            }).detach();
        }
    }

private:
    static const uint32_t kCmdWindow = 64;

    SOCKET listenSock, conn;
    std::string targetIqn;
    std::string peer = "?";          // "ip:port" of the initiator on this connection
    std::string bindAddr = "127.0.0.1";
    uint32_t maxChunk = 65536;
    uint16_t listenPort = 3260;      // advertised in SendTargets; where Windows reconnects
    uint32_t statSn, expCmdSn, maxCmdSn, nextTtt, nextTsih;
    uint32_t curEdtl = 0;            // ExpectedDataTransferLength of the command in flight
    uint32_t curDataOut = 0;         // bytes of Data-In already sent for it
    std::chrono::steady_clock::time_point tCmdHr;   // high-resolution command start
    bool loggedIn, discovery;
    long cmds;
    unsigned long long bytesIn, bytesOut;

    // ---- TCP plumbing ----
    bool recvAll(void* buf, size_t n) {
        uint8_t* p = (uint8_t*)buf;
        while (n) {
            int got = recv(conn, (char*)p, (int)(n > (1u << 20) ? (1u << 20) : n), 0);
            if (got <= 0) return false;
            p += got; n -= (size_t)got;
        }
        return true;
    }
    bool sendAll(const void* buf, size_t n) {
        const uint8_t* p = (const uint8_t*)buf;
        while (n) {
            int sent = send(conn, (const char*)p, (int)(n > (1u << 20) ? (1u << 20) : n), 0);
            if (sent <= 0) return false;
            p += sent; n -= (size_t)sent;
        }
        return true;
    }
    bool readPdu(IscsiBhs& bhs, std::vector<uint8_t>& data) {
        if (!recvAll(bhs.raw, 48)) return false;
        uint32_t dl = bhs.dataLen(), ahs = (uint32_t)bhs.ahsLen() * 4;
        data.clear();
        if (ahs) { std::vector<uint8_t> skip(ahs); if (!recvAll(skip.data(), ahs)) return false; }
        // 1 MiB is the largest thing this bridge ever needs (a 64 KiB chunk plus
        // iSCSI text); anything larger is a client we do not understand, and
        // allocating it would be the bug rather than the fix.
        if (dl > (1u << 20)) {
            printf("  [iscsi] refusing a %u-byte data segment\n", dl);
            return false;
        }
        data.resize(dl);
        if (dl && !recvAll(data.data(), dl)) return false;
        // TRAILING PADDING, and this single line was the whole normal-session
        // failure.  An iSCSI PDU's data segment is padded to a 4-byte boundary and
        // DataSegmentLength does NOT include that padding (RFC 7143 section 11.7),
        // so 1-3 bytes stay in the stream unless they are consumed.  Left behind,
        // they shift every following PDU by that many bytes.  Discovery hid it
        // completely: the discovery login's text is 88 bytes, already a multiple of
        // four, so there was nothing to skip.  The NORMAL login's text is 127 bytes,
        // so one padding byte remained and the initiator's next Login Request was
        // read one byte out of phase - its opcode 0x43 fell into our flags field,
        // we decoded opcode 0x00, answered a NOP-In, and the initiator sat waiting
        // for a Login Response until its 30 s login timer expired.  Read the
        // reference and it is obvious; read only what the target SENDS and it is
        // invisible, which is why the receive trace above is permanent.
        uint32_t pad = (4 - (dl & 3)) & 3;
        if (pad) {
            std::vector<uint8_t> skip(pad);
            if (!recvAll(skip.data(), pad)) return false;
        }
        return true;
    }

    // ---- PDU senders ----
    // A hexdump of the login exchange.  Small, and it is the only way to see a
    // rejected handshake: the initiator answers a bad Login Response by closing the
    // connection, with no error message anywhere.
    static void dumpPdu(const char* dir, const uint8_t* p, uint32_t len) {
        if (!g_iscsiTrace) return;
        // Wall-clock on every login PDU: the difference between "the initiator
        // rejected our answer" (immediate retry) and "the initiator never saw our
        // answer" (a retry after its 15 s login timeout) is the whole diagnosis, and
        // the bytes alone cannot show it.
        printf("    [iscsi] %s  t=%llums", dir, (unsigned long long)GetTickCount64() % 1000000);
        for (uint32_t i = 0; i < len && i < 64; i++) {
            if (i % 16 == 0) printf("\n      %3u:", i);
            printf(" %02X", p[i]);
        }
        printf("\n");
    }

    bool sendLoginRsp(const IscsiBhs& req, uint8_t cls, uint8_t detail, const IscsiText::TextSegment& keys) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_LOGIN_RSP;
        // T / CSG / NSG are mirrored from the request: the target accepts whatever
        // stage transition the initiator asked for.  This is legal (a target may
        // accept any legal progression) and it removes a whole class of guesswork
        // about which stage Windows expects to land in.
        p[1] = req.raw[1] & 0x8F;
        p[2] = 0x00;                        // Version-max: 0 == version 1
        p[3] = 0x00;                        // Version-active
        iscsi_wr24(p + 5, keys.length);
        memcpy(p + 8, req.raw + 8, 6);      // ISID echoed
        // TSIH: mirror LIO, the reference target Windows accepts.  On the wire it
        // answers the FIRST Login Response with TSIH **0**, and only the operational
        // one with the handle it assigned (captured with linux/lio_proxy.py: TSIH 0,
        // then 1).  Returning a non-zero TSIH to the first login makes the response
        // look like the target believes this is a continuation of an existing
        // session, and Windows stops right there - which is precisely the stall this
        // bridge had.  An earlier version of this line returned nextTsih++ for every
        // response, on the strength of one RFC sentence read out of context; the
        // reference disagreed and the capture settled it.
        iscsi_wr16(p + 14, (req.loginCsg() == ISCSI_STAGE_SECURITY) ? 0 : nextTsih);
        iscsi_wr32(p + 16, req.itt());
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        p[36] = cls;
        p[37] = detail;
        if (!sendAll(p, 48)) return false;
        if (!keys.wire.empty() && !sendAll(keys.wire.data(), keys.wire.size())) return false;
        dumpPdu("login RSP:", p, 48);
        if (g_iscsiTrace) printf("  [iscsi] login rsp: T=%d CSG=%u NSG=%u class=%u detail=%u, %u byte(s) of keys\n",
               (p[1] & 0x80) ? 1 : 0, (p[1] >> 2) & 3, p[1] & 3, cls, detail,
               (unsigned)keys.wire.size());
        return true;
    }

    bool sendTextRsp(const IscsiBhs& req, bool final, const IscsiText::TextSegment& keys) {
        uint8_t p[48] = {};
        (void)0;
        p[0] = ISCSI_OP_TEXT_RSP;
        if (final) p[1] |= 0x80;            // F
        // Same rule as the Login Response: the length excludes the padding.  For a
        // SendTargets reply this matters just as much - the reply is a list of
        // "TargetName=...,TargetAddress=..." entries, and trailing zeros read as
        // extra entries would make the initiator reject the discovery.
        iscsi_wr24(p + 5, keys.length);
        iscsi_wr32(p + 16, req.itt());
        iscsi_wr32(p + 20, 0xFFFFFFFFu);    // TTT: unsolicited
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        if (!sendAll(p, 48)) return false;
        if (!keys.wire.empty() && !sendAll(keys.wire.data(), keys.wire.size())) return false;
        return true;
    }

    bool sendScsiRsp(uint32_t itt, uint8_t status, const uint8_t* sense, uint32_t senseLen,
                     uint32_t residual, bool underflow) {
        // Underflow is derived, not passed in, because every data-returning handler
        // has the same shape (send what you have, then complete GOOD) and every one
        // of them used to claim residual 0.  A target that returns 4 of the 192 bytes
        // the initiator asked for and then reports "no residual" is telling it two
        // contradictory things; Windows answers that by dropping the connection.
        if (status == SCSI_STATUS_GOOD && !underflow && residual == 0 &&
            curDataOut < curEdtl) {
            residual = curEdtl - curDataOut;
            underflow = true;
        }
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_SCSI_RSP;
        p[1] = 0x80;                        // F
        if (underflow) p[1] |= 0x40;        // U
        p[2] = 0x00;                        // Response: command completed at target
        p[3] = status;                      // <- byte 3, not byte 2 (see the header note)
        // One line per answer, for the same reason the receive trace exists: a
        // command the target answered wrongly and a command it never answered look
        // identical from the initiator's side (it closes the session either way).
        if (g_iscsiTrace) printf("  [iscsi] -> itt 0x%08X status 0x%02X residual %u sense %u t=%llums (in %llu ms)\n",
               itt, status, residual, senseLen,
               (unsigned long long)GetTickCount64() % 100000000,
               (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tCmdHr).count() / 1000);
        uint32_t dl = senseLen ? senseLen + 2 : 0;
        iscsi_wr24(p + 5, dl);
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 20, 0xFFFFFFFFu);
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        iscsi_wr32(p + 36, 0);              // ExpDataSN
        iscsi_wr32(p + 44, residual);
        if (!sendAll(p, 48)) return false;
        if (senseLen) {
            // SenseLength (2) + sense (18) = 20 bytes, already 4-aligned; padding is
            // added anyway because a caller passing a different length must not be
            // able to desynchronise the stream.
            uint8_t sl[2] = { (uint8_t)(senseLen >> 8), (uint8_t)senseLen };
            if (!sendAll(sl, 2)) return false;
            if (!sendAll(sense, senseLen)) return false;
            uint32_t pad = (4 - ((senseLen + 2) % 4)) % 4;
            if (pad) { uint8_t z[3] = {}; if (!sendAll(z, pad)) return false; }
        }
        return true;
    }

    bool sendDataIn(uint32_t itt, const uint8_t* data, uint32_t len, uint32_t offset,
                    uint32_t dataSn, bool final) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_DATA_IN;
        if (final) p[1] |= 0x80;            // F
        iscsi_wr24(p + 5, len);
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 20, 0xFFFFFFFFu);    // TTT: unsolicited data-in
        iscsi_wr32(p + 24, statSn);         // StatSN: not incremented by data PDUs
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        iscsi_wr32(p + 36, dataSn);
        iscsi_wr32(p + 40, offset);
        if (!sendAll(p, 48)) return false;
        if (len && !sendAll(data, len)) return false;
        // Pad the data segment to a 4-byte boundary, exactly the rule that broke the
        // RECEIVE path (see readPdu).  It matters on this side just as much: an
        // INQUIRY answer whose VPD list is 7 bytes long, with no padding, puts the
        // following SCSI Response one byte out of phase, and the initiator reads
        // garbage and drops the session - which is what Windows did, once per
        // enumeration attempt, while the target's log showed a perfectly "GOOD"
        // answer to every command.
        uint32_t pad = (4 - (len & 3)) & 3;
        if (pad) {
            uint8_t zero[3] = {};
            if (!sendAll(zero, pad)) return false;
        }
        bytesOut += len;
        curDataOut += len;              // for the automatic residual in sendScsiRsp
        return true;
    }

    bool sendR2T(uint32_t itt, uint32_t ttt, uint32_t r2tSn, uint32_t offset, uint32_t len) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_R2T;
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 20, ttt);
        // StatSN is NOT incremented here, and that was tested rather than assumed.
        //
        // The R2T this target sends is rejected by Windows as "an invalid iSCSI PDU"
        // (iScsiPrt event 23, with the header dumped), and the dump and this code
        // were compared byte for byte: opcode 0x31, flags 0, DataSegmentLength 0,
        // LUN 0, ITT echoed, TTT 1, StatSN, ExpCmdSN, MaxCmdSN, R2TSN 0, Buffer
        // Offset 0, Desired Data Transfer Length 65536 - every field of RFC 7143
        // section 11.9 in the right place with a sane value.  Layout is therefore
        // not the problem.
        //
        // The first hypothesis was the sequence number: Windows accepted every PDU
        // from this target that had consumed a StatSN and rejected the one that had
        // not, so the R2T was changed to statSn++.  Windows rejected it exactly the
        // same way, so that hypothesis is dead and the RFC reading (an R2T carries
        // the current StatSN) is what this code does.
        iscsi_wr32(p + 24, statSn);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        iscsi_wr32(p + 36, r2tSn);
        iscsi_wr32(p + 40, offset);
        iscsi_wr32(p + 44, len);
        // Traced because a write that stalls is otherwise invisible: the command
        // arrives, no response follows, and nothing in the log says whether an R2T
        // ever went out.  A 64 KiB WRITE(10) the initiator never answers looks
        // exactly like a target that never asked.
        bool ok = sendAll(p, 48);
        if (g_iscsiTrace) printf("  [iscsi] R2T itt=0x%08X ttt=%u sn=%u offset=%u len=%u -> %s\n",
               itt, ttt, r2tSn, offset, len, ok ? "sent" : "SEND FAILED");
        // The bytes, next to the initiator's own dump of what it rejected: the
        // Windows event carries "the entire iSCSI header", so the two can be laid
        // side by side instead of argued about.
        dumpPdu("R2T sent:", p, 48);
        return ok;
    }

    bool sendNopIn(uint32_t itt, uint32_t ttt) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_NOP_IN;
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 20, ttt);
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        return sendAll(p, 48);
    }

    bool sendTaskMgmtRsp(uint32_t itt, uint8_t response) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_TASK_MGMT_RSP;
        p[2] = response;
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        return sendAll(p, 48);
    }

    bool sendLogoutRsp(uint32_t itt, uint8_t response) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_LOGOUT_RSP;
        p[2] = response;
        iscsi_wr32(p + 16, itt);
        iscsi_wr32(p + 24, statSn++);
        iscsi_wr32(p + 28, expCmdSn);
        iscsi_wr32(p + 32, maxCmdSn);
        return sendAll(p, 48);
    }

    bool sendReject(const IscsiBhs& req, uint8_t reason) {
        uint8_t p[48] = {};
        p[0] = ISCSI_OP_REJECT;
        p[2] = reason;
        iscsi_wr24(p + 5, 48);
        iscsi_wr32(p + 16, 0xFFFFFFFFu);
        if (!sendAll(p, 48)) return false;
        return sendAll(req.raw, 48);
    }

    // ---- sense builders ----
    static uint32_t buildSense(uint8_t* s, uint8_t key, uint8_t asc, uint8_t ascq) {
        memset(s, 0, 18);
        s[0] = 0x70;            // fixed format, current error
        s[2] = key;
        s[7] = 10;              // additional sense length
        s[12] = asc;
        s[13] = ascq;
        return 18;
    }

    // ---- the session ----
    bool session(IscsiBackend& be, std::vector<uint8_t>& scratch);
    bool handleScsi(IscsiBackend& be, const IscsiBhs& bhs, std::vector<uint8_t>& scratch);
    bool sendInquiry(const IscsiBhs& bhs, IscsiBackend& be, IscsiBackend* be2);
};

// ---------------------------------------------------------------------------
//  SCSI command handling.  Kept as one function so the command set is visible in
//  one place: a reader can see exactly what this bridge claims to support.
// ---------------------------------------------------------------------------
inline bool IscsiTarget::session(IscsiBackend& be, std::vector<uint8_t>& scratch) {
    for (;;) {
        // keep the NVMe side's keep-alive fed - under the backend lock, because a
        // keep-alive and a READ from another session cannot share the queue pair
        {
            std::lock_guard<std::mutex> lock(backendMutex());
            be.tick();
        }
        IscsiBhs bhs;
        std::vector<uint8_t> data;
        if (!readPdu(bhs, data)) return false;

        // Every PDU, as it arrives, with the connection that carried it.  Without
        // this the log shows what the target SAID and never what it HEARD, and the
        // two ways a login can die - "the initiator sent nothing more" and "we never
        // read what it sent" - look identical.  That ambiguity cost a full debugging
        // round here.
        if (g_iscsiTrace) printf("    [iscsi] <- %s opcode 0x%02X flags 0x%02X datalen %u\n", peer.c_str(),
               bhs.raw[0], bhs.raw[1], (unsigned)bhs.dataLen());

        switch (bhs.opcode()) {
        case ISCSI_OP_LOGIN_REQ: {
            dumpPdu("login REQ:", bhs.raw, 48);
            IscsiText t;
            t.parse(data.data(), (uint32_t)data.size());
            // Log what the initiator OFFERS.  An iSCSI responder may only answer keys
            // that the originator sent (RFC 7143 section 6.2), so this list - not our
            // wish list - decides what may legally go into the response.  Answering a
            // key the initiator never offered makes it reject the whole negotiation
            // and re-send the same Login Request, which is exactly what it did.
            if (!t.kv.empty()) {
                if (g_iscsiTrace) printf("      offered:");
                for (auto& kv : t.kv) printf(" %s=%s;", kv.first.c_str(), kv.second.c_str());
                printf("\n");
            }
            if (t.get("SessionType") == "Discovery") discovery = true;
            IscsiText::TextSegment keys;
            if (bhs.loginCsg() == ISCSI_STAGE_SECURITY) {
                // Mirrors what LIO sends, because LIO is a target Windows accepts
                // (captured in the proxy log; see linux/lio_proxy.py):
                //
                //   AuthMethod=None \0
                //   TargetAlias=LIO Target \0
                //   TargetPortalGroupTag=1 \0
                //
                // TargetPortalGroupTag is the one that matters.  It is a TARGET-ONLY
                // key, so it is sent unasked - the "answer only what was offered" rule
                // governs responder keys, not this class - and it is how the initiator
                // identifies the portal group it landed on.  Without it, Windows
                // answers the security stage of a NORMAL session and then waits until
                // its login timer expires: precisely the stall this bridge had, while
                // the DISCOVERY session (which needs no portal group) went through.
                std::string sec = "AuthMethod=None";
                sec.push_back('\0');
                sec += "TargetAlias=NVMe-oF bridge";
                sec.push_back('\0');
                sec += "TargetPortalGroupTag=1";
                keys = IscsiText::makeText(sec);
            } else {
                // ANSWER ONLY WHAT WAS OFFERED.  RFC 7143 section 6.2: a responder
                // that sends a key the originator did not offer gets it back as
                // "Irrelevant" and the negotiation fails.  The first version of this
                // code sent its whole wish list (14 keys) to a discovery login that
                // offered 5, and Windows answered exactly that - seven "Irrelevant"
                // plus "=NotUnderstood" - then re-sent the same Login Request.
                // The measured offer from Windows here is:
                //   HeaderDigest=None,CRC32C; DataDigest=None,CRC32C;
                //   MaxRecvDataSegmentLength=65536; DefaultTime2Wait=0;
                //   DefaultTime2Retain=60
                IscsiText p;
                std::string recvLen;
                {
                    char v[32];
                    sprintf_s(v, "%u", maxChunk);
                    recvLen = v;
                }
                auto offer = [&](const char* k, const std::string& v) {
                    for (auto& kv : t.kv) if (kv.first == k) { p.kv.push_back({ k, v }); return; }
                };
                // ANSWER THE OPERATIONAL KEYS, and this switch was false for a reason
                // worth recording: it started life as a bisect ("answer nothing at all
                // - a key the responder leaves alone keeps the initiator's offered
                // value, which is a legal and complete negotiation"), used to separate
                // "our values are wrong" from "the response itself is wrong" while the
                // real defect turned out to be the missing 4-byte padding on receive.
                //
                // Leaving it off is legal but WRONG for this bridge, because of what
                // Windows actually offers on a normal session (measured, DESIGN 8.55):
                //
                //   InitialR2T=No  ImmediateData=Yes  ErrorRecoveryLevel=2
                //   MaxConnections=32  MaxOutstandingR2T=16
                //
                // Every one of those is a capability this target does NOT have.  By
                // staying silent we were accepting the initiator's values: unsolicited
                // write data (which handleScsi would have to parse out of the SCSI
                // Command PDU), 32 concurrent connections, ERL 2 recovery, and 16
                // outstanding R2Ts.  Nothing broke only because WRITE is refused
                // outright by the read-only rail - the bug was hidden by the safety
                // feature.  A responder may only lower these values, never raise them,
                // so answering 0/Yes/No/1 is the legal way to say what we really are.
                //
                // Only offered keys are answered (the rule that cost a whole round
                // earlier), and DefaultTime2Wait / DefaultTime2Retain stay unanswered on
                // purpose: they are the initiator's own reinstatement timers, its
                // offered values are what we want, and answering them produced the
                // "=NotUnderstood" entries seen in that earlier round.
                offer("HeaderDigest", "None");            // we do not do CRC32C
                offer("DataDigest", "None");
                // MaxConnections: Windows offers 32 and this bridge answers 4 rather than 1.
                // MEASURED NEGATIVE RESULT: it does not make Windows open more than one
                // connection.  With this answered as 4, `iscsicli SessionList` still reports
                // "Number Connections: 1" and throughput is unchanged within run-to-run
                // noise (79 vs 76 MB/s at depth 1, ~230 MB/s both at depth 8).  A responder
                // may only lower this value and 4 <= the initiator's 32, so it stays at 4:
                // it costs nothing and it lets a manually configured multi-connection
                // session work, but the extra connections have to be asked for explicitly.
                // Parallelism here comes from the initiator's own queue depth, per the
                // concurrency numbers recorded with the READ path below.
                offer("MaxConnections", "4");
                offer("InitialR2T", "Yes");               // every write is R2T-driven
                offer("ImmediateData", "No");             // ... which requires this
                offer("MaxRecvDataSegmentLength", recvLen);
                // FirstBurstLength is not answered.  With InitialR2T=Yes it only sizes
                // the unsolicited burst, which by definition does not happen, and RFC
                // 7143 s12.14 makes an unanswered key keep the initiator's own value -
                // which is what we want.  (It was dropped while hunting the R2T
                // rejection; that hunt failed for other reasons, but leaving it out is
                // still the more defensible negotiation.)
                offer("MaxBurstLength", "262144");
                offer("MaxOutstandingR2T", "1");
                offer("DataPDUInOrder", "Yes");
                offer("DataSequenceInOrder", "Yes");
                offer("ErrorRecoveryLevel", "0");         // no connection reinstatement
                // These two are the initiator's own timeouts.  NOT answered, and
                // that is deliberate: a key the responder does not answer keeps the
                // initiator's offered value, which is what we want anyway - and
                // answering them is what produced Windows' two "=NotUnderstood"
                // entries (the initiator has no use for a target's opinion about how
                // long IT should wait before reinstating a connection).
                // offer("DefaultTime2Wait", t.get("DefaultTime2Wait", "0"));
                // offer("DefaultTime2Retain", t.get("DefaultTime2Retain", "0"));
                keys = IscsiText::makeText(p.build());
                {
                    // The answer, verbatim: MaxRecvDataSegmentLength here is what the initiator
                    // may put in ONE Data-Out PDU, so it is also the largest R2T this target may
                    // ask for.  Printing it turns 'the R2T was rejected' into a value to check.
                    std::string shown;
                    for (auto& kv : p.kv) { shown += kv.first; shown += '='; shown += kv.second; shown += ' '; }
                    if (g_iscsiTrace) printf("  [iscsi] answering:%s\n", shown.c_str());
                }
            }
            // The initiator's MaxRecvDataSegmentLength is what WE may put in one
            // Data-In PDU: the key declares what the sender can receive, so their
            // value caps our reads and ours caps their writes.
            std::string peer = t.get("MaxRecvDataSegmentLength");
            if (!peer.empty()) {
                uint32_t m = (uint32_t)atoi(peer.c_str());
                if (m >= 512 && m < maxChunk) maxChunk = m;
            }
            if (!sendLoginRsp(bhs, 0x00, 0x00, keys)) return false;
            loggedIn = true;
            if (bhs.loginT() && bhs.loginNsg() == ISCSI_STAGE_FULL_FEATURE) {
                printf("  [iscsi] full feature phase (session is %s)\n",
                       discovery ? "a DISCOVERY session" : "a normal session");
            }
            break;
        }
        case ISCSI_OP_TEXT_REQ: {
            IscsiText t;
            t.parse(data.data(), (uint32_t)data.size());
            IscsiText::TextSegment keys;
            if (t.get("SendTargets") == "All" || t.kv.empty()) {
                // The reference's exact reply (LIO, captured on the wire):
                //
                //   TargetName=iqn.2024-01.local.rdma:linuxtarget \0
                //   TargetAddress=127.0.0.1:3260,1 \0
                //
                // TWO NUL-separated keys, and the address carries ",<TPGT>".  Both
                // halves were paid for here: the comma-joined one-line form made
                // Windows answer "The iSCSI name specified contains invalid
                // characters or is too long" (it takes everything after "TargetName="
                // up to the NUL as the name), and the two-key form WITHOUT the ",1"
                // suffix made it answer "Invalid SendTargets response text".
                //
                // The address must be the one the initiator can reach: it is where
                // Windows connects for the NORMAL session, so getting it wrong sends
                // the disk to somebody else's machine - which is exactly what happened
                // while this was being worked out against the reference target.
                std::string st = "TargetName=" + targetIqn;
                st.push_back('\0');
                st += "TargetAddress=" + bindAddr + ":" + std::to_string(listenPort) + ",1";
                keys = IscsiText::makeText(st);
                printf("  [iscsi] SendTargets -> %s @ %s:%u,1\n", targetIqn.c_str(),
                       bindAddr.c_str(), (unsigned)listenPort);
            }
            if (!sendTextRsp(bhs, true, keys)) return false;
            break;
        }
        case ISCSI_OP_SCSI_CMD: {
            if (!loggedIn) { if (!sendReject(bhs, 0x02)) return false; break; }
            std::lock_guard<std::mutex> lock(backendMutex());
            if (!handleScsi(be, bhs, scratch)) return false;
            break;
        }
        case ISCSI_OP_NOP_OUT:
            if (!sendNopIn(bhs.itt(), bhs.ttt())) return false;
            break;
        case ISCSI_OP_TASK_MGMT_REQ:
            // Every function is answered "complete": with ErrorRecoveryLevel=0 and a
            // loopback link there is no state to unwind, and a Windows initiator that
            // gets "not supported" here retries the reset instead of moving on.
            if (!sendTaskMgmtRsp(bhs.itt(), 0x00)) return false;
            break;
        case ISCSI_OP_LOGOUT_REQ:
            if (!sendLogoutRsp(bhs.itt(), 0x00)) return false;
            return true;
        case ISCSI_OP_SNACK_REQ:
            break;                            // nothing to retransmit: we never lose PDUs
        case ISCSI_OP_DATA_OUT:
            // Only legal in answer to an R2T, and handleScsi consumes those inline.
            printf("  [iscsi] unexpected Data-Out outside an R2T\n");
            if (!sendReject(bhs, 0x04)) return false;
            break;
        default:
            printf("  [iscsi] unsupported opcode 0x%02X -> reject\n", bhs.opcode());
            if (!sendReject(bhs, 0x01)) return false;   // 0x01 = command not supported
            break;
        }
    }
}

inline bool IscsiTarget::sendInquiry(const IscsiBhs& bhs, IscsiBackend& be, IscsiBackend*) {
    const uint8_t* cdb = bhs.cdb();
    bool evpd = (cdb[1] & 0x01) != 0;
    uint8_t page = cdb[2];
    uint8_t out[256];
    uint32_t n = 0;
    memset(out, 0, sizeof(out));

    if (!evpd) {
        out[0] = 0x00;                       // direct access block device
        out[1] = 0x00;                       // not removable
        out[2] = 0x06;                       // SPC-4
        out[3] = 0x02;                       // response data format
        out[4] = 31;                         // additional length
        out[6] = 0x00;
        out[7] = 0x02;                       // CMDQUE
        memcpy(out + 8, "NVMEOF  ", 8);      // vendor
        // The product string carries where the disk really lives: it shows up in
        // Windows' Device Manager, and "whose disk is this" is the first question a
        // bridge like this raises.
        memcpy(out + 16, "iSCSI-NVMeoF    ", 16);
        memcpy(out + 32, "1.0 ", 4);
        n = 36;
    } else if (page == 0x00) {
        out[0] = 0x00; out[1] = 0x00; out[2] = 0x00; out[3] = 3; out[4] = 0x00;
        out[5] = 0x80; out[6] = 0x83;
        n = 7;
    } else if (page == 0x80) {
        const char* sn = be.serial();
        size_t l = strlen(sn); if (l > 32) l = 32;
        out[0] = 0x00; out[1] = 0x80; out[3] = (uint8_t)l;
        memcpy(out + 4, sn, l);
        n = 4 + (uint32_t)l;
    } else if (page == 0x83) {
        const char* sn = be.serial();
        size_t l = strlen(sn); if (l > 32) l = 32;
        // Designator type 2 = EUI-64, code set 1 = binary.  Windows keys a disk by
        // this, so it is derived from the real serial rather than invented.
        uint64_t eui = 0;
        for (size_t i = 0; i < l && i < 8; i++) eui = (eui << 8) | (uint8_t)sn[i];
        out[0] = 0x00; out[1] = 0x83; out[3] = 12;
        out[4] = 0x02; out[5] = 0x01; out[7] = 8;
        iscsi_wr64(out + 8, eui);
        n = 20;
    } else {
        uint8_t s[18];
        uint32_t sl = buildSense(s, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD, 0);
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
    }

    uint32_t alloc = iscsi_rd16(cdb + 3);
    // Answer with the FULL allocation length, zero-filled, instead of a short
    // transfer plus a residual count.  INQUIRY is the one command Windows insists on
    // before it will admit a disk exists, and it asked for the supported-VPD-page
    // list (page 0) with an allocation length of 255.  Sending 7 bytes and a residual
    // of 248 made it drop the connection and retry, once per attempt, while the log
    // showed a perfectly GOOD answer to every command - the earlier commands in the
    // same session (which returned exactly what was asked for, residual 0) went
    // through fine, and that difference is what identified this.  Real devices pad
    // these small descriptors; the page's own length field still says how much of the
    // buffer means anything.
    uint32_t send = (alloc <= sizeof(out)) ? alloc : (uint32_t)sizeof(out);
    if (!sendDataIn(bhs.itt(), out, send, 0, 0, true)) return false;
    return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0,
                       n > send ? n - send : 0, n > send);
}

inline bool IscsiTarget::handleScsi(IscsiBackend& be, const IscsiBhs& bhs,
                                    std::vector<uint8_t>& scratch) {
    const uint8_t* cdb = bhs.cdb();
    const uint8_t op = cdb[0];
    const uint32_t bs = be.blockSize();
    const uint64_t nblocks = be.blocks();
    const uint32_t edtl = bhs.edtl();
    cmds++;
    expCmdSn = bhs.cmdSn() + 1;              // this command is acknowledged
    curEdtl = edtl;                          // for the automatic residual below
    curDataOut = 0;
    // OPEN THE COMMAND WINDOW.  RFC 7143 section 6.3.1: an initiator MUST NOT send
    // a command whose CmdSN is greater than MaxCmdSN, and MaxCmdSN starts at 1
    // (LIO sends exactly that during login, and so do we).  Leaving it there means
    // the initiator gets ONE command through and then has no legal CmdSN left - the
    // session stays open, nothing errors, and the disk never appears.  That is
    // precisely the shape of the failure this bridge showed after the padding fix:
    // READ CAPACITY(10) answered GOOD, then silence forever.
    maxCmdSn = expCmdSn + kCmdWindow - 1;
    be.tick();
    if (g_iscsiTrace) printf("    [iscsi] <- SCSI CDB %02X %02X %02X %02X %02X %02X  lun=%u itt=0x%08X "
           "edtl=%u cmdsn=%u t=%llums\n", cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5],
           (unsigned)bhs.lun(), bhs.itt(), edtl, bhs.cmdSn(),
           (unsigned long long)GetTickCount64() % 100000000);
    tCmdHr = std::chrono::steady_clock::now();

    switch (op) {
    case 0x12:                               // INQUIRY
        return sendInquiry(bhs, be, &be);

    case 0x00:                               // TEST UNIT READY
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);

    case 0x03: {                             // REQUEST SENSE
        uint8_t s[18] = {};
        s[0] = 0x70; s[7] = 10;
        uint32_t alloc = cdb[4];
        uint32_t send = (alloc < 18) ? alloc : 18;
        if (!sendDataIn(bhs.itt(), s, send, 0, 0, true)) return false;
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x1A:                               // MODE SENSE(6)
    case 0x5A: {                             // MODE SENSE(10)
        // A 4-byte (or 8-byte) header with no pages.  Windows asks for page 0x08
        // (caching) and 0x3F (all); answering with just the header is legal and
        // means "no mode pages", which is exactly true of a pass-through bridge.
        uint8_t out[256] = {};
        uint32_t n;
        if (op == 0x1A) {
            bool dbd = (cdb[1] & 0x08) != 0;
            uint32_t bdLen = dbd ? 0 : 0;
            out[0] = (uint8_t)(3 + bdLen);
            out[1] = 0x00;                   // medium type
            // Device-specific parameter bit 7 = WP (write protected).  Reporting it
            // here is what makes Windows treat the whole disk as read-only instead
            // of failing every write later with an I/O error.
            out[2] = be.writable() ? 0x00 : 0x80;
            out[3] = (uint8_t)bdLen;
            n = 4 + bdLen;
        } else {
            out[0] = 0; out[1] = 0;
            out[2] = be.writable() ? 0x00 : 0x80;
            out[3] = 0;
            iscsi_wr16(out + 6, 0);          // block descriptor length
            n = 8;
        }
        uint32_t alloc = (op == 0x1A) ? cdb[4] : iscsi_rd16(cdb + 7);
        // Same rule as INQUIRY: hand back the whole allocation length, zero-filled.
        uint32_t send = (alloc <= sizeof(out)) ? alloc : (uint32_t)sizeof(out);
        if (!sendDataIn(bhs.itt(), out, send, 0, 0, true)) return false;
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x25: {                             // READ CAPACITY(10)
        uint8_t out[8];
        uint64_t last = nblocks - 1;
        iscsi_wr32(out, (uint32_t)last);
        iscsi_wr32(out + 4, bs);
        if (!sendDataIn(bhs.itt(), out, 8, 0, 0, true)) return false;
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x9E: {                             // READ CAPACITY(16) - SERVICE ACTION IN(16)
        if ((cdb[1] & 0x1F) != 0x10) break;
        uint8_t out[32] = {};
        iscsi_wr64(out, nblocks - 1);
        iscsi_wr32(out + 8, bs);
        if (!sendDataIn(bhs.itt(), out, 32, 0, 0, true)) return false;
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0xA0: {                             // REPORT LUNS
        uint8_t out[16] = {};
        iscsi_wr32(out, 8);                  // LUN list length: one LUN
        out[8] = 0x00; out[9] = 0x00;        // LUN 0 in flat space addressing
        uint32_t alloc = iscsi_rd32(cdb + 6);
        uint32_t send = (alloc < 16) ? alloc : 16;
        if (!sendDataIn(bhs.itt(), out, send, 0, 0, true)) return false;
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x1B:                               // START STOP UNIT
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);

    case 0x28:                               // READ(10)
    case 0x88: {                             // READ(16)
        uint64_t lba; uint32_t count;
        if (op == 0x28) { lba = iscsi_rd32(cdb + 2); count = iscsi_rd16(cdb + 7); }
        else            { lba = iscsi_rd64(cdb + 2); count = iscsi_rd32(cdb + 10); }
        uint64_t bytes = (uint64_t)count * bs;
        if (count == 0 || lba + count > nblocks || bytes > edtl) {
            uint8_t s[18];
            uint32_t sl = buildSense(s, SCSI_SENSE_ILLEGAL_REQUEST,
                                     SCSI_ASC_LBA_OUT_OF_RANGE, 0);
            return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
        }
        uint32_t offset = 0, dataSn = 0;
        // Aggregate timing for this command (see g_iscsiTime above).
        auto tCmd = std::chrono::steady_clock::now();
        long long nvmeTotalUs = 0, sendTotalUs = 0;
        uint32_t pduCount = 0;
        while (offset < bytes) {
            // ---- one NVMe read per *staging unit*, one Data-In PDU per chunk ----
            //
            // SIZING THE NVMe TRANSFER SEPARATELY FROM THE PDU IS THE WHOLE POINT.
            // The loop used to read 64 KiB and send it, then read the next 64 KiB and
            // send that: sixteen submit-and-wait round trips for a 1 MiB READ, because
            // the NVMe unit was pinned to the largest iSCSI data segment the initiator
            // will accept.
            //
            // HOW BIG A TRANSFER ARRIVES WAS MEASURED OFF THE WIRE, NOT ASSUMED.
            // Windows sizes the CDB to the request: a 64 KiB read arrives as one
            // READ(10) with edtl = 65536, and a 1 MiB read arrives as FOUR READ(10)s
            // with edtl = 262144 each (captured with -iscsitrace; the transfer length
            // is in the CDB, which is why the trace prints edtl).  So a 256 KiB
            // command stages 256 KiB in one NVMe read and emits four 64 KiB Data-In
            // PDUs, which is exactly this change, and it is worth +17% at 1 MiB
            // (102.1 -> 119.7 MB/s, DESIGN 8.57/8.59) because it removes three
            // submit-and-wait round trips per command.
            //
            // An earlier draft of this comment claimed the opposite - that Windows
            // caps every READ at 64 KiB and the decoupling never engages.  That came
            // from extrapolating per-command times instead of reading the CDB, and it
            // was wrong: the times fit both stories, the wire fits only one.
            //
            // The real constraint on this path was measured too (-iscsitime, one
            // aggregate line per command): a 64 KiB READ costs 755 us total, of which
            // NVMe staging is 115 us and the Data-In socket write is 635 us, with
            // nothing else measurable.  A 256 KiB command costs 1922 us = 218 us NVMe
            // + 1653 us for four PDUs.  So the path is LATENCY-bound per command and
            // the PDU write dominates it, which is why a single synchronous reader
            // (depth 1) sees ~79 MB/s at 64 KiB and ~119 MB/s at 1 MiB while the same
            // bridge measured 230.6 MB/s with 8 concurrent readers.  A throughput
            // number for this bridge is meaningless without its queue depth.
            uint32_t unitCap = be.maxTransfer();
            // No capacity test here: the scratch vector is resized to whatever the
            // backend wants to stage.  An earlier version consulted
            // scratch.capacity() first, which is 0 on the first command of a session,
            // so the unit stayed pinned at one Data-In PDU and the change did nothing
            // measurable - the guard defeated the feature it was guarding.
            if (unitCap == 0) unitCap = maxChunk;    // backend has no preference: old shape
            uint32_t unit = (uint32_t)((bytes - offset) < unitCap ? (bytes - offset) : unitCap);
            if (unit % bs) unit -= unit % bs;
            if (unit == 0) break;
            uint64_t curLba = lba + offset / bs;
            // The buffer must EXIST before the backend writes into it.  With one
            // connection per thread the scratch vector arrives empty, and an empty
            // vector's data() is a null pointer - which is exactly what the target
            // side logged: an SGL with addr 0x0.  The read then "failed" with a bare
            // NVMe status and no explanation anywhere on the wire.
            if (scratch.size() < unit) scratch.resize(unit);
            // Time the NVMe submission separately from everything else.  A streaming
            // test that showed a flat ~45 ms per SCSI command could be either the
            // bridge's own round trip or the initiator's turnaround, and the two have
            // completely different fixes - so measure rather than argue.
            auto tNvme = std::chrono::steady_clock::now();
            bool nvmeOk = be.read(curLba, unit / bs, scratch.data());
            long long nvmeUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tNvme).count();
            if (nvmeUs >= 1000) {
                if (g_iscsiTrace) printf("    [iscsi] NVMe READ %u blocks took %lld us\n", unit / bs, nvmeUs);
            }
            nvmeTotalUs += nvmeUs;
            if (!nvmeOk) {
                printf("  [iscsi] READ lba=%llu blocks=%u failed on the NVMe side\n",
                       (unsigned long long)curLba, unit / bs);
                uint8_t s[18];
                uint32_t sl = buildSense(s, 0x04 /*HARDWARE ERROR*/, 0x11 /*unrecovered read*/, 0);
                return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
            }
            // Hand the staged bytes out as Data-In PDUs, each no larger than what the
            // initiator said it can receive in one PDU.  The data segment must end on
            // a 4-byte boundary (RFC 7143 11.1); sendDataIn pads per PDU, and only the
            // final PDU of the transfer carries F.
            for (uint32_t sent = 0; sent < unit; sent += maxChunk) {
                uint32_t part = (uint32_t)((unit - sent) < maxChunk ? (unit - sent) : maxChunk);
                bool last = (offset + sent + part >= bytes);
                auto tSend = std::chrono::steady_clock::now();
                bool ok = sendDataIn(bhs.itt(), scratch.data() + sent, part, offset + sent,
                                     dataSn++, last);
                sendTotalUs += std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - tSend).count();
                pduCount++;
                if (!ok) {
                    return false;
                }
            }
            offset += unit;
        }
        if (g_iscsiTime) {
            long long totalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - tCmd).count();
            printf("  [iscsi] time READ %llu B: total %lld us, nvme %lld us, datain %lld us, "
                   "%u PDU(s), other %lld us\n",
                   (unsigned long long)bytes, totalUs, nvmeTotalUs, sendTotalUs, pduCount,
                   totalUs - nvmeTotalUs - sendTotalUs);
        }
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x2A:                               // WRITE(10)
    case 0x8A: {                             // WRITE(16)
        uint64_t lba; uint32_t count;
        if (op == 0x2A) { lba = iscsi_rd32(cdb + 2); count = iscsi_rd16(cdb + 7); }
        else            { lba = iscsi_rd64(cdb + 2); count = iscsi_rd32(cdb + 10); }
        uint64_t bytes = (uint64_t)count * bs;

        // READ-ONLY IS THE DEFAULT, and it is refused here rather than at the NVMe
        // layer: nvmet cannot mark a namespace read-only, so the only place this
        // promise can be kept is before the command is issued.
        if (!be.writable()) {
            printf("  [iscsi] WRITE lba=%llu blocks=%u REFUSED (read-only bridge)\n",
                   (unsigned long long)lba, count);
            uint8_t s[18];
            uint32_t sl = buildSense(s, SCSI_SENSE_DATA_PROTECT, SCSI_ASC_WRITE_PROTECTED, 0);
            return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
        }
        if (count == 0 || lba + count > nblocks || bytes > edtl) {
            uint8_t s[18];
            uint32_t sl = buildSense(s, SCSI_SENSE_ILLEGAL_REQUEST,
                                     SCSI_ASC_LBA_OUT_OF_RANGE, 0);
            return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
        }

                uint32_t offset = 0, r2tSn = 0;
        while (offset < bytes) {
            uint32_t chunk = (uint32_t)((bytes - offset) < maxChunk ? (bytes - offset) : maxChunk);
            if (chunk % bs) chunk -= chunk % bs;
            if (chunk == 0) break;
            uint32_t ttt = nextTtt++;
            if (!sendR2T(bhs.itt(), ttt, r2tSn++, offset, chunk)) return false;

            // Collect exactly `chunk` bytes of Data-Out PDUs.  MaxOutstandingR2T=1,
            // so nothing else may arrive in between; anything else IS the error.
            uint32_t got = 0, expectSn = 0;
            while (got < chunk) {
                IscsiBhs d;
                std::vector<uint8_t> payload;
                if (!readPdu(d, payload)) return false;
                if (d.opcode() != ISCSI_OP_DATA_OUT) {
                    printf("  [iscsi] expected Data-Out, got opcode 0x%02X\n", d.opcode());
                    return false;
                }
                if (d.itt() != bhs.itt() || d.dataSn() != expectSn) {
                    printf("  [iscsi] Data-Out mismatch (itt 0x%08X vs 0x%08X, DataSN %u vs %u)\n",
                           d.itt(), bhs.itt(), d.dataSn(), expectSn);
                    return false;
                }
                if (d.bufferOff() != offset + got || payload.size() > chunk - got) {
                    printf("  [iscsi] Data-Out offset %u (want %u) or %u bytes (room %u)\n",
                           d.bufferOff(), offset + got, (unsigned)payload.size(), chunk - got);
                    return false;
                }
                memcpy(scratch.data() + got, payload.data(), payload.size());
                got += (uint32_t)payload.size();
                bytesIn += payload.size();
                expectSn++;
            }
            uint64_t curLba = lba + offset / bs;
            if (!be.write(curLba, chunk / bs, scratch.data())) {
                printf("  [iscsi] WRITE lba=%llu blocks=%u failed on the NVMe side\n",
                       (unsigned long long)curLba, chunk / bs);
                uint8_t s[18];
                uint32_t sl = buildSense(s, 0x03 /*MEDIUM ERROR*/, 0x0C /*write error*/, 0);
                return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
            }
            offset += chunk;
        }
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    case 0x35:                               // SYNCHRONIZE CACHE(10)
    case 0x91: {                             // SYNCHRONIZE CACHE(16)
        if (be.writable() && !be.flush()) {
            uint8_t s[18];
            uint32_t sl = buildSense(s, 0x04, 0x11, 0);
            return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
        }
        return sendScsiRsp(bhs.itt(), SCSI_STATUS_GOOD, nullptr, 0, 0, false);
    }

    default:
        break;
    }

    printf("  [iscsi] unsupported SCSI opcode 0x%02X -> CHECK CONDITION\n", op);
    uint8_t s[18];
    uint32_t sl = buildSense(s, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_OPCODE, 0);
    return sendScsiRsp(bhs.itt(), SCSI_STATUS_CHECK_COND, s, sl, 0, false);
}

#endif  // NVMEOF_ISCSI_H












