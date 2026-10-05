// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
//  nvmeof_dhchap.h - DH-HMAC-CHAP authentication (the target half).
//
//  Every constant, offset and concatenation here is taken from the Linux reference
//  implementation, read in source form (drivers/nvme/{host,target}/auth.c,
//  target/fabrics-cmd-auth.c, common/auth.c) and summarised in
//  ref/NVME_DHCHAP_PROTOCOL_REPORT.md.  The traps it documents are the reason this
//  file is written the way it is:
//
//   * Auth happens AFTER a successful fabrics Connect, and the target announces it by
//     setting ATR (bit 17) in the Connect COMPLETION RESULT - bits 15:0 stay the
//     controller id.  It is not a status code.
//   * Ks = H(shared_secret) where the secret is exactly p_size bytes, big-endian,
//     LEFT-ZERO-PADDED (mpi_write_to_sgl).  Stripping leading zeros - what every
//     bignum library does by default - changes the HMAC input.
//   * The Reply always carries 2*h bytes of rval area even when cvalid == 0.
//   * The DH value sits at offset 16+h in a Challenge and at 16+2*h in a Reply.
//   * Success2 carries NO HMAC.
//   * NQNs are not in the auth payloads; they come from the Connect data, and the
//     host key is bound to the HOST NQN while the controller key is bound to the
//     SUBSYSTEM NQN.
//   * Every failure ends the controller: the reference calls
//     nvmet_ctrl_fatal_error() which sets CSTS.CFS and drops the connection.
// ===========================================================================
#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <wincrypt.h>

#include "nvmeof_auth.h"

#pragma comment(lib, "crypt32.lib")

namespace nvmeof_dhchap {

using nvmeof_auth::HASH_SHA256;
using nvmeof_auth::HASH_INVALID;
using nvmeof_auth::dhGroupFor;
using nvmeof_auth::DhGroup;
using nvmeof_auth::DhKey;

// ---- message layout constants (ref/linux_nvme.h + the report's byte maps) ----
static const uint8_t  kSecpDhchap       = 0xE9;   // NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER
static const uint8_t  kAuthTypeCommon   = 0x00;
static const uint8_t  kAuthTypeDhchap   = 0x01;
static const uint8_t  kMsgNegotiate     = 0x00;
static const uint8_t  kMsgChallenge     = 0x01;
static const uint8_t  kMsgReply         = 0x02;
static const uint8_t  kMsgSuccess1      = 0x03;
static const uint8_t  kMsgSuccess2      = 0x04;
static const uint8_t  kMsgFailure2      = 0xF0;
static const uint8_t  kMsgFailure1      = 0xF1;
static const uint8_t  kHashIdSha256     = 0x01;
static const uint8_t  kHashIdSha384     = 0x02;
static const uint8_t  kHashIdSha512     = 0x03;
static const uint8_t  kAuthIdDhchap     = 0x01;   // NVME_AUTH_DHCHAP_AUTH_ID

// Little-endian 32-bit store, used by both halves for seqnum and for the lengths
// inside the response concatenation.
static inline void putLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---- failure reason codes (ref/linux_nvme.h NVME_AUTH_DHCHAP_FAILURE_*) ----
// These are the values that travel in Failure1's rescode_exp byte.  They are the
// only diagnostic a peer gets, so a target that always answers 0x01 ("FAILED")
// is telling the truth but nothing else; the codes below are the ones this
// implementation can actually distinguish.
enum : uint8_t {
    kFailureFailed          = 0x01,   // FAILED: bad response, no key, ...
    kFailureNotUsable       = 0x02,   // NOT_USABLE: the configured key cannot be parsed
    kFailureHashUnusable    = 0x04,   // HASH_UNUSABLE
    kFailureDhgroupUnusable = 0x05,   // DHGROUP_UNUSABLE
    kFailureIncorrectPayload= 0x06,   // INCORRECT_PAYLOAD: short/malformed message
    kFailureIncorrectMessage= 0x07,   // INCORRECT_MESSAGE: wrong type or step
};

static const size_t   kNegotiateSize    = 8 + 64;   // header + one protocol descriptor
static const size_t   kCommonHeader     = 16;       // challenge/reply/success1 prefix
static const size_t   kMaxHashIds       = 30;
static const size_t   kMaxDhIds         = 30;
static const size_t   kMaxBuf           = 4096;     // CHAP_BUF_SIZE

// The literal the key transform appends, 17 bytes, no NUL.
static const char kFabricsTag[] = "NVMe-over-Fabrics";
static const size_t kFabricsTagLen = 17;

// Domain separation literals inside the response HMACs.
static const char kHostTag[]  = "HostHost";      // 8
static const char kCtrlTag[]  = "Controller";    // 10
static const size_t kHostTagLen = 8;
static const size_t kCtrlTagLen = 10;

// ---------------------------------------------------------------------------
//  CRC32 (IEEE, zlib-compatible)
// ---------------------------------------------------------------------------
// The key blob ends with LE32(crc) where the reference computes `~crc32(~0, key, len)`
// - i.e. the ordinary zlib CRC32 of the key bytes.  Checked against the standard
// "123456789" -> 0xCBF43926 vector in the self-test.
static inline uint32_t crc32Ieee(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
//  Key parsing: "DHHC-1:<hh>:<base64(secret || LE32(crc32(secret)))>[:]"
// ---------------------------------------------------------------------------
struct Key {
    bool     valid = false;
    uint8_t  hashId = HASH_INVALID;   // from the "hh" field
    uint8_t  secret[64] = {};
    size_t   secretLen = 0;
};

static inline bool base64Decode(const char* text, uint8_t* out, size_t outCap, size_t* outLen) {
    // nvme-cli writes "DHHC-1:hh:<base64>:" - WITH a trailing colon - and the decoder
    // must not see it: CryptStringToBinaryA stops at the first character it does not
    // recognise, so the whole key then fails to parse.  Cut at the first colon.
    char clean[256] = {};
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(clean); i++) {
        if (text[i] == ':') break;
        clean[i] = text[i];
    }
    DWORD n = 0;
    // CryptStringToBinaryA takes the OUTPUT BUFFER SIZE in *pcbBinary on the way in,
    // and it is also where the produced length comes back.  Passing 0 asks for a
    // zero-byte buffer, which fails - and the failure looks exactly like "this key is
    // not valid base64", which is what made a perfectly good key unparseable.
    DWORD cap = (DWORD)outCap;
    if (!CryptStringToBinaryA(clean, 0, CRYPT_STRING_BASE64, out, &cap, nullptr, nullptr)) return false;
    n = cap;
    if (n > outCap) return false;
    *outLen = n;
    return true;
}

// `text` may or may not end with ':'.  The target's sscanf is
// "DHHC-1:%hhd:%*s" (trailing colon optional); nvme-cli prints one.
static inline bool parseKey(const char* text, Key& k) {
    k = Key();
    if (!text) return false;
    if (strncmp(text, "DHHC-1:", 7) != 0) return false;
    char hh[8] = {};
    if (sscanf_s(text + 7, "%2s", hh, (unsigned)sizeof(hh)) != 1) return false;
    // "%2s" stops at the colon and NUL-terminates right there, so the colon has to be
    // looked for in the ORIGINAL string (checking hh[2] can never be true - that is
    // what made every valid key fail to parse).
    if (strlen(hh) != 2 || text[7 + 2] != ':') return false;
    uint8_t hashId = (uint8_t)strtoul(hh, nullptr, 16);
    const char* b64 = text + 7 + 3;
    uint8_t raw[128] = {};
    size_t rawLen = 0;
    if (!base64Decode(b64, raw, sizeof(raw), &rawLen)) return false;
    if (rawLen < 4) return false;
    size_t secretLen = rawLen - 4;
    if (secretLen != 32 && secretLen != 48 && secretLen != 64) return false;
    uint32_t crcOnWire = (uint32_t)raw[secretLen] | ((uint32_t)raw[secretLen + 1] << 8) |
                         ((uint32_t)raw[secretLen + 2] << 16) | ((uint32_t)raw[secretLen + 3] << 24);
    if (crc32Ieee(raw, secretLen) != crcOnWire) return false;
    k.hashId = (hashId == 0) ? HASH_INVALID : hashId;   // 0 means "use the raw key"
    memcpy(k.secret, raw, secretLen);
    k.secretLen = secretLen;
    k.valid = true;
    return true;
}

// Kt = HMAC_H(raw_key, NQN || "NVMe-over-Fabrics"), or the raw key when hh == 0.
static inline bool transformKey(const Key& k, uint8_t hashId,
                                const char* nqn, uint8_t* out, size_t* outLen) {
    if (!k.valid) return false;
    if (k.hashId == HASH_INVALID) {          // "DHHC-1:00:" - no transform
        memcpy(out, k.secret, k.secretLen);
        *outLen = k.secretLen;
        return true;
    }
    size_t nqnLen = strlen(nqn);
    uint8_t msg[256 + 32] = {};
    if (nqnLen + kFabricsTagLen > sizeof(msg)) return false;
    memcpy(msg, nqn, nqnLen);
    memcpy(msg + nqnLen, kFabricsTag, kFabricsTagLen);
    size_t hlen = nvmeof_auth::hashLen(hashId);
    if (!hlen) return false;
    if (!nvmeof_auth::hmac(hashId, k.secret, k.secretLen, msg, nqnLen + kFabricsTagLen, out)) return false;
    *outLen = hlen;
    return true;
}

// The 8-byte Failure1 payload, as a free function so that a caller which has no
// exchange to fail (a target with no key configured, which still has to answer the
// host's Auth Receive) can produce the same bytes.
//
//   byte 0    auth_type = 0x00 (common)
//   byte 1    auth_id   = 0xF1 (Failure1)
//   bytes 2..3 rsvd
//   bytes 4..5 t_id, the transaction id of the Negotiate that started this
//              exchange - 0 if there never was one, which is exactly what nvmet
//              echoes from a queue that has not seen a Negotiate
//   byte 6    rescode: 0x01 FAILED, the only value the reference ever sends
//   byte 7    rescode_exp: why
static inline void buildFailure1Msg(uint8_t* m, uint16_t tId, uint8_t exp) {
    if (!m) return;
    memset(m, 0, kCommonHeader);
    m[0] = kAuthTypeCommon;
    m[1] = kMsgFailure1;
    m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
    m[6] = kFailureFailed;             // rescode: always FAILED in the reference
    m[7] = exp;
}

// ---------------------------------------------------------------------------
//  The target's half of one exchange
// ---------------------------------------------------------------------------
//
// One instance per controller, used only on the admin queue.  The caller drives it:
//   onAuthSend(data, len)   -> may produce a message to hand back on the next
//                              Auth Receive (pending()), or mark the controller
//                              authenticated / failed
//   onAuthReceive(al)       -> fills `al` bytes (zero padded) with pending()
// ---------------------------------------------------------------------------
struct TargetAuth {
    // configuration
    Key      hostKey;                 // from dhchap_key
    Key      ctrlKey;                 // from dhchap_ctrl_key (unused: one-way only)
    uint8_t  hashId = kHashIdSha256;  // dhchap_hash, default SHA-256
    uint8_t  dhGroupId = nvmeof_auth::DHGROUP_2048;

    // per-exchange state
    bool     enabled = false;         // a host key is configured
    bool     authenticated = false;
    bool     failed = false;
    bool     expectSuccess2 = false;  // Success1 carried a controller response
    bool     gotSuccess2 = false;     // ...and the host confirmed it
    char     failWhy[160] = {};
    int      step = 0;                // 0 = expect Negotiate, 1 = expect Reply
    uint16_t tId = 0;
    uint32_t s1 = 0, s2 = 0;
    uint8_t  c1[64] = {};             // our challenge
    size_t   hlen = 0;
    DhKey    dh;
    bool     haveDh = false;
    uint8_t  pendingMsg[kMaxBuf] = {};
    size_t   pendingLen = 0;

    // context the caller supplies once
    char     hostNqn[256] = {};
    char     subsysNqn[256] = {};

    bool init(const char* hostKeyText, const char* hostNqn_, const char* subsysNqn_) {
        snprintf(hostNqn, sizeof(hostNqn), "%s", hostNqn_ ? hostNqn_ : "");
        snprintf(subsysNqn, sizeof(subsysNqn), "%s", subsysNqn_ ? subsysNqn_ : "");
        if (!hostKeyText || !*hostKeyText) { enabled = false; return true; }
        if (!parseKey(hostKeyText, hostKey)) {
            snprintf(failWhy, sizeof(failWhy), "the configured host key is not a valid DHHC-1 key");
            return false;
        }
        hashId = (hostKey.hashId != HASH_INVALID) ? hostKey.hashId : kHashIdSha256;
        hlen   = nvmeof_auth::hashLen(hashId);
        if (!hlen) { snprintf(failWhy, sizeof(failWhy), "unsupported hash in the key"); return false; }
        const DhGroup* g = dhGroupFor(dhGroupId);
        if (!g) { snprintf(failWhy, sizeof(failWhy), "unsupported DH group"); return false; }
        char why[128] = {};
        if (!nvmeof_auth::dhGenerate(*g, dh, why, sizeof(why))) {
            snprintf(failWhy, sizeof(failWhy), "DH key generation: %s", why);
            return false;
        }
        haveDh  = true;
        enabled = true;
        return true;
    }

    void fail(const char* why) {
        failed = true;
        snprintf(failWhy, sizeof(failWhy), "%s", why);
        pendingLen = 0;
    }

    // The Failure1 message the host reads on its next Auth Receive.
    void buildFailure1(uint8_t exp) {
        buildFailure1Msg(pendingMsg, tId, exp);
        pendingLen = 8;
    }

    // ---- the host's Auth Send ------------------------------------------------
    //
    // auth_type is NOT the same for every message, and getting it wrong is
    // invisible between two copies of the same mistake: the NEGOTIATE, FAILURE1 and
    // FAILURE2 are COMMON messages (0x00) while CHALLENGE, REPLY, SUCCESS1 and
    // SUCCESS2 are DH-HMAC-CHAP messages (0x01).  The first version of this
    // function required 0x01 from the Negotiate and its self-test sent 0x01, so
    // both ends agreed and neither matched the reference - the §8.29 pattern,
    // found here by the host half this file grew later (the hand-written test used
    // the same wrong byte, which is why it passed).
    bool onAuthSend(const uint8_t* data, size_t len) {
        if (!enabled || failed) return true;
        if (len < 2) { fail("auth payload too short"); buildFailure1(kFailureIncorrectPayload); return true; }
        uint8_t type = data[0], id = data[1];
        if (type == kAuthTypeCommon) {
            // A Negotiate at any step restarts the exchange, which is what nvmet
            // does (`if (data->auth_id == NEGOTIATE) ... reset negotiation`).
            if (id == kMsgNegotiate) return onNegotiate(data, len);
            if (id == kMsgFailure2) {
                fail("the host sent Failure2");
                return true;
            }
            fail("unexpected common auth_id");
            buildFailure1(kFailureIncorrectMessage);
            return true;
        }
        if (type != kAuthTypeDhchap) { fail("unexpected auth_type"); buildFailure1(kFailureIncorrectMessage); return true; }
        if (id == kMsgReply)     return onReply(data, len);
        if (id == kMsgSuccess2)  return onSuccess2(data, len);
        fail("unexpected auth_id for this step");
        buildFailure1(kFailureIncorrectMessage);
        return true;
    }

    // SUCCESS2: the host confirming that IT verified the controller's response.
    // 16 bytes and no HMAC (there is nothing left to prove), and it is what makes
    // the queue authenticated in the reference - the host's Success1 alone does not
    // (nvmet_execute_auth_send, case NVME_AUTH_DHCHAP_MESSAGE_SUCCESS2).
    //
    // It must not arrive out of nowhere: a host that sends it without a controller
    // key is claiming to have verified something it never asked for.
    bool onSuccess2(const uint8_t* d, size_t len) {
        if (!expectSuccess2) {
            fail("Success2 arrived but no controller response was ever sent");
            buildFailure1(kFailureIncorrectMessage);
            return true;
        }
        if (len < kCommonHeader) {
            fail("Success2 is shorter than its 16-byte header");
            buildFailure1(kFailureIncorrectPayload);
            return true;
        }
        if (((uint16_t)d[4] | ((uint16_t)d[5] << 8)) != tId) {
            fail("Success2 carries a different transaction id");
            buildFailure1(kFailureIncorrectPayload);
            return true;
        }
        authenticated = true;
        gotSuccess2 = true;
        return true;
    }

    // NEGOTIATE: the target picks the hash and the DH group (the reference's own
    // selection loop lets its configured group win) and answers with a CHALLENGE.
    bool onNegotiate(const uint8_t* d, size_t len) {
        if (len < kNegotiateSize) { fail("negotiate payload too short"); buildFailure1(kFailureIncorrectPayload); return true; }
        tId = (uint16_t)d[4] | ((uint16_t)d[5] << 8);
        uint8_t napd = d[7];
        if (napd != 1) { fail("napd != 1"); buildFailure1(kFailureHashUnusable); return true; }
        const uint8_t* desc = d + 8;
        uint8_t authid = desc[0], halen = desc[2], dhlen = desc[3];
        if (authid != 0x01)      { fail("authid != 1"); buildFailure1(kFailureIncorrectPayload); return true; }
        if (halen > kMaxHashIds) { fail("halen > 30"); buildFailure1(kFailureIncorrectPayload); return true; }
        if (dhlen > kMaxDhIds)   { fail("dhlen > 30"); buildFailure1(kFailureIncorrectPayload); return true; }
        const uint8_t* hashes = desc + 4;              // idlist[0..29]
        // The DH list is at idlist[30..59]; the reference's selection loop lets the
        // TARGET's configured group win, so the host's list is only validated here
        // (its length was checked above) - which is what the report calls out as
        // gotcha #6.
        const uint8_t* groups = desc + 4 + 30;         // idlist[30..59]
        (void)groups;

        // Hash choice: the reference takes the first host entry it can use, falling
        // back to the first entry with a valid hash length.
        bool hashOk = false;
        for (size_t i = 0; i < halen && i < kMaxHashIds; i++) {
            if (hashes[i] == hashId) { hashOk = true; break; }
        }
        if (!hashOk) {
            for (size_t i = 0; i < halen && i < kMaxHashIds; i++) {
                if (nvmeof_auth::hashLen(hashes[i])) { hashId = hashes[i]; hlen = nvmeof_auth::hashLen(hashId); hashOk = true; break; }
            }
        }
        if (!hashOk) { fail("no usable hash in the host's list"); buildFailure1(kFailureHashUnusable); return true; }

        // DH group: the target's configured group is the one used.
        const DhGroup* g = dhGroupFor(dhGroupId);
        if (!g) { fail("no usable DH group"); buildFailure1(kFailureDhgroupUnusable); return true; }
        if (!haveDh) { fail("no DH key pair"); buildFailure1(kFailureDhgroupUnusable); return true; }

        // C1 fresh per queue, s1 a nonzero random sequence number.
        if (!NT_SUCCESS(BCryptGenRandom(nullptr, c1, (ULONG)hlen, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            fail("random for the challenge failed"); buildFailure1(kFailureFailed); return true;
        }
        do {
            if (!NT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&s1, sizeof(s1), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                fail("random for seqnum failed"); buildFailure1(kFailureFailed); return true;
            }
        } while (s1 == 0);

        uint8_t* m = pendingMsg;
        memset(m, 0, kCommonHeader);
        m[0] = kAuthTypeDhchap;
        m[1] = kMsgChallenge;
        m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
        m[6] = (uint8_t)hlen;
        m[8] = hashId;
        m[9] = dhGroupId;
        uint16_t dhvlen = (uint16_t)g->size;
        m[10] = (uint8_t)(dhvlen & 0xff); m[11] = (uint8_t)(dhvlen >> 8);
        m[12] = (uint8_t)(s1 & 0xff); m[13] = (uint8_t)((s1 >> 8) & 0xff);
        m[14] = (uint8_t)((s1 >> 16) & 0xff); m[15] = (uint8_t)((s1 >> 24) & 0xff);
        memcpy(m + kCommonHeader, c1, hlen);                  // cval
        memcpy(m + kCommonHeader + hlen, dh.pub, g->size);     // DH value at 16+h
        pendingLen = kCommonHeader + hlen + g->size;
        step = 1;
        return true;
    }

    // REPLY: verify the host's response, then either finish (one-way) or answer with
    // SUCCESS1 carrying the controller's own response.
    bool onReply(const uint8_t* d, size_t len) {
        if (step != 1) { fail("Reply before Negotiate"); buildFailure1(kFailureIncorrectPayload); return true; }
        const DhGroup* g = dhGroupFor(dhGroupId);
        if (!g) { fail("no DH group"); buildFailure1(kFailureDhgroupUnusable); return true; }
        size_t dhvlen = g->size;
        if (len < kCommonHeader + 2 * hlen + dhvlen) { fail("Reply too short"); buildFailure1(kFailureIncorrectPayload); return true; }
        if (d[6] != hlen) { fail("Reply hl mismatch"); buildFailure1(kFailureIncorrectPayload); return true; }
        uint16_t rdvlen = (uint16_t)d[10] | ((uint16_t)d[11] << 8);
        if (rdvlen != dhvlen) { fail("Reply DH length mismatch"); buildFailure1(kFailureIncorrectPayload); return true; }
        uint32_t s2r = (uint32_t)d[12] | ((uint32_t)d[13] << 8) |
                       ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
        const uint8_t* rval = d + kCommonHeader;
        const uint8_t* c2   = rval + hlen;
        const uint8_t* hostPub = c2 + hlen;
        uint8_t cvalid = d[8];

        // Ks = H(shared secret), the secret being exactly p_size bytes.
        uint8_t ks[64] = {}, shared[512] = {};
        char why[128] = {};
        if (!nvmeof_auth::dhShared(*g, dh, hostPub, shared, why, sizeof(why))) {
            fail("DH agreement failed"); buildFailure1(kFailureFailed); return true;
        }
        if (!nvmeof_auth::hash(hashId, shared, g->size, ks)) { fail("hash failed"); buildFailure1(kFailureFailed); return true; }
        // Ca1 = HMAC(Ks, C1); with a NULL group it would be C1 verbatim, and this
        // target does not offer that group.
        uint8_t ca1[64] = {};
        if (!nvmeof_auth::hmac(hashId, ks, hlen, c1, hlen, ca1)) { fail("hmac failed"); buildFailure1(kFailureFailed); return true; }

        // R1 = HMAC(Kt_host, Ca1 || LE32(s1) || LE16(t_id) || sc_c || "HostHost" ||
        //             hostnqn || 0x00 || subsysnqn)
        uint8_t ktHost[64] = {};
        size_t  ktHostLen = 0;
        if (!transformKey(hostKey, hashId, hostNqn, ktHost, &ktHostLen)) {
            fail("host key transform failed"); buildFailure1(kFailureFailed); return true;
        }
        uint8_t msg[64 + 4 + 2 + 1 + 16 + 256 + 1 + 256] = {};
        size_t  n = 0;
        memcpy(msg + n, ca1, hlen);                 n += hlen;
        msg[n++] = (uint8_t)(s1 & 0xff); msg[n++] = (uint8_t)((s1 >> 8) & 0xff);
        msg[n++] = (uint8_t)((s1 >> 16) & 0xff); msg[n++] = (uint8_t)((s1 >> 24) & 0xff);
        msg[n++] = (uint8_t)(tId & 0xff); msg[n++] = (uint8_t)(tId >> 8);
        msg[n++] = 0x00;                            // sc_c: no secure concatenation
        memcpy(msg + n, kHostTag, kHostTagLen);     n += kHostTagLen;
        size_t hn = strlen(hostNqn);  memcpy(msg + n, hostNqn, hn);  n += hn;
        msg[n++] = 0x00;
        size_t sn = strlen(subsysNqn); memcpy(msg + n, subsysNqn, sn); n += sn;
        uint8_t r1[64] = {};
        if (!nvmeof_auth::hmac(hashId, ktHost, ktHostLen, msg, n, r1)) {
            fail("hmac failed"); buildFailure1(kFailureFailed); return true;
        }
        if (memcmp(r1, rval, hlen) != 0) {
            fail("the host's response did not verify (wrong secret, or a bug here)");
            buildFailure1(kFailureFailed);
            return true;
        }

        // Host verified.  With no controller key this is a ONE-WAY exchange, but it is
        // still not over: the host is waiting for Success1 either way, and a target
        // that answers with nothing (which is what this did) makes the host see
        // auth_type 0x00 / auth_id 0x00, report INCORRECT_MESSAGE and give up.  nvmet
        // always builds Success1 - nvmet_auth_success1() fills rval only when it has a
        // controller response to put there, and leaves rvalid 0 otherwise.
        s2 = s2r;
        authenticated = true;
        {
            uint8_t* m = pendingMsg;
            memset(m, 0, kCommonHeader);
            m[0] = kAuthTypeDhchap;
            m[1] = kMsgSuccess1;
            m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
            m[6] = (uint8_t)hlen;
            m[8] = 0x00;                            // rvalid = 0
            pendingLen = kCommonHeader + hlen;      // the rval area is present, zeroed
        }
        if (!ctrlKey.valid) return true;

        // Bidirectional: R2 = HMAC(Kt_ctrl, Ca2 || LE32(s2) || LE16(t_id) || sc_c ||
        //                           "Controller" || subsysnqn || 0x00 || hostnqn)
        // Ca2 = HMAC(Ks, C2).  Linux only includes the controller response when it
        // sent cvalid=1 and a C2; otherwise rvalid stays 0.
        uint8_t ktCtrl[64] = {};
        size_t  ktCtrlLen = 0;
        if (!transformKey(ctrlKey, hashId, subsysNqn, ktCtrl, &ktCtrlLen)) {
            fail("controller key transform failed"); buildFailure1(kFailureFailed); return true;
        }
        uint8_t ca2[64] = {};
        if (!nvmeof_auth::hmac(hashId, ks, hlen, c2, hlen, ca2)) { fail("hmac failed"); buildFailure1(kFailureFailed); return true; }
        n = 0;
        memcpy(msg + n, ca2, hlen);                 n += hlen;
        msg[n++] = (uint8_t)(s2 & 0xff); msg[n++] = (uint8_t)((s2 >> 8) & 0xff);
        msg[n++] = (uint8_t)((s2 >> 16) & 0xff); msg[n++] = (uint8_t)((s2 >> 24) & 0xff);
        msg[n++] = (uint8_t)(tId & 0xff); msg[n++] = (uint8_t)(tId >> 8);
        msg[n++] = 0x00;
        memcpy(msg + n, kCtrlTag, kCtrlTagLen);     n += kCtrlTagLen;
        memcpy(msg + n, subsysNqn, sn);             n += sn;
        msg[n++] = 0x00;
        memcpy(msg + n, hostNqn, hn);               n += hn;
        uint8_t r2[64] = {};
        if (!nvmeof_auth::hmac(hashId, ktCtrl, ktCtrlLen, msg, n, r2)) {
            fail("hmac failed"); buildFailure1(kFailureFailed); return true;
        }
        uint8_t* m = pendingMsg;
        memset(m, 0, kCommonHeader);
        m[0] = kAuthTypeDhchap;
        m[1] = kMsgSuccess1;
        m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
        m[6] = (uint8_t)hlen;
        m[8] = (cvalid & 0x01);                     // rvalid
        memcpy(m + kCommonHeader, r2, hlen);
        pendingLen = kCommonHeader + hlen;
        // The host only confirms (Success2) when there was a controller response to
        // check, so the target must expect it under exactly the same condition.
        expectSuccess2 = (cvalid & 0x01) != 0;
        return true;
    }

    // ---- the host's Auth Receive --------------------------------------------
    // The reference always returns exactly `al` bytes, zero padded.
    size_t onAuthReceive(size_t al, uint8_t* out) {
        memset(out, 0, al);
        size_t n = pendingLen;
        if (n > al) n = al;
        if (n) memcpy(out, pendingMsg, n);
        pendingLen = 0;
        return al;
    }
};

// ---------------------------------------------------------------------------
//  The host's half of one exchange
// ---------------------------------------------------------------------------
//
// The host drives it and the target answers:
//
//   out = buildNegotiate()   -> Auth Send          (72 bytes)
//        <- Challenge                              (16 + h + dhvlen)
//   out = onChallenge(...)   -> Auth Send          (16 + 2h + dhvlen)
//        <- Success1 (rvalid=0 without a controller key, 1 with one)
//        onSuccess1(...)      -> optional Success2   (16 bytes, NO HMAC)
//
// The host is NOT the passive side: it picks the transaction id, it derives the
// first response (R1), and - with a controller key - it also verifies the target's
// own response (R2).  That last step is the only part of the exchange that proves
// the TARGET knows the controller secret, so it is implemented rather than
// skipped: a host that ignores rvalid authenticates a target it never checked.
struct HostAuth {
    // configuration
    Key      hostKey;
    Key      ctrlKey;                    // enables bidirectional authentication
    uint8_t  hashOverride = HASH_INVALID;   // --dhchap-hash; 0 = take it from the key
    uint8_t  preferredDhGroup = nvmeof_auth::DHGROUP_2048;
    bool     enabled = false;
    char     hostNqn[256] = {};
    char     subsysNqn[256] = {};

    // per-exchange state
    uint16_t tId = 0;
    uint8_t  hashId = 0;
    size_t   hlen = 0;
    uint8_t  dhGroupId = 0;
    uint32_t s1 = 0, s2 = 0;
    uint8_t  c1[64] = {};                // the target's challenge
    uint8_t  c2[64] = {};                // ours, when cvalid = 1
    uint8_t  r1[64] = {};                // what we sent as rval
    uint8_t  r2[64] = {};                // what the target must send back
    bool     bidirectional = false;
    bool     authenticated = false;
    bool     failed = false;
    char     failWhy[192] = {};
    int      step = 0;                   // 0 need Negotiate, 1 sent, 2 Success1 seen
    uint16_t failRescode = 0, failRescodeExp = 0;
    DhKey    dh;
    bool     haveDh = false;
    uint8_t  outMsg[kMaxBuf] = {};       // the next message to put in an Auth Send
    size_t   outLen = 0;

    bool init(const char* keyText, const char* hostNqn_, const char* subsysNqn_,
              const char* ctrlKeyText = nullptr) {
        snprintf(hostNqn, sizeof(hostNqn), "%s", hostNqn_ ? hostNqn_ : "");
        snprintf(subsysNqn, sizeof(subsysNqn), "%s", subsysNqn_ ? subsysNqn_ : "");
        if (!keyText || !*keyText) { enabled = false; return true; }
        if (!parseKey(keyText, hostKey)) {
            snprintf(failWhy, sizeof(failWhy), "the host key is not a valid DHHC-1 key");
            return false;
        }
        hashId = (hashOverride != HASH_INVALID) ? hashOverride : hostKey.hashId;
        if (hashId == HASH_INVALID) hashId = kHashIdSha256;
        hlen = nvmeof_auth::hashLen(hashId);
        if (!hlen) { snprintf(failWhy, sizeof(failWhy), "unsupported hash in the key"); return false; }
        if (ctrlKeyText && *ctrlKeyText) {
            if (!parseKey(ctrlKeyText, ctrlKey)) {
                snprintf(failWhy, sizeof(failWhy), "the controller key is not a valid DHHC-1 key");
                return false;
            }
            bidirectional = true;
        }
        // The transaction id is the host's: any non-zero 16-bit value, and it must
        // be echoed by both of the target's messages.
        uint16_t t = 0;
        if (!NT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&t, sizeof(t),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            snprintf(failWhy, sizeof(failWhy), "BCryptGenRandom failed");
            return false;
        }
        tId = t ? t : 0x1234;
        enabled = true;
        return true;
    }

    void fail(const char* why) {
        failed = true;
        snprintf(failWhy, sizeof(failWhy), "%s", why);
        outLen = 0;
    }

    // Failure2: 8 bytes, auth_type COMMON + auth_id 0xF0, the host telling the
    // target that IT decided the exchange failed.  No HMAC - like Success2, the
    // common messages carry none.
    void buildFailure2(uint8_t exp) {
        buildFailure1Msg(outMsg, tId, exp);      // same layout, different auth_id
        outMsg[1] = kMsgFailure2;
        outLen = 8;
    }

    // ---- 1. NEGOTIATE ---------------------------------------------------------
    bool buildNegotiate() {
        uint8_t* m = outMsg;
        memset(m, 0, kNegotiateSize);
        m[0] = kAuthTypeCommon;
        m[1] = kMsgNegotiate;
        m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
        m[6] = 0x00;                       // sc_c: no secure concatenation
        m[7] = 1;                          // napd
        m[8] = kAuthIdDhchap;              // auth_protocol[0].authid
        m[10] = 3;                         // halen
        m[11] = 6;                         // dhlen
        // The list order is the reference host's, and it matters: the target takes
        // the first entry that DIFFERS from its configured group, so a list that
        // leads with the group the target wants pushes it onto the next one.
        m[12] = kHashIdSha256; m[13] = kHashIdSha384; m[14] = kHashIdSha512;
        m[12 + kMaxHashIds + 0] = nvmeof_auth::DHGROUP_NULL;
        m[12 + kMaxHashIds + 1] = nvmeof_auth::DHGROUP_2048;
        m[12 + kMaxHashIds + 2] = nvmeof_auth::DHGROUP_3072;
        m[12 + kMaxHashIds + 3] = nvmeof_auth::DHGROUP_4096;
        m[12 + kMaxHashIds + 4] = 0x04;     // 6144
        m[12 + kMaxHashIds + 5] = 0x05;     // 8192
        outLen = kNegotiateSize;
        step = 1;
        return true;
    }

    // ---- 2. CHALLENGE -> REPLY ------------------------------------------------
    bool onChallenge(const uint8_t* d, size_t len) {
        if (step != 1) { fail("Challenge out of order"); buildFailure2(kFailureIncorrectMessage); return false; }
        if (len < kCommonHeader) { fail("Challenge shorter than its header"); buildFailure2(kFailureIncorrectPayload); return false; }
        if (d[0] != kAuthTypeDhchap || d[1] != kMsgChallenge) {
            fail("not a Challenge"); buildFailure2(kFailureIncorrectMessage); return false;
        }
        if (((uint16_t)d[4] | ((uint16_t)d[5] << 8)) != tId) {
            fail("the Challenge carries a different transaction id"); buildFailure2(kFailureIncorrectPayload); return false;
        }
        if (d[6] != hlen) { fail("the Challenge's hash length is not the one we offered"); buildFailure2(kFailureHashUnusable); return false; }
        hashId = d[8];
        hlen = nvmeof_auth::hashLen(hashId);
        if (!hlen) { fail("the Challenge names a hash we cannot do"); buildFailure2(kFailureHashUnusable); return false; }
        dhGroupId = d[9];
        uint16_t dhvlen = (uint16_t)d[10] | ((uint16_t)d[11] << 8);
        s1 = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
        if (len < kCommonHeader + hlen + dhvlen) {
            fail("the Challenge payload is shorter than hl + dhvlen");
            buildFailure2(kFailureIncorrectPayload);
            return false;
        }

        const DhGroup* g = dhGroupFor(dhGroupId);
        if (dhGroupId != nvmeof_auth::DHGROUP_NULL && !g) {
            fail("the Challenge names a DH group we do not have"); buildFailure2(kFailureDhgroupUnusable); return false;
        }
        if (dhGroupId == nvmeof_auth::DHGROUP_NULL && dhvlen != 0) {
            fail("a NULL DH group with a DH value"); buildFailure2(kFailureIncorrectPayload); return false;
        }

        memcpy(c1, d + kCommonHeader, hlen);
        const uint8_t* targetPub = d + kCommonHeader + hlen;

        // Ks = H(shared); without DH (NULL group) Ks is the transformed host key
        // itself, which is why a NULL group is "no DH", not "a DH of zero".
        uint8_t shared[512] = {}, ks[64] = {};
        char why[128] = {};
        if (g) {
            if (!nvmeof_auth::dhGenerate(*g, dh, why, sizeof(why))) {
                fail("DH key generation failed"); buildFailure2(kFailureDhgroupUnusable); return false;
            }
            haveDh = true;
            if (!nvmeof_auth::dhShared(*g, dh, targetPub, shared, why, sizeof(why))) {
                fail("DH agreement failed"); buildFailure2(kFailureDhgroupUnusable); return false;
            }
            if (!nvmeof_auth::hash(hashId, shared, g->size, ks)) {
                fail("hash failed"); buildFailure2(kFailureHashUnusable); return false;
            }
        } else {
            size_t klen = 0;
            if (!transformKey(hostKey, hashId, hostNqn, ks, &klen)) {
                fail("host key transform failed"); buildFailure2(kFailureNotUsable); return false;
            }
            (void)klen;
        }

        // Ca1 = HMAC(Ks, C1), and with a NULL group that is C1 itself.
        uint8_t ca1[64] = {};
        if (g) {
            if (!nvmeof_auth::hmac(hashId, ks, hlen, c1, hlen, ca1)) {
                fail("hmac failed"); buildFailure2(kFailureHashUnusable); return false;
            }
        } else {
            memcpy(ca1, c1, hlen);
        }

        uint8_t kt[64] = {};
        size_t  ktLen = 0;
        if (!transformKey(hostKey, hashId, hostNqn, kt, &ktLen)) {
            fail("host key transform failed"); buildFailure2(kFailureNotUsable); return false;
        }
        if (!hmacResponse(hashId, tId, kt, ktLen, ca1, s1, kHostTag, kHostTagLen,
                          hostNqn, subsysNqn, r1)) {
            fail("hmac failed"); buildFailure2(kFailureHashUnusable); return false;
        }

        // The Reply: 16 + h (rval) + h (cval, ALWAYS present) + dhvlen.  `cvalid`
        // and a C2 only when we have a controller key, and then we must also compute
        // the controller's expected response R2 so Success1 can be checked.
        uint8_t* m = outMsg;
        size_t need = kCommonHeader + 2 * hlen + (g ? g->size : 0);
        if (need > kMaxBuf) { fail("Reply does not fit the buffer"); buildFailure2(kFailureIncorrectPayload); return false; }
        memset(m, 0, need);
        m[0] = kAuthTypeDhchap;
        m[1] = kMsgReply;
        m[4] = (uint8_t)(tId & 0xff); m[5] = (uint8_t)(tId >> 8);
        m[6] = (uint8_t)hlen;
        memcpy(m + kCommonHeader, r1, hlen);
        if (bidirectional) {
            m[8] = 0x01;                                   // cvalid
            if (!NT_SUCCESS(BCryptGenRandom(nullptr, c2, (ULONG)hlen,
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                fail("random for C2 failed"); buildFailure2(kFailureFailed); return false;
            }
            memcpy(m + kCommonHeader + hlen, c2, hlen);
            uint8_t ca2[64] = {}, ktCtrl[64] = {};
            size_t  ktCtrlLen = 0;
            if (g) {
                if (!nvmeof_auth::hmac(hashId, ks, hlen, c2, hlen, ca2)) {
                    fail("hmac failed"); buildFailure2(kFailureHashUnusable); return false;
                }
            } else {
                memcpy(ca2, c2, hlen);
            }
            if (!transformKey(ctrlKey, hashId, subsysNqn, ktCtrl, &ktCtrlLen)) {
                fail("controller key transform failed"); buildFailure2(kFailureNotUsable); return false;
            }
            uint32_t s2rnd = 0;
            do {
                if (!NT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&s2rnd, sizeof(s2rnd),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                    fail("random for s2 failed"); buildFailure2(kFailureFailed); return false;
                }
            } while (s2rnd == 0);
            s2 = s2rnd;
            putLe32(m + 12, s2);
            if (!hmacResponse(hashId, tId, ktCtrl, ktCtrlLen, ca2, s2, kCtrlTag, kCtrlTagLen,
                              subsysNqn, hostNqn, r2)) {
                fail("hmac failed"); buildFailure2(kFailureHashUnusable); return false;
            }
        } else {
            m[8] = 0x00;
            putLe32(m + 12, 0);
        }
        if (g) {
            m[10] = (uint8_t)(g->size & 0xff); m[11] = (uint8_t)(g->size >> 8);
            memcpy(m + kCommonHeader + 2 * hlen, dh.pub, g->size);
        }
        outLen = need;
        step = 2;
        return true;
    }

    // ---- 3. SUCCESS1 (and the optional SUCCESS2) ------------------------------
    bool onSuccess1(const uint8_t* d, size_t len, bool* needSuccess2) {
        if (needSuccess2) *needSuccess2 = false;
        if (step != 2) { fail("Success1 out of order"); buildFailure2(kFailureIncorrectMessage); return false; }
        if (len < kCommonHeader + hlen) { fail("Success1 is too short"); buildFailure2(kFailureIncorrectPayload); return false; }
        if (d[0] != kAuthTypeDhchap || d[1] != kMsgSuccess1) {
            fail("not a Success1"); buildFailure2(kFailureIncorrectMessage); return false;
        }
        if (((uint16_t)d[4] | ((uint16_t)d[5] << 8)) != tId) {
            fail("Success1 carries a different transaction id"); buildFailure2(kFailureIncorrectPayload); return false;
        }
        if (d[6] != hlen) { fail("Success1's hash length is not ours"); buildFailure2(kFailureHashUnusable); return false; }
        if (d[8] & 0x01) {
            // rvalid: the target claims to have proved it holds the controller key.
            // A host that does not check this accepts a target that never did.
            if (!bidirectional) {
                fail("the target sent a controller response but we have no controller key");
                buildFailure2(kFailureFailed);
                return false;
            }
            if (memcmp(d + kCommonHeader, r2, hlen) != 0) {
                fail("the TARGET's response did not verify (wrong controller secret)");
                buildFailure2(kFailureFailed);
                return false;
            }
        } else if (bidirectional) {
            // Not fatal in the reference (rvalid is optional), but it means the
            // controller key we configured was never exercised.  Say so.
            printf("  [auth] note: the target sent no controller response (rvalid=0) "
                   "even though a controller key was configured\n");
        }
        authenticated = true;
        outLen = 0;
        step = 3;
        if (bidirectional) {
            // Success2: 16 bytes, no HMAC, and it is what tells the target the host
            // is satisfied.  nvmet marks the queue authenticated when it arrives.
            memset(outMsg, 0, kCommonHeader);
            outMsg[0] = kAuthTypeDhchap;
            outMsg[1] = kMsgSuccess2;
            outMsg[4] = (uint8_t)(tId & 0xff); outMsg[5] = (uint8_t)(tId >> 8);
            outLen = kCommonHeader;
            if (needSuccess2) *needSuccess2 = true;
        }
        return true;
    }

    // Failure1 arrives instead of a Challenge or a Success1.  One-way, so there is
    // no Failure1 handler in the reference host's chain: the rescode_exp is the only
    // diagnostic and the exchange is over (nvme_auth_receive_validate just copies it
    // into chap->status).
    bool onFailure1(const uint8_t* d, size_t len) {
        if (len < 8) { fail("Failure1 shorter than 8 bytes"); return false; }
        failRescode    = d[6];
        failRescodeExp = d[7];
        char why[160];
        sprintf_s(why, "the target answered Failure1 (rescode=0x%02X rescode_exp=0x%02X: %s)",
                  (unsigned)failRescode, (unsigned)failRescodeExp,
                  failureReasonName(failRescodeExp));
        fail(why);
        // The reference host answers a terminal failure with Failure2 before the
        // target tears the controller down (host/auth.c fail2:), so the target sees
        // a clean "both sides gave up" rather than a dropped connection.
        buildFailure2((uint8_t)failRescodeExp);
        return false;
    }

    // The name of a rescode_exp, for a log line that says something.
    static const char* failureReasonName(uint16_t exp) {
        switch (exp) {
        case kFailureFailed:           return "FAILED (a response did not verify, or no key)";
        case kFailureNotUsable:        return "NOT_USABLE (the configured key cannot be parsed)";
        case 0x03:                     return "CONCAT_MISMATCH (secure concatenation)";
        case kFailureHashUnusable:     return "HASH_UNUSABLE";
        case kFailureDhgroupUnusable:  return "DHGROUP_UNUSABLE";
        case kFailureIncorrectPayload: return "INCORRECT_PAYLOAD";
        case kFailureIncorrectMessage: return "INCORRECT_MESSAGE";
        default:                       return "unknown";
        }
    }

    // The order-dependent response HMAC.  Both directions use this one function
    // because the concatenation is identical apart from the two NQNs and the
    // domain-separation literal - writing it twice is how the host's and the
    // target's copies drift apart.
    //
    //   HMAC(Kt, Ca || LE32(seq) || LE16(t_id) || sc_c || tag || nqn1 || 0x00 || nqn2)
    //
    // The t_id is easy to drop and impossible to notice by reading: the HMAC is just
    // wrong, and the far end answers Failure1.  It was dropped here on the first
    // attempt, and the loopback test below is what caught it - the hand-written host
    // half in the older part of this file does include it, which is exactly why the
    // two had to be run against each other.
    static bool hmacResponse(uint8_t hashId_, uint16_t tId, const uint8_t* kt, size_t ktLen,
                             const uint8_t* ca, uint32_t seq, const char* tag,
                             size_t tagLen, const char* nqn1, const char* nqn2,
                             uint8_t* out) {
        uint8_t msg[64 + 4 + 2 + 1 + 16 + 256 + 1 + 256] = {};
        size_t n = 0;
        size_t hlen = nvmeof_auth::hashLen(hashId_);
        if (!hlen || hlen > 64) return false;
        memcpy(msg + n, ca, hlen);                       n += hlen;
        putLe32(msg + n, seq);                           n += 4;
        msg[n++] = (uint8_t)(tId & 0xff);                // LE16 t_id
        msg[n++] = (uint8_t)(tId >> 8);
        msg[n++] = 0x00;                                 // sc_c
        memcpy(msg + n, tag, tagLen);                    n += tagLen;
        size_t l1 = strlen(nqn1); memcpy(msg + n, nqn1, l1); n += l1;
        msg[n++] = 0x00;
        size_t l2 = strlen(nqn2); memcpy(msg + n, nqn2, l2); n += l2;
        return nvmeof_auth::hmac(hashId_, kt, ktLen, msg, n, out);
    }
};

// ---------------------------------------------------------------------------
//  Self-test: the pieces that a wrong guess would break
// ---------------------------------------------------------------------------
//
// The key parsing and the key transform are checked with an nvme-cli-shaped key that
// the test builds from a known secret (so the expected values come from the standard
// CRC32/base64 definitions, not from this code), and the state machine is driven
// through a full one-way exchange whose host half is computed here from the reference
// concatenations - a loopback, which is why interop still has to be run against a real
// host (DESIGN 8.51).
static inline int selfTest(int& failures) {
    auto check = [&](const char* what, bool ok, const char* detail) {
        printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
               (detail && *detail) ? " - " : "", (detail && *detail) ? detail : "");
        if (!ok) failures++;
    };

    // CRC32 against the standard check value.
    {
        const uint8_t v[] = { '1','2','3','4','5','6','7','8','9' };
        uint32_t c = crc32Ieee(v, sizeof(v));
        char d[64];
        sprintf_s(d, "0x%08X (want 0xCBF43926)", c);
        check("CRC32-IEEE check value", c == 0xCBF43926u, d);
    }

    // Key parsing: build "DHHC-1:01:<base64(secret||LE32(crc))>:" from a known secret.
    {
        uint8_t secret[32];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0xA0 + i);
        uint8_t raw[36];
        memcpy(raw, secret, 32);
        putLe32(raw + 32, crc32Ieee(secret, 32));
        char b64[128] = {};
        DWORD b64len = sizeof(b64);
        bool enc = CryptBinaryToStringA(raw, sizeof(raw),
                                        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len);
        char text[192] = {};
        sprintf_s(text, "DHHC-1:01:%s:", b64);
        Key k;
        bool parsed = enc && parseKey(text, k);
        if (!parsed) {
            // Say WHY: a silent "parse failed" on a key the test itself built is not
            // something to debug by staring at the parser.
            uint8_t probe[128] = {}; size_t plen = 0;
            bool b64ok = base64Decode(text + 7 + 3, probe, sizeof(probe), &plen);
            printf("         key text   : %s\n", text);
            printf("         prefix ok  : %d  b64 decode ok: %d (%zu bytes, want 36)\n",
                   (int)(strncmp(text, "DHHC-1:", 7) == 0), (int)b64ok, plen);
            if (b64ok && plen == 36) {
                printf("         crc on wire: 0x%08X  computed: 0x%08X\n",
                       (unsigned)((uint32_t)probe[32] | ((uint32_t)probe[33] << 8) |
                                  ((uint32_t)probe[34] << 16) | ((uint32_t)probe[35] << 24)),
                       crc32Ieee(probe, 32));
            }
        }
        check("a DHHC-1 key parses and its CRC verifies",
              parsed && k.valid && k.secretLen == 32 &&
                  memcmp(k.secret, secret, 32) == 0 && k.hashId == kHashIdSha256,
              parsed ? "" : "parse failed");
        // A corrupted CRC must be rejected: this is the field that catches typos in a
        // key a human typed, and accepting it would produce "wrong HMAC" later.
        raw[35] ^= 0xFF;
        b64len = sizeof(b64);
        CryptBinaryToStringA(raw, sizeof(raw), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len);
        sprintf_s(text, "DHHC-1:01:%s:", b64);
        Key bad;
        check("a key with a broken CRC is rejected", !parseKey(text, bad), nullptr);
    }

    // Full one-way exchange, host half computed here.
    {
        const char* hostNqn = "nqn.2014-08.org.nvmexpress:uuid:test-host";
        const char* subsysNqn = "nqn.2024-01.local.rdma:windows-nd";
        uint8_t secret[32];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(i * 7 + 1);
        uint8_t raw[36];
        memcpy(raw, secret, 32);
        putLe32(raw + 32, crc32Ieee(secret, 32));
        char b64[128] = {};
        DWORD b64len = sizeof(b64);
        CryptBinaryToStringA(raw, sizeof(raw), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len);
        char keyText[192] = {};
        sprintf_s(keyText, "DHHC-1:01:%s:", b64);

        TargetAuth ta;
        bool ok = ta.init(keyText, hostNqn, subsysNqn) && ta.enabled;
        // 1. host -> target: Negotiate (72 bytes, hash list {1,2,3}, group list {0,1,2,3,4,5})
        //    auth_type is COMMON (0x00) here, NOT the DH-HMAC-CHAP value: this byte
        //    was wrong in the first version of this test, and because the target
        //    checked it the same wrong way the test passed.  Pinned by an explicit
        //    check below rather than left to the state machine.
        uint8_t neg[72] = {};
        neg[0] = kAuthTypeCommon; neg[1] = kMsgNegotiate;
        neg[4] = 0x34; neg[5] = 0x12;                 // t_id = 0x1234
        neg[7] = 1;                                   // napd
        neg[8] = 0x01; neg[10] = 3; neg[11] = 6;      // authid, halen, dhlen
        neg[12] = 1; neg[13] = 2; neg[14] = 3;        // hash ids
        for (int i = 0; i < 6; i++) neg[12 + 30 + i] = (uint8_t)i;   // DH ids
        ok = ok && ta.onAuthSend(neg, sizeof(neg));
        uint8_t rbuf[kMaxBuf] = {};
        size_t got = ta.onAuthReceive(4096, rbuf);
        bool challengeOk = ok && got == 4096 && rbuf[0] == kAuthTypeDhchap &&
                           rbuf[1] == kMsgChallenge && rbuf[6] == 32 && rbuf[8] == kHashIdSha256 &&
                           rbuf[9] == nvmeof_auth::DHGROUP_2048;
        check("the target answers a Negotiate with a Challenge", challengeOk, nullptr);
        if (!challengeOk) { printf("  (state machine stopped here)\n"); return failures; }

        // Host side of the exchange, using the Challenge the target produced.
        uint16_t tId = (uint16_t)(rbuf[4] | (rbuf[5] << 8));
        uint32_t s1 = (uint32_t)rbuf[12] | ((uint32_t)rbuf[13] << 8) |
                      ((uint32_t)rbuf[14] << 16) | ((uint32_t)rbuf[15] << 24);
        const uint8_t* c1 = rbuf + kCommonHeader;
        const uint8_t* targetPub = c1 + 32;
        const DhGroup* g = dhGroupFor(ta.dhGroupId);
        uint8_t hostSecret[32];
        for (int i = 0; i < 32; i++) hostSecret[i] = (uint8_t)(0x40 + i);
        DhKey hostDh;
        char why[128] = {};
        bool hostOk = g && nvmeof_auth::dhGenerateWith(*g, hostSecret, hostDh, why, sizeof(why));
        uint8_t shared[512] = {}, ks[64] = {}, ca1[64] = {};
        hostOk = hostOk && nvmeof_auth::dhShared(*g, hostDh, targetPub, shared, why, sizeof(why)) &&
                 nvmeof_auth::hash(kHashIdSha256, shared, g->size, ks) &&
                 nvmeof_auth::hmac(kHashIdSha256, ks, 32, c1, 32, ca1);
        // Kt_host = HMAC(raw, hostnqn || "NVMe-over-Fabrics")
        uint8_t ktMsg[512] = {}, kt[64] = {};
        size_t hn = strlen(hostNqn);
        memcpy(ktMsg, hostNqn, hn);
        memcpy(ktMsg + hn, kFabricsTag, kFabricsTagLen);
        hostOk = hostOk && nvmeof_auth::hmac(kHashIdSha256, secret, 32, ktMsg,
                                             hn + kFabricsTagLen, kt);
        // R1 = HMAC(Kt, Ca1 || LE32(s1) || LE16(t_id) || sc_c || "HostHost" ||
        //           hostnqn || 0x00 || subsysnqn)
        uint8_t m2[1024] = {};
        size_t n = 0;
        memcpy(m2 + n, ca1, 32); n += 32;
        putLe32(m2 + n, s1);     n += 4;
        m2[n++] = (uint8_t)(tId & 0xff); m2[n++] = (uint8_t)(tId >> 8);
        m2[n++] = 0x00;
        memcpy(m2 + n, kHostTag, kHostTagLen); n += kHostTagLen;
        memcpy(m2 + n, hostNqn, hn);           n += hn;
        m2[n++] = 0x00;
        size_t sn = strlen(subsysNqn);
        memcpy(m2 + n, subsysNqn, sn);         n += sn;
        uint8_t r1[64] = {};
        hostOk = hostOk && nvmeof_auth::hmac(kHashIdSha256, kt, 32, m2, n, r1);

        uint8_t c2[32];
        for (int i = 0; i < 32; i++) c2[i] = (uint8_t)(0x90 + i);
        uint8_t reply[16 + 64 + 512] = {};
        reply[0] = kAuthTypeDhchap; reply[1] = kMsgReply;
        reply[4] = (uint8_t)(tId & 0xff); reply[5] = (uint8_t)(tId >> 8);
        reply[6] = 32; reply[8] = 0;                 // hl, cvalid = 0 (no controller key)
        uint16_t dhv = (uint16_t)g->size;
        reply[10] = (uint8_t)(dhv & 0xff); reply[11] = (uint8_t)(dhv >> 8);
        putLe32(reply + 12, 0x0BADF00D);             // s2: nobody checks it
        memcpy(reply + 16, r1, 32);
        memcpy(reply + 16 + 32, c2, 32);             // the 2*h area is always present
        memcpy(reply + 16 + 64, hostDh.pub, g->size);
        size_t replyLen = 16 + 64 + g->size;
        ok = hostOk && ta.onAuthSend(reply, replyLen);
        check("the target verifies a correct response and authenticates the host",
              ok && ta.authenticated && !ta.failed,
              ta.failed ? ta.failWhy : (hostOk ? "" : "the test's host half failed"));

        // The same exchange with one byte of the response flipped must fail.
        TargetAuth tb;
        tb.init(keyText, hostNqn, subsysNqn);
        tb.onAuthSend(neg, sizeof(neg));
        tb.onAuthReceive(4096, rbuf);
        reply[16] ^= 0x01;
        tb.onAuthSend(reply, replyLen);
        check("a tampered response is rejected with Failure1",
              tb.failed && tb.pendingLen == 8 && tb.pendingMsg[1] == kMsgFailure1 &&
                  tb.pendingMsg[6] == 0x01 && tb.pendingMsg[7] == 0x01,
              tb.failed ? tb.failWhy : "the tampered response was ACCEPTED");
    }

    // ---- both halves against each other -------------------------------------
    //
    // The hand-written host side above checks the TARGET against a fixed sequence.
    // This one checks the two implementations in this project against each other,
    // which is the weaker arrangement in general (§8.29/§8.46: two wrong halves that
    // agree) - but not here: every byte of both halves is derived from the same
    // reference concatenations, and the value of this pairing is that it exercises
    // the paths a fixed sequence cannot: the DH group the TARGET chose, a
    // bidirectional exchange, and Success2.
    {
        const char* hostNqn = "nqn.2014-08.org.nvmexpress:uuid:test-host";
        const char* subsysNqn = "nqn.2024-01.local.rdma:windows-nd";
        uint8_t secret[32], ctrlSecret[32];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(i * 5 + 3);
        for (int i = 0; i < 32; i++) ctrlSecret[i] = (uint8_t)(0xFF - i);
        auto mkKey = [](const uint8_t* s, char* out, size_t outCap) {
            uint8_t raw[36]; memcpy(raw, s, 32); putLe32(raw + 32, crc32Ieee(s, 32));
            char b64[128] = {}; DWORD n = sizeof(b64);
            CryptBinaryToStringA(raw, sizeof(raw), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &n);
            sprintf_s(out, outCap, "DHHC-1:01:%s:", b64);
        };
        char hostKeyText[192] = {}, ctrlKeyText[192] = {};
        mkKey(secret, hostKeyText, sizeof(hostKeyText));
        mkKey(ctrlSecret, ctrlKeyText, sizeof(ctrlKeyText));

        // The auth_type of each message, pinned.  The reference mixes the two
        // values across one exchange (Negotiate/Failure1/Failure2 are COMMON,
        // Challenge/Reply/Success1/Success2 are DH-HMAC-CHAP), and a pair of
        // endpoints that agree on the wrong one looks exactly like a working
        // implementation - until it meets nvme-cli.
        {
            HostAuth h;
            h.init(hostKeyText, hostNqn, subsysNqn);
            h.buildNegotiate();
            check("Negotiate carries auth_type COMMON (0x00), not DH-HMAC-CHAP",
                  h.outMsg[0] == kAuthTypeCommon && h.outMsg[1] == kMsgNegotiate, nullptr);
            h.buildFailure2(kFailureFailed);
            check("Failure2 carries auth_type COMMON (0x00)",
                  h.outMsg[0] == kAuthTypeCommon && h.outMsg[1] == kMsgFailure2 && h.outLen == 8,
                  nullptr);
        }

        // One-way: the target has the host key and no controller key.
        {
            TargetAuth t; HostAuth h;
            bool ok = t.init(hostKeyText, hostNqn, subsysNqn) &&
                      h.init(hostKeyText, hostNqn, subsysNqn);
            ok = ok && h.buildNegotiate() && t.onAuthSend(h.outMsg, h.outLen);
            uint8_t buf[kMaxBuf] = {};
            t.onAuthReceive(4096, buf);
            ok = ok && h.onChallenge(buf, 4096);
            ok = ok && h.outMsg[0] == kAuthTypeDhchap && h.outMsg[1] == kMsgReply;
            ok = ok && t.onAuthSend(h.outMsg, h.outLen);
            t.onAuthReceive(4096, buf);
            bool need2 = true;
            ok = ok && h.onSuccess1(buf, 4096, &need2);
            check("host and target agree on a one-way exchange, and both are done",
                  ok && h.authenticated && !h.failed && t.authenticated && !t.failed &&
                      !need2 && !h.bidirectional,
                  h.failed ? h.failWhy : (t.failed ? t.failWhy : ""));
        }
        // Bidirectional: the target holds both keys and must prove it holds the
        // controller one; the host must SEND Success2 for the queue to authenticate.
        {
            TargetAuth t; HostAuth h;
            bool ok = t.init(hostKeyText, hostNqn, subsysNqn);
            // TargetAuth::init only reads the host key; the controller key is
            // configured the same way nvmet does it - a separate key for the
            // subsystem NQN.
            ok = ok && parseKey(ctrlKeyText, t.ctrlKey);
            ok = ok && h.init(hostKeyText, hostNqn, subsysNqn, ctrlKeyText);
            ok = ok && h.buildNegotiate() && t.onAuthSend(h.outMsg, h.outLen);
            uint8_t buf[kMaxBuf] = {};
            t.onAuthReceive(4096, buf);
            ok = ok && h.onChallenge(buf, 4096);
            ok = ok && t.onAuthSend(h.outMsg, h.outLen);
            t.onAuthReceive(4096, buf);
            bool need2 = false;
            ok = ok && h.onSuccess1(buf, 4096, &need2);
            bool hostVerified = ok && h.authenticated && need2;
            ok = hostVerified && t.onAuthSend(h.outMsg, h.outLen);   // Success2
            check("a bidirectional exchange verifies the TARGET's response too",
                  ok && t.authenticated && t.gotSuccess2 && h.authenticated && !h.failed,
                  h.failed ? h.failWhy : (t.failed ? t.failWhy
                                       : (need2 ? "" : "the host did not build Success2")));
        }
        // And the same with the CONTROLLER secret wrong on the target: the host must
        // refuse, which is the whole point of checking rvalid.
        {
            TargetAuth t; HostAuth h;
            uint8_t wrong[32];
            for (int i = 0; i < 32; i++) wrong[i] = (uint8_t)(i + 1);
            char wrongKey[192] = {};
            mkKey(wrong, wrongKey, sizeof(wrongKey));
            bool ok = t.init(hostKeyText, hostNqn, subsysNqn) && parseKey(wrongKey, t.ctrlKey) &&
                      h.init(hostKeyText, hostNqn, subsysNqn, ctrlKeyText);
            ok = ok && h.buildNegotiate() && t.onAuthSend(h.outMsg, h.outLen);
            uint8_t buf[kMaxBuf] = {};
            t.onAuthReceive(4096, buf);
            ok = ok && h.onChallenge(buf, 4096);
            ok = ok && t.onAuthSend(h.outMsg, h.outLen);
            t.onAuthReceive(4096, buf);
            bool need2 = false;
            h.onSuccess1(buf, 4096, &need2);
            check("a target that holds the WRONG controller key is rejected by the host",
                  ok && h.failed && !h.authenticated && !need2,
                  h.failed ? h.failWhy : "the host accepted a controller response it should have rejected");
        }
    }
    return failures;
}

}   // namespace nvmeof_dhchap
