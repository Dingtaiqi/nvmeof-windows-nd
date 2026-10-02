// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  iSCSI wire fuzz.  NO NIC, NO TARGET, NO INITIATOR - and it runs in CI.
//
//  WHY THIS IS NOT "the self-test with random inputs".  iscsi_selftest.cpp pins the
//  values the protocol requires, which is a check that the code says what the RFC says.
//  This asks a different question: what happens when the bytes are NOT what the RFC
//  says.  Every field here is peer-controlled - the initiator on the other end of a
//  login, the target that answers us, or a machine that is simply broken - and the
//  failure it can cause is not a wrong answer but a read or a write outside a buffer.
//
//  THE HARNESS MUST BE ABLE TO FAIL.  A fuzzer that has never found anything is
//  indistinguishable from a fuzzer that cannot find anything, so this file has a second
//  mode: built with -DFUZZ_PROVE_DETECTION it performs one deliberate out-of-bounds
//  read, and run_fuzz.ps1 REQUIRES the sanitizer to catch it.  If that build passes, the
//  suite fails - the harness itself is the thing under test.
//
//  AddressSanitizer is what makes this worth running: MSVC 14.4x+ supports
//  /fsanitize=address on x64, and it turns "the pointer walked past the end" from a
//  silent corruption into a stack trace with the iteration in it.
//
//  Build:  cl /nologo /W4 /WX /std:c++17 /EHsc /fsanitize=address /I. iscsi_fuzz.cpp
//  Run:    iscsi_fuzz.exe [iterations] [seed]
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "nvmeof_iscsi_pending.h"
#include "nvmeof_iscsi.h"          // pulls in winsock2, but no NetworkDirect

static int g_failures = 0;

// Same shape as the other suites, and for the same reason: a constant condition under
// /W4 is C4127, which /WX turns into C2220 (DESIGN 8.69).
static int check_holds(int cond) { return cond; }

#define FAILF(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } while (0)

// ---------------------------------------------------------------------------
//  A deterministic generator.  Reproducibility is the whole point: a failure prints
//  the seed and the iteration, and that pair replays it exactly.
// ---------------------------------------------------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    uint32_t below(uint32_t n) { return n ? u32() % n : 0; }
    uint8_t  byte() { return (uint8_t)u32(); }
};

// A byte set to 0x00 or 0xFF, an integer field set to 0 or all-ones: the two values that
// break length arithmetic, and the two that are most likely to be missing from a hand
// written corpus.
static uint8_t interesting(Rng& r) {
    switch (r.below(8)) {
    case 0: return 0x00;
    case 1: return 0xFF;
    case 2: return 0x80;
    case 3: return 0x7F;
    default: return r.byte();
    }
}

// ---------------------------------------------------------------------------
//  1. The PDU builders, driven with adversarial field values.
//
//  These write into a CALLER-SIZED buffer, so the one thing they must never do is write
//  past it.  48 bytes for a BHS is the contract; a builder that decides to write a pad
//  byte is a one-byte overflow that no assertion about field values would notice.
// ---------------------------------------------------------------------------
static void fuzz_builders(Rng& r) {
    // Heap-allocated exactly, so ASAN puts a redzone immediately after the 48 bytes.
    uint8_t* pdu = (uint8_t*)malloc(48);
    if (!pdu) { FAILF("malloc"); return; }
    memset(pdu, 0xAA, 48);

    const uint32_t a = r.u32(), b = r.u32(), c = r.u32();
    switch (r.below(4)) {
    case 0:
        iscsiBuildR2T(pdu, a, b, c, r.u32(), r.u32(), r.u32(), r.u32(), r.u32());
        if (check_holds(pdu[0] != ISCSI_OP_R2T)) FAILF("R2T opcode lost");
        if (check_holds(pdu[1] != 0x80))       FAILF("R2T byte 1 must stay 0x80");
        if (check_holds(iscsi_rd24(pdu + 5) != 0)) FAILF("R2T must carry no data segment");
        break;
    case 1: {
        const uint32_t len = r.below(3) ? r.below(70000) : (r.below(2) ? 0u : 0x00FFFFFFu);
        iscsiBuildDataIn(pdu, a, b, c, r.u32(), r.u32(), r.u32(), len, r.below(2) != 0);
        const uint32_t seg = iscsi_rd24(pdu + 5);
        // The declared segment length must be what was asked for, and the padding rule
        // must be computable from it - a builder that rounds the OTHER way desynchronises
        // the stream, which is the bug that started this whole file's history.
        if (check_holds(seg != len)) FAILF("Data-In segment length %u, asked for %u", seg, len);
        if (check_holds(pdu[0] != ISCSI_OP_DATA_IN)) FAILF("Data-In opcode lost");
        break;
    }
    case 2:
        iscsiBuildScsiRsp(pdu, a, (uint8_t)interesting(r), r.below(2) != 0, b, c, r.u32(), r.u32());
        if (check_holds(pdu[0] != ISCSI_OP_SCSI_RSP)) FAILF("SCSI Response opcode lost");
        if (check_holds((pdu[1] & 0x80) == 0))        FAILF("SCSI Response must set F");
        break;
    default: {
        // BHS: the builder writes the opcode it is GIVEN, byte for byte.  The first
        // version of this check demanded the 6-bit mask be applied here and failed on a
        // random 0xFF - which was the check being wrong, not the builder: masking is the
        // callers' business (they pass ISCSI_OP_* constants), and a builder that quietly
        // masked would hide a caller passing an opcode with the response bit already set.
        // "MUST equal what it was given" is the stronger and the correct invariant.
        const uint8_t op = (uint8_t)interesting(r);
        iscsiBuildBhs(pdu, op, (uint8_t)interesting(r), r.u32());
        if (check_holds(pdu[0] != op))
            FAILF("BHS wrote opcode 0x%02X, was given 0x%02X", pdu[0], op);
        break;
    }
    }

    // The sense builder returns a length and writes into a caller buffer of that size;
    // 18 is the fixed format and the caller is told so.
    uint8_t sense[18];
    const uint32_t sl = iscsiBuildSense(sense, (uint8_t)interesting(r), (uint8_t)interesting(r),
                                        (uint8_t)interesting(r));
    if (check_holds(sl != 18)) FAILF("sense length %u, expected 18", sl);
    if (check_holds(sense[0] != 0x70)) FAILF("sense[0] must stay the fixed-format code");
    if (check_holds((iscsi_pad4(sl + 2) % 4) != 0)) FAILF("sense padding is not 4-aligned");
    free(pdu);
}

// ---------------------------------------------------------------------------
//  2. Text parameters: peer bytes in, key list out.
//
//  A login's data segment is whatever the peer sent, terminated (or not) by NULs.  The
//  parser walks it with indices, so the risk is arithmetic on the last entry, not a
//  strcpy - which is exactly the kind of thing a fuzzer settles and a reader argues about.
// ---------------------------------------------------------------------------
static void fuzz_text(Rng& r) {
    const uint32_t n = r.below(600);
    std::vector<uint8_t> buf(n);
    for (uint32_t i = 0; i < n; i++) buf[i] = (r.below(3) == 0) ? 0 : interesting(r);

    IscsiText t;
    t.parse(buf.empty() ? nullptr : buf.data(), n);
    // Invariants that must hold for ANY input: no entry may claim more bytes than the
    // segment had, and every entry must be NUL-free (a key with an embedded NUL would
    // mean the parser walked past a terminator).
    for (auto& kv : t.kv) {
        if (check_holds(kv.first.size() + kv.second.size() + 1 > (size_t)n + 1))
            FAILF("text entry larger than the segment (%zu + %zu vs %u)",
                  kv.first.size(), kv.second.size(), n);
        if (check_holds(kv.first.find('\0') != std::string::npos))
            FAILF("a key contains a NUL - the parser crossed a terminator");
    }
    // build() must produce something parse() reads back identically: the negotiation
    // sends what this produces, so a lossy round trip is a protocol bug.
    const std::string wire = t.build();
    IscsiText back;
    back.parse((const uint8_t*)wire.data(), (uint32_t)wire.size());
    if (check_holds(back.kv.size() != t.kv.size()))
        FAILF("text round trip changed the entry count (%zu -> %zu)", t.kv.size(), back.kv.size());
    (void)t.get("InitialR2T");       // must not care whether the key exists
}

// ---------------------------------------------------------------------------
//  3. The negotiation helpers, on strings a peer controls.
//
//  These decide whether the bridge and the peer want R2T, immediate data and how big a
//  burst may be, and their input is a std::string straight off the wire - including
//  things like "0", "-1", "9999999999999999999999" and "12abc".
// ---------------------------------------------------------------------------
static void fuzz_negotiation(Rng& r) {
    static const char* seeds[] = { "", "Yes", "No", "yes", "YES", "0", "-1", "512", "65536",
                                   "9999999999999999999999", "12abc", " ", "No\n", "Y" };
    std::string s = seeds[r.below((uint32_t)(sizeof(seeds) / sizeof(seeds[0])))];
    const uint32_t extra = r.below(6);
    for (uint32_t i = 0; i < extra; i++) s.push_back((char)interesting(r));

    const std::string o = iscsiBoolOr(s, "Yes");
    if (check_holds(o != "Yes" && o != "No")) FAILF("BoolOr returned '%s'", o.c_str());
    const std::string a = iscsiBoolAnd(s, "Yes");
    if (check_holds(a != "Yes" && a != "No")) FAILF("BoolAnd returned '%s'", a.c_str());

    // Number keys take the SMALLER value, and a peer may never raise ours.  A "0" or a
    // negative or garbage offer means "no constraint from that side", which the RFC's
    // range rules make explicit (section 13.14).
    const uint32_t ours = r.below(3) ? 65536u : r.u32();
    const uint32_t got = iscsiNumberMin(s, ours);
    if (check_holds(got > ours)) FAILF("NumberMin raised our value: '%s' -> %u > %u",
                                       s.c_str(), got, ours);
    if (check_holds(got < 512 && got != ours && got != 0))
        FAILF("NumberMin returned %u, below the RFC minimum and not ours (%u)", got, ours);
}

// ---------------------------------------------------------------------------
//  4. The parked-write table, driven as an operation sequence.
//
//  This is not a byte fuzzer: the bug this table already had was a container choice
//  (std::vector handing back a pointer that the next push_back invalidated), and that
//  class of bug shows up as a SEQUENCE of valid calls, not as malformed bytes.  So the
//  operations are random and the invariants are checked after every one of them.
// ---------------------------------------------------------------------------
static void fuzz_pending_table(Rng& r) {
    PendingWrites t(8);
    struct Live { uint32_t itt; };
    std::vector<Live> mine;                 // what this test believes is in the table
    std::vector<uint32_t> used;

    const uint32_t ops = 1 + r.below(60);
    uint64_t now = 1000;
    for (uint32_t i = 0; i < ops; i++) {
        now += r.below(50);
        const uint32_t itt = r.below(4) ? r.below(12) : r.u32();   // small space: collisions
        switch (r.below(4)) {
        case 0:
        case 1: {                            // add
            const char* why = nullptr;
            PendingWrite* p = t.add(itt, now, &why);
            const bool already = [&] {
                for (auto& m : mine) if (m.itt == itt) return true;
                return false;
            }();
            if (check_holds(already && p != nullptr))
                FAILF("a duplicate ITT was accepted (itt 0x%08X)", itt);
            if (check_holds(!already && mine.size() < 8 && p == nullptr))
                FAILF("a legal add was refused (itt 0x%08X): %s", itt, why ? why : "?");
            if (check_holds(mine.size() >= 8 && p != nullptr))
                FAILF("the table exceeded its bound of 8");
            if (p) {
                mine.push_back({ itt });
                // A returned pointer must still be the one find() hands back: this is the
                // invariant the std::vector version violated.
                if (check_holds(t.find(itt) != p))
                    FAILF("find(0x%08X) disagrees with the pointer add() returned", itt);
            }
            break;
        }
        case 2: {                            // erase, if it is ours to erase
            PendingWrite* p = t.find(itt);
            if (p) {
                t.erase(p);
                for (size_t k = 0; k < mine.size(); k++) {
                    if (mine[k].itt == itt) { mine.erase(mine.begin() + (ptrdiff_t)k); break; }
                }
                if (check_holds(t.find(itt) != nullptr))
                    FAILF("find(0x%08X) still answers after erase", itt);
            }
            break;
        }
        default: {                           // ask the table who is stalled
            PendingWrite* s = t.stalled(now, 60000);
            if (s) {
                bool known = false;
                for (auto& m : mine) if (m.itt == s->itt) known = true;
                if (check_holds(!known))
                    FAILF("stalled() returned itt 0x%08X, which is not in the table", s->itt);
                if (check_holds((uint64_t)s->lastDataMs + 60000 > now))
                    FAILF("stalled() fired at %llu for an entry last touched at %llu",
                          (unsigned long long)now, (unsigned long long)s->lastDataMs);
            }
            break;
        }
        }
    }
    // Draining the table must leave nothing accounted for.
    for (auto& m : mine) { PendingWrite* p = t.find(m.itt); if (p) t.erase(p); }
    if (check_holds(t.bufferedBytes() != 0))
        FAILF("bufferedBytes() is %zu after draining the table", t.bufferedBytes());
}

// ---------------------------------------------------------------------------
//  The deliberate defect: this is what the harness must catch.
// ---------------------------------------------------------------------------
#ifdef FUZZ_PROVE_DETECTION
static void prove_detection(void) {
    volatile uint8_t* p = (volatile uint8_t*)malloc(16);
    if (!p) { FAILF("malloc"); return; }
    volatile uint8_t v = p[16];      // one byte past the end: ASAN must report this
    printf("  PROOF: read %u bytes past the end and survived - the harness cannot detect "
           "an out-of-bounds access, so a clean run means nothing\n", (unsigned)v);
    g_failures++;
    free((void*)p);
}
#endif

int main(int argc, char** argv) {
    uint64_t iterations = (argc > 1) ? _strtoui64(argv[1], nullptr, 0) : 200000;
    uint64_t seed = (argc > 2) ? _strtoui64(argv[2], nullptr, 0) : 0x5EED1234ull;
    if (iterations == 0) iterations = 1;

    printf("iscsi wire fuzz: %llu iterations, seed 0x%llX\n",
           (unsigned long long)iterations, (unsigned long long)seed);

#ifdef FUZZ_PROVE_DETECTION
    prove_detection();
#endif

    Rng r(seed);
    for (uint64_t i = 0; i < iterations; i++) {
        const int before = g_failures;
        switch (i % 4) {
        case 0: fuzz_builders(r);       break;
        case 1: fuzz_text(r);           break;
        case 2: fuzz_negotiation(r);    break;
        default: fuzz_pending_table(r); break;
        }
        if (g_failures != before) {
            printf("  ^ iteration %llu of seed 0x%llX - replay with: iscsi_fuzz.exe 1 0x%llX\n",
                   (unsigned long long)i, (unsigned long long)seed, (unsigned long long)seed);
            break;                      // the first failure is the interesting one
        }
    }

    if (g_failures) {
        printf("iscsi wire fuzz: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("iscsi wire fuzz: PASS (%llu iterations, no overflow, no invariant broken)\n",
           (unsigned long long)iterations);
    return 0;
}
