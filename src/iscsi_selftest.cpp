// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  iSCSI/SCSI protocol self-test.  NO HARDWARE, NO INITIATOR, NO RDMA CARD.
//
//  Why this file exists: the iSCSI layer is where every regression in this project
//  has actually happened - an unpadded data segment that shifted the whole stream, a
//  residual computed from the wrong counter, an R2T that asked for offset 0 when the
//  initiator had already sent 65536 bytes, a parked write with no bound, a
//  negotiation rule applied backwards.  Every one of those was found by attaching a
//  real Windows initiator and staring at a trace, because the rules lived inside a
//  2000-line socket-bound class where nothing could be tested.
//
//  The parts that carry those rules are pure now (nvmeof_iscsi_pending.h, the
//  iscsiBuild* PDU builders, the iscsiBool*/iscsiNumberMin negotiation helpers), so
//  this suite pins them on a machine with no NIC at all - which is what lets CI run
//  it on every push.
//
//  Build:  cl /nologo /W4 /WX /std:c++17 /EHsc /I. iscsi_selftest.cpp /link ws2_32.lib
//  Run:    iscsi_selftest.exe      (prints PASS/FAIL, exits non-zero on failure)
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "nvmeof_iscsi_pending.h"
#include "nvmeof_iscsi.h"          // pulls in winsock2, but no NetworkDirect
#include "nvmeof_wire.h"           // the NVMe-oF side: capability bits and Identify offsets

static int g_failures = 0;

// Same shape as wire_selftest.c's CHECK, and the same reason for the function call:
// this file checks constant comparisons on purpose, and MSVC 14.4x answers a constant
// conditional with C4127, which /W4 /WX turns into C2220 (DESIGN 8.69).
static int check_holds(int cond) { return cond; }

#define CHECK(cond, msg) do { \
    if (!check_holds(!!(cond))) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_failures++; } \
} while (0)

#define CHECK_EQ_U32(got, want, msg) do { \
    unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
    if (g_ != w_) { printf("FAIL: %s: got %u, want %u (%s:%d)\n", msg, g_, w_, __FILE__, __LINE__); g_failures++; } \
} while (0)

// ---------------------------------------------------------------------------
//  1. The 4-byte data-segment padding rule
// ---------------------------------------------------------------------------
static void test_padding(void) {
    // RFC 7143 section 11.7: the data segment is padded to a multiple of four, and
    // DataSegmentLength does NOT include the padding.  Both directions of this target
    // have been broken by getting it wrong.
    CHECK_EQ_U32(iscsi_pad4(0), 0, "pad(0)");
    CHECK_EQ_U32(iscsi_pad4(1), 3, "pad(1)");
    CHECK_EQ_U32(iscsi_pad4(2), 2, "pad(2)");
    CHECK_EQ_U32(iscsi_pad4(3), 1, "pad(3)");
    CHECK_EQ_U32(iscsi_pad4(4), 0, "pad(4)");
    CHECK_EQ_U32(iscsi_pad4(127), 1, "pad(127) - the normal login text that broke receive");
    CHECK_EQ_U32(iscsi_pad4(7), 1, "pad(7) - the VPD list that broke send");
    CHECK_EQ_U32(iscsi_pad4(65536), 0, "pad(64 KiB)");
    // pad(len) + len must always land on a multiple of four, which is the property
    // that actually matters: every residue, not just the ones seen on the wire.
    for (uint32_t len = 0; len < 64; len++) {
        if (((len + iscsi_pad4(len)) & 3u) != 0) { printf("FAIL: pad(%u) does not align\n", len); g_failures++; }
    }
}

// ---------------------------------------------------------------------------
//  2. BHS accessors: the target's view of an incoming PDU
// ---------------------------------------------------------------------------
static void test_bhs_accessors(void) {
    uint8_t raw[48] = {};
    raw[0] = 0x01;                       // SCSI Command, immediate bit clear
    raw[1] = 0xC1;                       // F | R | task attribute 1
    iscsi_wr24(raw + 5, 65536);          // DataSegmentLength
    iscsi_wr64(raw + 8, 0);              // LUN
    iscsi_wr32(raw + 16, 0xDEADBEEF);    // ITT
    iscsi_wr32(raw + 20, 0x11223344);    // ExpectedDataTransferLength (command PDU)
    iscsi_wr32(raw + 24, 7);             // CmdSN
    raw[32] = 0x2A;                      // READ(10) opcode in the CDB
    IscsiBhs b;
    memcpy(b.raw, raw, 48);
    CHECK_EQ_U32(b.opcode(), ISCSI_OP_SCSI_CMD, "opcode");
    CHECK(b.fBit(), "F bit");
    CHECK_EQ_U32(b.dataLen(), 65536, "DataSegmentLength");
    CHECK_EQ_U32(b.itt(), 0xDEADBEEF, "ITT");
    CHECK_EQ_U32(b.edtl(), 0x11223344, "ExpectedDataTransferLength");
    CHECK_EQ_U32(b.cmdSn(), 7, "CmdSN");
    CHECK_EQ_U32(b.cdb()[0], 0x2A, "CDB byte 0 is at offset 32");

    // A Data-Out PDU carries the fields the parked-write table matches on: ITT, TTT,
    // DataSN and BufferOffset (RFC 7143 section 11.6).
    uint8_t do_[48] = {};
    do_[0] = 0x05;                       // Data-Out
    do_[1] = 0x80;                       // F: last PDU of the burst
    iscsi_wr24(do_ + 5, 65536);
    iscsi_wr32(do_ + 16, 0x01020304);    // ITT
    iscsi_wr32(do_ + 20, 0xAABBCCDD);    // TTT
    iscsi_wr32(do_ + 36, 2);             // DataSN
    iscsi_wr32(do_ + 40, 131072);        // BufferOffset
    IscsiBhs d;
    memcpy(d.raw, do_, 48);
    CHECK_EQ_U32(d.opcode(), ISCSI_OP_DATA_OUT, "Data-Out opcode");
    CHECK(d.fBit(), "Data-Out F bit");
    CHECK_EQ_U32(d.itt(), 0x01020304, "Data-Out ITT");
    CHECK_EQ_U32(d.ttt(), 0xAABBCCDD, "Data-Out TTT");
    CHECK_EQ_U32(d.dataSn(), 2, "Data-Out DataSN");
    CHECK_EQ_U32(d.bufferOff(), 131072, "Data-Out BufferOffset");
    CHECK_EQ_U32(d.dataLen(), 65536, "Data-Out DataSegmentLength");
}

// ---------------------------------------------------------------------------
//  3. The PDU builders: exact bytes, which is what a capture used to be needed for
// ---------------------------------------------------------------------------
static void test_builders(void) {
    // --- R2T (RFC 7143 section 11.8): opcode 0x31, and byte 1 is 0x80 ------------
    // The 0x80 is not cosmetic: the reference target sends it and this bridge sent
    // 0x00 for a whole round while fifteen hypotheses searched the R2T's other fields
    // (DESIGN 8.63).
    uint8_t r2t[48];
    iscsiBuildR2T(r2t, 0x19, 1, 0x38, 0x1A, 0x21, 0, 65536, 196608);
    CHECK_EQ_U32(r2t[0], ISCSI_OP_R2T, "R2T opcode 0x31");
    CHECK_EQ_U32(r2t[1], 0x80, "R2T byte 1 = 0x80, the reference's value");
    CHECK_EQ_U32(iscsi_rd24(r2t + 5), 0, "R2T has no data segment");
    CHECK_EQ_U32(iscsi_rd32(r2t + 16), 0x19, "R2T ITT echoes the command");
    CHECK_EQ_U32(iscsi_rd32(r2t + 20), 1, "R2T TTT");
    CHECK_EQ_U32(iscsi_rd32(r2t + 24), 0x38, "R2T StatSN is the current one, not incremented");
    CHECK_EQ_U32(iscsi_rd32(r2t + 28), 0x1A, "R2T ExpCmdSN");
    CHECK_EQ_U32(iscsi_rd32(r2t + 32), 0x21, "R2T MaxCmdSN");
    CHECK_EQ_U32(iscsi_rd32(r2t + 36), 0, "R2TSN starts at 0 for a command");
    CHECK_EQ_U32(iscsi_rd32(r2t + 40), 65536, "R2T BufferOffset: after the immediate data");
    CHECK_EQ_U32(iscsi_rd32(r2t + 44), 196608, "R2T DesiredDataTransferLength");

    // --- Data-In: the padding and the offsets Windows reads -----------------------
    uint8_t di[48];
    iscsiBuildDataIn(di, 0x21, 0x40, 0x22, 0x29, 3, 131072, 65536, true);
    CHECK_EQ_U32(di[0], ISCSI_OP_DATA_IN, "Data-In opcode 0x25");
    CHECK_EQ_U32(di[1], 0x80, "Data-In F bit");
    CHECK_EQ_U32(iscsi_rd24(di + 5), 65536, "Data-In DataSegmentLength excludes padding");
    CHECK_EQ_U32(iscsi_rd32(di + 16), 0x21, "Data-In ITT");
    CHECK_EQ_U32(iscsi_rd32(di + 20), 0xFFFFFFFF, "Data-In TTT is unsolicited");
    CHECK_EQ_U32(iscsi_rd32(di + 24), 0x40, "Data-In StatSN is not consumed by data PDUs");
    CHECK_EQ_U32(iscsi_rd32(di + 36), 3, "Data-In DataSN");
    CHECK_EQ_U32(iscsi_rd32(di + 40), 131072, "Data-In BufferOffset");
    CHECK_EQ_U32(iscsi_pad4(iscsi_rd24(di + 5)), 0, "a 64 KiB data segment needs no padding");
    iscsiBuildDataIn(di, 0x21, 0x40, 0x22, 0x29, 0, 0, 7, true);
    CHECK_EQ_U32(iscsi_pad4(iscsi_rd24(di + 5)), 1, "a 7-byte data segment needs one pad byte");

    // --- SCSI Response: status in byte 3, and the residual -------------------------
    uint8_t rsp[48];
    iscsiBuildScsiRsp(rsp, 0x30, SCSI_STATUS_GOOD, false, 0x55, 0x31, 0x38, 0);
    CHECK_EQ_U32(rsp[0], ISCSI_OP_SCSI_RSP, "SCSI Response opcode 0x21");
    CHECK_EQ_U32(rsp[1], 0x80, "SCSI Response F");
    CHECK_EQ_U32(rsp[2], 0x00, "response code: completed at target");
    CHECK_EQ_U32(rsp[3], SCSI_STATUS_GOOD, "status lives in byte 3, not byte 2");
    CHECK_EQ_U32(iscsi_rd32(rsp + 16), 0x30, "SCSI Response ITT");
    CHECK_EQ_U32(iscsi_rd32(rsp + 20), 0xFFFFFFFF, "SCSI Response TTT unsolicited");
    CHECK_EQ_U32(iscsi_rd32(rsp + 24), 0x55, "SCSI Response StatSN");
    CHECK_EQ_U32(iscsi_rd32(rsp + 44), 0, "residual 0");
    iscsiBuildScsiRsp(rsp, 0x30, SCSI_STATUS_GOOD, true, 0x55, 0x31, 0x38, 4096);
    CHECK_EQ_U32(rsp[1], 0xC0, "underflow sets U as well as F");
    CHECK_EQ_U32(iscsi_rd32(rsp + 44), 4096, "residual is carried");
    iscsiBuildScsiRsp(rsp, 0x30, SCSI_STATUS_CHECK_COND, false, 0x55, 0x31, 0x38, 0);
    CHECK_EQ_U32(rsp[3], SCSI_STATUS_CHECK_COND, "CHECK CONDITION in byte 3");

    // The sense data block: 2-byte length then the 18-byte fixed-format sense, and its
    // own padding rule.
    uint8_t sense[18];
    const uint32_t sl = iscsiBuildSense(sense, SCSI_SENSE_ILLEGAL_REQUEST,
                                                SCSI_ASC_LBA_OUT_OF_RANGE, 0);
    CHECK_EQ_U32(sl, 18, "sense is 18 bytes");
    CHECK_EQ_U32(sense[0], 0x70, "fixed format, current error");
    CHECK_EQ_U32(sense[2], SCSI_SENSE_ILLEGAL_REQUEST, "sense key");
    CHECK_EQ_U32(sense[7], 10, "additional sense length");
    CHECK_EQ_U32(sense[12], SCSI_ASC_LBA_OUT_OF_RANGE, "ASC");
    CHECK_EQ_U32(iscsi_pad4(sl + 2), 0, "SenseLength + sense is already 4-aligned");
}

// ---------------------------------------------------------------------------
//  4. Login text: NUL-separated key=value, and the answer-only-what-was-offered rule
// ---------------------------------------------------------------------------
static void test_login_text(void) {
    // Windows offers this on a normal session (captured, DESIGN 8.55/8.63).  The
    // segments are NUL-separated and the last one has no trailing NUL on the wire.
    std::string offer = "HeaderDigest=None,CRC32C";
    offer.push_back('\0');
    offer += "InitialR2T=No";
    offer.push_back('\0');
    offer += "ImmediateData=Yes";
    offer.push_back('\0');
    offer += "FirstBurstLength=65536";
    offer.push_back('\0');
    offer += "MaxRecvDataSegmentLength=65536";
    IscsiText t;
    t.parse((const uint8_t*)offer.data(), (uint32_t)offer.size());
    CHECK_EQ_U32(t.kv.size(), 5, "five keys parsed");
    CHECK(t.get("InitialR2T") == "No", "InitialR2T offer");
    CHECK(t.get("ImmediateData") == "Yes", "ImmediateData offer");
    CHECK(t.get("FirstBurstLength") == "65536", "FirstBurstLength offer");
    CHECK(t.get("HeaderDigest") == "None,CRC32C", "a comma list survives parsing");
    CHECK(t.get("NoSuchKey").empty(), "an absent key reads as empty, not as garbage");

    // makeText: every key ends with a NUL, including the last one - the SendTargets
    // reply depends on it (Windows answers "Invalid SendTargets response text" if the
    // framing is off, and the ",1" portal-group suffix is load-bearing too).
    std::string body = "TargetName=iqn.test";
    body.push_back('\0');
    body += "TargetAddress=127.0.0.1:3260,1";
    IscsiText::TextSegment seg = IscsiText::makeText(body);
    CHECK(seg.wire.size() >= body.size(), "wire form is at least the text");
    CHECK_EQ_U32(seg.wire.size() % 4, 0, "wire form is padded to four bytes");
    CHECK(seg.wire[body.size()] == '\0', "the text is NUL-terminated on the wire");
}

// ---------------------------------------------------------------------------
//  5. Negotiation rules
// ---------------------------------------------------------------------------
static void test_negotiation(void) {
    // InitialR2T is OR-negotiated (LIO TYPERANGE_BOOL_OR).  The measured case is the
    // one that mattered: Windows offers No, the reference target answers Yes, and Yes
    // wins - answering No here is what made Windows reject the R2T (DESIGN 8.63).
    CHECK(iscsiBoolOr("No", "Yes") == "Yes", "InitialR2T: our Yes wins over their No");
    CHECK(iscsiBoolOr("Yes", "No") == "Yes", "InitialR2T: their Yes wins the OR over our No");
    CHECK(iscsiBoolOr("Yes", "Yes") == "Yes", "InitialR2T: both Yes");
    CHECK(iscsiBoolOr("No", "No") == "No", "InitialR2T: both No");
    CHECK(iscsiBoolOr("", "Yes") == "Yes", "InitialR2T: an absent offer does not veto ours");

    // ImmediateData is AND-negotiated (TYPERANGE_BOOL_AND): both must say Yes.
    CHECK(iscsiBoolAnd("Yes", "Yes") == "Yes", "ImmediateData: both Yes");
    CHECK(iscsiBoolAnd("Yes", "No") == "No", "ImmediateData: our No wins");
    CHECK(iscsiBoolAnd("No", "Yes") == "No", "ImmediateData: their No wins");
    CHECK(iscsiBoolAnd("No", "No") == "No", "ImmediateData: both No");

    // Number keys: the smaller wins, a responder may only lower its own, and an
    // absent/unusable offer is not a constraint.
    CHECK_EQ_U32(iscsiNumberMin("262144", 65536), 65536, "their larger value does not raise ours");
    CHECK_EQ_U32(iscsiNumberMin("65536", 262144), 65536, "their smaller value lowers ours");
    CHECK_EQ_U32(iscsiNumberMin("", 65536), 65536, "absent offer: ours stands");
    CHECK_EQ_U32(iscsiNumberMin("0", 65536), 65536, "a zero offer is not a constraint");
    CHECK_EQ_U32(iscsiNumberMin("not-a-number", 65536), 65536, "garbage offer: ours stands");
    CHECK_EQ_U32(iscsiNumberMin("512", 65536), 512, "the RFC 7143 s13.14 minimum is respected");
}

// ---------------------------------------------------------------------------
//  5b. The R2T burst size, including the limit that protects the backend buffer
// ---------------------------------------------------------------------------
static void test_burst_size(void) {
    // The regression: MaxBurstLength 4 MiB (what install.ps1 pushes, mirroring the
    // initiator's own value) against a backend that can stage 256 KiB.  Before the
    // clamp this returned 4194304 and the backend copied 4 MiB into a 256 KiB buffer.
    CHECK_EQ_U32(iscsiBurstSize(4194304u, 4194304u, 262144u, 262144u, 512u), 262144u,
                 "a 4 MiB burst is clamped to what the backend can stage");
    CHECK_EQ_U32(iscsiBurstSize(4194304u, 4194304u, 262144u, 4194304u, 512u), 4194304u,
                 "with a 4 MiB staging buffer the whole 4 MiB burst goes out in one R2T");
    // stageMax == 0 is "the backend has no opinion", which must NOT mean "zero bytes".
    CHECK_EQ_U32(iscsiBurstSize(4194304u, 4194304u, 262144u, 0u, 512u), 4194304u,
                 "a backend with no staging preference does not veto the burst");
    // The burst is never larger than the PDU size when the PDU size is the bigger of
    // the two: asking for more than one PDU in a single R2T would still be legal, but
    // this is the rule sendNextR2T has always used, so it is pinned here.
    CHECK_EQ_U32(iscsiBurstSize(1048576u, 262144u, 1048576u, 4194304u, 512u), 1048576u,
                 "the larger of MaxBurstLength and our segment length is the burst");
    // The tail: a burst never exceeds what is still outstanding, so the last R2T of a
    // command asks for the remainder only.  bs = 1 here so that the block rounding
    // below cannot be what the check is measuring - the first draft of this check
    // used bs = 512 and expected 1000000, and 1000000 is not a multiple of 512.
    CHECK_EQ_U32(iscsiBurstSize(1000000u, 4194304u, 262144u, 4194304u, 1u), 1000000u,
                 "the last burst is the remainder, not a full burst");
    CHECK_EQ_U32(iscsiBurstSize(1000000u, 4194304u, 262144u, 4194304u, 512u), 999936u,
                 "the same remainder rounds down to a whole number of blocks");
    // Whole blocks only: a burst that is not a multiple of the block size would make
    // the backend write a partial block, and the residual arithmetic would be wrong.
    CHECK_EQ_U32(iscsiBurstSize(1000000u, 4194304u, 262144u, 4194304u, 4096u), 999424u,
                 "the burst is truncated to whole blocks");
    CHECK_EQ_U32(iscsiBurstSize(4096u, 4194304u, 262144u, 4194304u, 4096u), 4096u,
                 "a single block still asks for a whole block");
    // A remainder smaller than one block has no whole block left; 0 is the caller's
    // signal to refuse, and it must not be confused with a valid small burst.
    CHECK_EQ_U32(iscsiBurstSize(2048u, 4194304u, 262144u, 4194304u, 4096u), 0u,
                 "less than one block left yields 0, the caller's refusal signal");
}

// ---------------------------------------------------------------------------
//  6. The parked-write table: the rules that keep a write state machine bounded
// ---------------------------------------------------------------------------
static void test_pending_table(void) {
    // --- one entry per ITT --------------------------------------------------------
    // Two live entries with the same ITT make find() ambiguous: the second write's
    // Data-Out would be copied into the first write's buffer.
    {
        PendingWrites t(8);
        const char* why = nullptr;
        PendingWrite* a = t.add(0x100, 1000, &why);
        CHECK(a != nullptr, "first entry is accepted");
        PendingWrite* dup = t.add(0x100, 1000, &why);
        CHECK(dup == nullptr, "a duplicate ITT is refused");
        CHECK(why && strstr(why, "duplicate") != nullptr, "and it says why");
        CHECK_EQ_U32(t.size(), 1, "the refused entry did not grow the table");
        CHECK(t.find(0x100) == a, "find still returns the live entry");
    }

    // --- bounded by the advertised command window ---------------------------------
    // Each entry can hold a whole MaxBurstLength buffer (this bridge advertises 4 MiB),
    // so "bounded by the protocol" has to be enforced here rather than assumed.
    {
        PendingWrites t(8);
        const char* why = nullptr;
        for (uint32_t i = 0; i < 8; i++) {
            CHECK(t.add(0x200 + i, 1000, &why) != nullptr, "entries up to the window are accepted");
        }
        CHECK_EQ_U32(t.size(), 8, "table is full at the window size");
        PendingWrite* over = t.add(0x999, 1000, &why);
        CHECK(over == nullptr, "one past the window is refused");
        CHECK(why && strstr(why, "full") != nullptr, "and it says the table is full");
        CHECK_EQ_U32(t.size(), 8, "a refused entry cannot grow the table");
        // A cap of 0 disables the bound, which is only for tests - but it must not
        // misbehave either.
        PendingWrites u(0);
        for (uint32_t i = 0; i < 64; i++) CHECK(u.add(0x300 + i, 1000, &why) != nullptr, "unbounded table accepts");
        CHECK_EQ_U32(u.size(), 64, "unbounded table keeps them");
    }

    // --- the R2T range arithmetic -------------------------------------------------
    // A Data-Out is accepted only inside [wantOff, wantEnd): the guard that keeps a
    // stale or hostile offset from being memcpy'd into the burst buffer.
    {
        PendingWrites t(4);
        PendingWrite* pw = t.add(0x400, 1000, nullptr);
        pw->bytes = 262144;
        pw->received = 65536;        // the immediate data already arrived
        pw->wantOff = 65536;
        pw->want = 196608;
        CHECK_EQ_U32(pw->wantEnd(), 262144, "R2T range end");
        CHECK_EQ_U32(pw->remaining(), 196608, "bytes still to come");
        CHECK(!pw->complete(), "not complete yet");
        pw->received = 262144;
        CHECK(pw->complete(), "complete at the advertised length");
        CHECK_EQ_U32(pw->remaining(), 0, "nothing left");
        pw->wantOff = 4000000000u;   // the arithmetic must be 64-bit: wantOff + want
        pw->want = 400000000u;       // overflows 32 bits on purpose
        CHECK(pw->wantEnd() > 4000000000u, "wantEnd is computed in 64 bits");
    }

    // --- progress, not age, decides what is stalled -------------------------------
    // A slow write that keeps delivering Data-Out must not be killed; one that never
    // delivers must not hold a session forever.
    {
        PendingWrites t(4);
        PendingWrite* pw = t.add(0x500, 1000, nullptr);
        CHECK(t.stalled(1000 + 59999, 60000) == nullptr, "young entry is not stalled");
        CHECK(t.stalled(1000 + 60000, 60000) == pw, "no progress for the whole window = stalled");
        pw->lastDataMs = 61000;                        // progress: one Data-Out PDU
        CHECK(t.stalled(61000 + 59999, 60000) == nullptr, "progress resets the stall timer");
        CHECK(t.stalled(61000 + 60000, 60000) == pw, "and it stalls again after another window");
        CHECK(t.find(0x500) == nullptr ? false : true, "the stalled entry is still in the table until the caller erases it");
    }

    // --- erase, buffered bytes ----------------------------------------------------
    {
        PendingWrites t(4);
        PendingWrite* a = t.add(1, 0, nullptr);
        PendingWrite* b = t.add(2, 0, nullptr);
        a->buf.assign(262144, 0);
        b->buf.assign(131072, 0);
        CHECK_EQ_U32(t.bufferedBytes(), 393216, "the table reports what it holds");
        t.erase(a);
        CHECK_EQ_U32(t.size(), 1, "erase removes one entry");
        CHECK(t.find(1) == nullptr, "the erased ITT is gone");
        CHECK(t.find(2) == b, "the other entry survives the erase (no iterator reuse bug)");
        t.erase(nullptr);            // must not crash: callers pass find() results directly
        CHECK_EQ_U32(t.size(), 1, "erasing nothing changes nothing");
        t.clear();
        CHECK(t.empty(), "clear empties the table");
    }
}

// ---------------------------------------------------------------------------
//  7. A whole write, played through the table the way session() plays it
// ---------------------------------------------------------------------------
static void test_write_flow(void) {
    // 4 MiB write, immediate data 64 KiB, then the R2T-driven remainder in bursts -
    // the shape measured on the wire (DESIGN 8.64).  This is the accounting, not the
    // I/O: it is what decides when the command is answered.
    PendingWrites t(8);
    const uint32_t kTotal = 4u * 1024 * 1024;
    PendingWrite* pw = t.add(0x700, 0, nullptr);
    pw->bs = 512;
    pw->bytes = kTotal;
    pw->received = 65536;            // immediate data in the command PDU
    pw->ttt = 1;

    uint32_t chunk = 4u * 1024 * 1024;     // one R2T per burst
    if (chunk > pw->remaining()) chunk = pw->remaining();
    pw->wantOff = pw->received;
    pw->want = chunk;
    pw->chunkOff = pw->received;           // where buf starts - sendNextR2T sets this
    pw->buf.assign(chunk, 0);
    CHECK_EQ_U32(pw->wantEnd(), kTotal, "one burst covers the whole remainder");

    // Four Data-Out PDUs carrying the remainder: three of 1 MiB and a shorter last one,
    // because the remainder is 4 MiB MINUS the 64 KiB of immediate data - writing four
    // full MiB here is exactly the arithmetic slip the range guard is there to catch
    // (it did, on this suite's first run).
    uint32_t off = pw->wantOff;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t left = (uint32_t)(pw->wantEnd() - off);
        const uint32_t len = (left < 1024 * 1024) ? left : 1024 * 1024;
        CHECK(off >= pw->wantOff && (uint64_t)off + len <= pw->wantEnd(), "Data-Out inside the R2T");
        pw->received = off + len;
        pw->lastDataMs += 1;
        off += len;
        if (i < 3) CHECK(!pw->complete(), "not complete until the last PDU");
    }
    CHECK(pw->complete(), "complete after the fourth Data-Out");
    CHECK_EQ_U32(pw->received - pw->chunkOff, pw->want, "every requested byte landed in the burst");
    CHECK_EQ_U32(pw->chunkOff / pw->bs + pw->want / pw->bs, kTotal / 512, "blocks written cover the command");
    t.erase(pw);
    CHECK(t.empty(), "the table is empty once the write is answered");

    // A short write that never completes is exactly what the stall guard is for.
    PendingWrites u(8);
    PendingWrite* q = u.add(0x701, 5000, nullptr);
    q->bytes = kTotal;
    q->received = 65536;
    q->wantOff = 65536;
    q->want = 1024 * 1024;
    q->buf.assign(q->want, 0);
    CHECK(u.stalled(5000 + 60000, 60000) == q, "a write that never got its Data-Out is stalled");
}

// ---------------------------------------------------------------------------
//  8b. The NVMe-oF capability table: what we advertise must be what we answer
// ---------------------------------------------------------------------------
//
// ONCS and NSFEAT are promises a host acts on.  This target sat with ONCS = 0 while
// Write Zeroes and DSM were implemented and byte-verified, and the cost was that no
// host's block layer would offer discard on the namespace at all; the opposite
// mistake - a bit set for a command the dispatch has no case for - is worse, because
// the host sends it and gets a failure it could not have predicted.  The two sides
// therefore come from one place, kNvmeOfIoCaps in nvmeof_wire.h, and this pins it.
static void test_nvmeof_caps(void) {
    const uint16_t want = NVMEOF_CTRL_ONCS_DSM | NVMEOF_CTRL_ONCS_WRITE_ZEROES;
    const uint16_t advertised = nvmeofOncsFromCaps();

    CHECK_EQ_U32(advertised, want, "ONCS is exactly Dataset Management + Write Zeroes");

    // Every row contributes its bit and every bit comes from a row: a bit cannot be
    // advertised without a row, and a row cannot be silently ignored.
    uint16_t fromRows = 0;
    for (unsigned i = 0; i < sizeof(kNvmeOfIoCaps) / sizeof(kNvmeOfIoCaps[0]); i++) {
        fromRows |= kNvmeOfIoCaps[i].oncsBit;
        CHECK((advertised & kNvmeOfIoCaps[i].oncsBit) != 0,
              "every row's bit appears in the ONCS the target writes");
        CHECK(kNvmeOfIoCaps[i].opcode == NVMEOF_OPC_DSM ||
              kNvmeOfIoCaps[i].opcode == NVMEOF_OPC_WRITE_ZEROES,
              "every row names an opcode the I/O dispatch has a case for");
    }
    CHECK_EQ_U32(fromRows, advertised, "no ONCS bit exists outside the table");

    // The bits we must NOT set, each because there is no case for the command behind
    // it.  These four are where "a made-up capability bit is worse than a missing
    // one" stops being a slogan: a host believes them.
    CHECK((advertised & NVMEOF_CTRL_ONCS_COMPARE) == 0, "Compare is not advertised");
    CHECK((advertised & NVMEOF_CTRL_ONCS_WRITE_UNCOR) == 0, "Write Uncorrectable is not advertised");
    CHECK((advertised & NVMEOF_CTRL_ONCS_RESERVATIONS) == 0, "Reservations are not advertised");
    CHECK((advertised & NVMEOF_CTRL_ONCS_TIMESTAMP) == 0, "Timestamp is not advertised");

    // The namespace-side half, with its value read from the reference header
    // (ref/linux_nvme.h: NVME_NS_FEAT_THIN = 1 << 0).
    CHECK_EQ_U32(NVMEOF_NS_FEAT_THIN, 1u, "nsfeat bit 0 is thin provisioning");

    // The Identify Namespace offsets.  NSFEAT/DLFEAT/NPWG..NOWS were DERIVED by
    // walking struct nvme_id_ns (ref/linux_nvme.h:438), and the five older offsets
    // are the anchors that prove the walk - so they are pinned here alongside the new
    // ones.  A future edit that moves a field has to argue with the derivation.
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NSFEAT, 24u, "nsfeat follows nsze/ncap/nuse");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NLBAF,  25u, "anchor: nlbaf is byte 25");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_FLBAS,  26u, "anchor: flbas is byte 26");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NMIC,   30u, "anchor: nmic is byte 30");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_DLFEAT, 33u, "dlfeat follows nmic/rescap/fpi");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NPWG,   64u, "npwg follows nvmcap[16]");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NPWA,   66u, "npwa follows npwg");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NPDG,   68u, "npdg follows npwa");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NPDA,   70u, "npda follows npdg");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NOWS,   72u, "nows follows npda");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_NSATTR, 99u, "anchor: nsattr is byte 99");
    CHECK_EQ_U32(NVMEOF_ID_NS_OFF_LBAF,  128u, "anchor: lbaf[0] is byte 128");
}

int main(void) {
    printf("nvmeof_iscsi self-test\n");
    test_padding();
    test_bhs_accessors();
    test_builders();
    test_login_text();
    test_negotiation();
    test_burst_size();
    test_pending_table();
    test_write_flow();
    test_nvmeof_caps();

    if (g_failures == 0) {
        printf("  iscsi self-test: PASS (padding, BHS, PDU builders, login text, "
               "negotiation rules, R2T burst sizing, parked-write table, "
               "NVMe-oF capability table)\n");
        return 0;
    }
    printf("  iscsi self-test: %d FAILURE(S)\n", g_failures);
    return 1;
}
