// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  The parked-write table, on its own so it can be tested without hardware.
//
//  A WRITE whose data does not fit in the command PDU becomes STATE here: the
//  command is parked, an R2T asks for the rest, and the Data-Out PDUs that answer it
//  are matched back by ITT (DESIGN 8.64).  That was previously a std::vector living
//  inside IscsiTarget, where the only way to test a rule was to attach a real
//  initiator - and the rules are exactly the part worth testing:
//
//    * ONE entry per ITT.  The ITT identifies the command, so two live entries with
//      the same ITT make find() ambiguous: the second write's data would be copied
//      into the first write's buffer.  A duplicate is an initiator protocol
//      violation and is refused here, not accepted and mis-assigned.
//    * BOUNDED.  The initiator may only have MaxCmdSN - ExpCmdSN + 1 commands in
//      flight, so a table larger than the advertised command window can only be
//      filled by an initiator that broke the window rule - and each entry can hold a
//      whole MaxBurstLength buffer, which this bridge now advertises as 4 MiB.  This
//      is the difference between "bounded by the protocol" and "bounded by the
//      peer's goodwill".
//    * STALLABLE.  A parked write whose initiator stops sending Data-Out holds its
//      buffer until the session ends.  The table remembers when each entry last made
//      progress so the session loop can give up on it.
//
//  No sockets, no RDMA, no iSCSI wire header: <vector>, <string> and <chrono> only,
//  so iscsi_selftest.cpp can include it on a hosted runner.
// ---------------------------------------------------------------------------
#ifndef NVMEOF_ISCSI_PENDING_H
#define NVMEOF_ISCSI_PENDING_H

#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

// One parked WRITE.
struct PendingWrite {
    uint32_t itt = 0;               // the command's initiator task tag
    uint32_t ttt = 0;               // target transfer tag we handed out in the R2T
    uint64_t lba = 0;
    uint32_t bs = 512;
    uint32_t bytes = 0;             // total the command asked for
    uint32_t received = 0;          // absolute offset of the next expected byte
    uint32_t wantOff = 0;           // absolute offset of the outstanding R2T
    uint32_t want = 0;              // bytes the outstanding R2T asked for
    uint32_t chunkOff = 0;          // where `buf` starts within the command
    uint32_t r2tSn = 0;
    std::vector<uint8_t> buf;       // ONE burst, never the whole command
    uint64_t startedMs = 0;         // when it was parked
    uint64_t lastDataMs = 0;        // when its last Data-Out PDU arrived

    // End of the outstanding R2T's range.  A Data-Out is accepted only inside
    // [wantOff, wantEnd); anything else means the initiator and this target disagree
    // about where the transfer is, and the byte stream can no longer be trusted.
    uint64_t wantEnd() const { return (uint64_t)wantOff + (uint64_t)want; }
    // Bytes still to come over the whole command.
    uint32_t remaining() const { return bytes - received; }
    bool complete() const { return received >= bytes; }
};

class PendingWrites {
public:
    // `cap` is the advertised command window: at most that many commands may be in
    // flight, and therefore at most that many writes may be parked.  A cap of 0
    // disables the bound, which is only for tests that want to see what happens.
    explicit PendingWrites(uint32_t cap = 0) : cap_(cap) {}

    // Refusals are reported through `why` (a static string, safe to print) and the
    // entry is NOT created.  Returning nullptr with a reason beats both silently
    // accepting (memory grows, ITTs collide) and silently dropping (the initiator
    // waits forever).
    PendingWrite* add(uint32_t itt, uint64_t nowMs, const char** why) {
        if (find(itt)) { if (why) *why = "duplicate ITT: a command with this tag is already parked"; return nullptr; }
        if (cap_ && v_.size() >= cap_) { if (why) *why = "the parked-write table is full (command window exceeded)"; return nullptr; }
        PendingWrite pw;
        pw.itt = itt;
        pw.startedMs = nowMs;
        pw.lastDataMs = nowMs;
        v_.push_back(pw);
        return &v_.back();
    }

    PendingWrite* find(uint32_t itt) {
        for (auto& pw : v_) if (pw.itt == itt) return &pw;
        return nullptr;
    }

    void erase(PendingWrite* pw) {
        if (!pw) return;
        for (size_t i = 0; i < v_.size(); i++) {
            if (&v_[i] == pw) { v_.erase(v_.begin() + (ptrdiff_t)i); return; }
        }
    }

    size_t  size() const { return v_.size(); }
    bool    empty() const { return v_.empty(); }
    uint32_t cap() const { return cap_; }
    // What the table costs while it is full: the bound is on entries, and the memory
    // is entries x one burst.
    size_t bufferedBytes() const {
        size_t n = 0;
        for (const auto& pw : v_) n += pw.buf.size();
        return n;
    }

    // The entry that has made no progress for longer than `timeoutMs`, or nullptr.
    // Progress, not age: a slow write that keeps delivering Data-Out is not stalled,
    // and a write that never delivers one should not keep a session (or its buffer)
    // forever.  The caller decides what to do - this table only answers the question.
    PendingWrite* stalled(uint64_t nowMs, uint64_t timeoutMs) {
        for (auto& pw : v_) {
            if (nowMs - pw.lastDataMs >= timeoutMs) return &pw;
        }
        return nullptr;
    }

    void clear() { v_.clear(); }
    const std::deque<PendingWrite>& all() const { return v_; }

private:
    // std::deque, not std::vector, and the self-test is why: add() hands out a pointer
    // to the entry, and a vector reallocates on growth - so the *next* add() left the
    // previous pointer dangling.  iscsi_selftest.cpp caught it on the first run (a test
    // that added two entries and filled both buffers reported only the second one:
    // 131072 bytes instead of 393216).  The bridge's own call sites happen not to hold
    // a pointer across an add(), so nothing was broken yet - but an API whose handles
    // die on the next call is a trap set for the next edit.  deque keeps references to
    // existing elements stable across push_back.
    std::deque<PendingWrite> v_;
    uint32_t cap_ = 0;
};

#endif // NVMEOF_ISCSI_PENDING_H
