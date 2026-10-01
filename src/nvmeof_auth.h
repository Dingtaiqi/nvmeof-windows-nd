// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ===========================================================================
//  nvmeof_auth.h - the crypto primitives DH-HMAC-CHAP needs, and nothing else.
//
//  Deliberately separated from the protocol: the message order and the HMAC input
//  construction are easy to get wrong, and when a handshake fails it is impossible
//  to tell a wrong hash from a wrong message unless the primitives are known good
//  on their own.  `-authselftest` checks them against published test vectors
//  (FIPS 180-4, RFC 4231) and against a DH group whose answer is known by hand.
//
//  Everything here goes through Windows CNG (bcrypt.dll):
//    * SHA-256/384/512 and HMAC over them - `BCrypt*Hash` / `BCrypt*Hmac`
//    * Diffie-Hellman - CNG accepts CALLER-SUPPLIED parameters, which is what the
//      NVMe-oF DH groups need (they are not any of the groups CNG ships with), so
//      there is no big-integer code here at all: the modexp is done by CNG.
//
//  The alternative (hand-rolled modexp over 2048-bit numbers) is the kind of code
//  that is wrong in ways only an independent implementation can see, and we have
//  one: PowerShell's System.Numerics.BigInteger, used to cross-check the public
//  keys this file produces (see DESIGN 8.51).
// ===========================================================================
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

// The DH modexp.  CNG cannot do this DH (see nvmeof_bignum.h), so it is ours, and it
// is verified against BigInteger rather than against itself.
#include "nvmeof_bignum.h"

namespace nvmeof_auth {

// ---------------------------------------------------------------------------
//  Hashes
// ---------------------------------------------------------------------------
enum : uint8_t {
    HASH_SHA256 = 0x01,     // ref/linux_nvme.h: NVME_AUTH_HASH_SHA256
    HASH_SHA384 = 0x02,
    HASH_SHA512 = 0x03,
    HASH_INVALID = 0xff,
};

// Digest length for a hash id, or 0 when the id is not one we implement.
static inline size_t hashLen(uint8_t id) {
    switch (id) {
    case HASH_SHA256: return 32;
    case HASH_SHA384: return 48;
    case HASH_SHA512: return 64;
    default:          return 0;
    }
}

static inline LPCWSTR hashAlg(uint8_t id) {
    switch (id) {
    case HASH_SHA256: return BCRYPT_SHA256_ALGORITHM;
    case HASH_SHA384: return BCRYPT_SHA384_ALGORITHM;
    case HASH_SHA512: return BCRYPT_SHA512_ALGORITHM;
    default:          return nullptr;
    }
}

// One-shot digest.  Returns false if the hash id is unsupported or CNG failed.
static inline bool hash(uint8_t id, const uint8_t* data, size_t len, uint8_t* out) {
    LPCWSTR alg = hashAlg(id);
    size_t  need = hashLen(id);
    if (!alg || !need) return false;
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&h, alg, nullptr, 0))) return false;
    NTSTATUS s = BCryptHash(h, nullptr, 0,
                            (PUCHAR)data, (ULONG)len, out, (ULONG)need);
    BCryptCloseAlgorithmProvider(h, 0);
    return NT_SUCCESS(s);
}

// HMAC over the same hashes.  `key` may be any length (CNG hashes long keys and
// pads short ones exactly as RFC 2104 says, which is what the reference does too).
static inline bool hmac(uint8_t id, const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t len, uint8_t* out) {
    LPCWSTR alg = hashAlg(id);
    size_t  need = hashLen(id);
    if (!alg || !need) return false;
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&h, alg, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG))) return false;
    NTSTATUS s = BCryptHash(h, (PUCHAR)key, (ULONG)keyLen,
                            (PUCHAR)data, (ULONG)len, out, (ULONG)need);
    BCryptCloseAlgorithmProvider(h, 0);
    return NT_SUCCESS(s);
}

// ---------------------------------------------------------------------------
//  Diffie-Hellman with caller-supplied parameters
// ---------------------------------------------------------------------------
//
// All values are BIG-ENDIAN on the wire and in CNG blobs, and are `size` bytes
// wide exactly (leading zeros included) - the DH value in a Challenge/Reply is a
// fixed-length field, not a minimal-length integer.
struct DhGroup {
    const uint8_t* prime;     // big-endian, `size` bytes
    uint32_t       size;      // bytes
    uint32_t       generator; // small integer (2 for every NVMe-oF group)
};

// A key pair, in the form the wire needs: fixed-width BIG-ENDIAN values.
//
// The private exponent is kept (and exported from CNG) because the self-test checks
// the pair against an independent implementation: g^x mod p must equal the public
// value.  Without that check a wrong byte order inside CNG's blobs is invisible
// until a real handshake fails with "wrong HMAC".
struct DhKey {
    uint8_t  priv[512] = {};
    uint8_t  pub[512]  = {};
    uint32_t size = 0;
};

// Generate a key pair: pick x at random, then pub = g^x mod p with our own modexp.
//
// This does NOT use CNG's DH.  Measured (DESIGN 8.51): CNG answers
// STATUS_NOT_SUPPORTED for custom DH parameters, and STATUS_INVALID_PARAMETER when a
// private key blob's public value is not the correct g^x mod p - so the one operation
// the protocol needs is the one it refuses to do.  The modexp lives in
// nvmeof_bignum.h and is checked against BigInteger (src/run_authselftest.ps1).
static inline bool dhGenerateWith(const DhGroup& g, const uint8_t* priv,
                                  DhKey& k, char* why = nullptr, size_t whyLen = 0) {
    auto fail = [&](const char* what) {
        if (why && whyLen) sprintf_s(why, whyLen, "%s", what);
        return false;
    };
    if (!g.prime || !g.size || g.size > sizeof(k.pub) || !priv) return fail("bad group");
    uint8_t base[512] = {};
    base[g.size - 1] = (uint8_t)g.generator;             // g = 2, big-endian
    if (!nvmeof_bignum::modexp(base, (int)g.size, priv, (int)g.size, g.prime, k.pub))
        return fail("modexp failed");
    memcpy(k.priv, priv, g.size);
    k.size = g.size;
    return true;
}

// The same, with the private exponent from CNG's RNG.
static inline bool dhGenerate(const DhGroup& g, DhKey& k,
                              char* why = nullptr, size_t whyLen = 0) {
    if (!g.prime || !g.size || g.size > sizeof(k.pub)) {
        if (why && whyLen) sprintf_s(why, whyLen, "bad group");
        return false;
    }
    uint8_t x[512] = {};
    if (!NT_SUCCESS(BCryptGenRandom(nullptr, x, g.size, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        if (why && whyLen) sprintf_s(why, whyLen, "BCryptGenRandom failed");
        return false;
    }
    // A DH private exponent must be a real secret: force the top two bits so it is
    // large, and make it odd so it is not a power of two.
    x[0] |= 0xC0;
    x[g.size - 1] |= 0x01;
    return dhGenerateWith(g, x, k, why, whyLen);
}

// Shared secret: peerPub^x mod p, `size` bytes big-endian, left-zero-padded.
//
// The padding is not cosmetic: the reference computes the shared secret with
// crypto_kpp_compute_shared_secret into a buffer of exactly p_size bytes and hashes
// ALL of them (mpi_write_to_sgl fills leading bytes with zero), so stripping leading
// zeros - which every bignum library does by default - produces a different Ks and a
// handshake that fails with "bad HMAC" (DESIGN 8.51, trap #1).
static inline bool dhShared(const DhGroup& g, const DhKey& mine,
                            const uint8_t* peerPub, uint8_t* out,
                            char* why = nullptr, size_t whyLen = 0) {
    if (!g.prime || !g.size || !peerPub || !out || mine.size != g.size) {
        if (why && whyLen) sprintf_s(why, whyLen, "bad arguments");
        return false;
    }
    if (!nvmeof_bignum::modexp(peerPub, (int)g.size, mine.priv, (int)g.size, g.prime, out)) {
        if (why && whyLen) sprintf_s(why, whyLen, "modexp failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  The groups NVMe-oF actually uses
// ---------------------------------------------------------------------------
//
// ref/linux_nvme.h: NVME_AUTH_DHGROUP_2048 = 1, _3072 = 2, _4096 = 3, generator 2.
// The primes come from src/nvmeof_dhgroups.h, generated and verified by
// src/gen_dhgroups.ps1 from RFC 7919 (the negotiated FFDHE groups).
#include "nvmeof_dhgroups.h"

enum : uint8_t {
    DHGROUP_NULL   = 0x00,
    DHGROUP_2048   = 0x01,
    DHGROUP_3072   = 0x02,
    DHGROUP_4096   = 0x03,
    DHGROUP_INVALID = 0xff,
};

static inline const DhGroup* dhGroupFor(uint8_t id) {
    static const DhGroup g2048 = { kFfdhe2048, kFfdhe2048Size, 2 };
    static const DhGroup g3072 = { kFfdhe3072, kFfdhe3072Size, 2 };
    static const DhGroup g4096 = { kFfdhe4096, kFfdhe4096Size, 2 };
    switch (id) {
    case DHGROUP_2048: return &g2048;
    case DHGROUP_3072: return &g3072;
    case DHGROUP_4096: return &g4096;
    default:           return nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Self-test: published vectors, plus the one thing a vector cannot check
// ---------------------------------------------------------------------------
static inline bool toHex(const uint8_t* b, size_t n, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
    return true;
}

// Print one hex value per call.  The first version filled ONE buffer inside a single
// printf with four comma-operator arguments, so every %s pointed at the same buffer
// and all four values printed as whichever was evaluated last (they all read as zeros,
// which is how this was found).
static inline void printHexValue(const char* name, const uint8_t* v, size_t n) {
    char buf[2 * 512 + 1];
    toHex(v, n, buf);
    printf("  [dh] %s=%s\n", name, buf);
}

static inline int selfTest() {
    int failures = 0;
    auto check = [&](const char* what, const char* gotHex, const char* wantHex) {
        bool ok = (strcmp(gotHex, wantHex) == 0);
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) { printf("         got  %s\n         want %s\n", gotHex, wantHex); failures++; }
    };
    // Large enough for the biggest value printed here: 2 * 256 bytes + NUL.  It was
    // 200 bytes at first, and toHex then wrote 513 bytes into it - a stack overflow
    // that corrupted the neighbouring values and made the cross-check compare against
    // printed zeros.
    char hex[2 * 512 + 1];

    // FIPS 180-4 / NIST examples for "abc".
    {
        const uint8_t msg[] = { 'a', 'b', 'c' };
        uint8_t d[64];
        if (hash(HASH_SHA256, msg, 3, d)) { toHex(d, 32, hex);
            check("SHA-256(\"abc\")", hex,
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
        else { printf("  [FAIL] SHA-256 not available\n"); failures++; }
        if (hash(HASH_SHA384, msg, 3, d)) { toHex(d, 48, hex);
            check("SHA-384(\"abc\")", hex,
                  "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                  "8086072ba1e7cc2358baeca134c825a7"); }
        if (hash(HASH_SHA512, msg, 3, d)) { toHex(d, 64, hex);
            check("SHA-512(\"abc\")", hex,
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"); }
    }
    // RFC 4231 test case 1: key = 0x0b x20, data = "Hi There".
    {
        uint8_t key[20]; memset(key, 0x0b, sizeof(key));
        const uint8_t msg[] = { 'H','i',' ','T','h','e','r','e' };
        uint8_t d[64];
        if (hmac(HASH_SHA256, key, sizeof(key), msg, sizeof(msg), d)) {
            toHex(d, 32, hex);
            check("HMAC-SHA-256 (RFC 4231 case 1)", hex,
                  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
        } else { printf("  [FAIL] HMAC-SHA-256 not available\n"); failures++; }
        if (hmac(HASH_SHA512, key, sizeof(key), msg, sizeof(msg), d)) {
            toHex(d, 64, hex);
            check("HMAC-SHA-512 (RFC 4231 case 1)", hex,
                  "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
                  "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
        }
    }
    // Diffie-Hellman with the REAL group: DHGROUP_2048 is the RFC 7919 ffdhe2048
    // group, and CNG validates the modulus (it insists on a safe prime - which is why
    // an arbitrary 2048-bit prime was refused earlier, and why these fetched groups
    // are the right thing to test with rather than a made-up one).
    //
    // CNG picks the private exponent, so the check is on the RELATIONSHIP: the
    // self-test prints priv/pub/shared as `[dh]` lines and src/run_authselftest.ps1
    // recomputes g^x mod p and pub^x mod p with .NET BigInteger - an implementation
    // that shares no code with CNG.  A byte-order mistake anywhere in the blobs fails
    // there instead of showing up later as "wrong HMAC".
    {
        const DhGroup* g = dhGroupFor(DHGROUP_2048);
        DhKey A, B;
        uint8_t sA[512] = {}, sB[512] = {};
        char why[128] = {};
        // Fixed exponents so the PowerShell side can recompute everything:
        // xA = 2^200 + 12345, xB = 2^248 + 6789 (big-endian, 256 bytes).
        uint8_t xA[256] = {}, xB[256] = {};
        xA[255 - 200 / 8] |= (uint8_t)(1u << (200 % 8)); xA[254] = 0x30; xA[255] = 0x39;
        xB[255 - 248 / 8] |= (uint8_t)(1u << (248 % 8)); xB[254] = 0x1A; xB[255] = 0x85;
        bool ok = g && g->size == 256 &&
                  dhGenerateWith(*g, xA, A, why, sizeof(why)) &&
                  dhGenerateWith(*g, xB, B, why, sizeof(why));
        if (ok) {
            // Did CNG compute g^x mod p, or hand back what it was given?  The
            // PowerShell check decides; printing the values is what lets it.
            printf("  [dh] group=ffdhe2048 size=%u\n", (unsigned)g->size);
            printHexValue("privA", A.priv, 256);
            printHexValue("pubA",  A.pub,  256);
            printHexValue("privB", B.priv, 256);
            printHexValue("pubB",  B.pub,  256);
        } else { printf("  [FAIL] DH key generation failed: %s\n", why); failures++; }
        ok = ok && dhShared(*g, A, B.pub, sA, why, sizeof(why)) &&
                   dhShared(*g, B, A.pub, sB, why, sizeof(why));
        if (ok) printHexValue("shared", sA, 256);
        else printf("  [FAIL] DH agreement failed: %s\n", why);
        check("DH: both sides derived the same secret",
              (ok && memcmp(sA, sB, 256) == 0) ? "equal" : "DIFFERENT", "equal");
    }
    printf("  (src/run_authselftest.ps1 recomputes the [dh] values with BigInteger)\n");
    printf("\nauth self-test failures: %d\n", failures);
    return failures;
}

}   // namespace nvmeof_auth
