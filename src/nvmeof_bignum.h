// ===========================================================================
//  nvmeof_bignum.h - fixed-size big-endian modular exponentiation, and nothing else.
//
//  Why this exists at all: Windows CNG cannot do the DH that NVMe-oF requires.
//  Measured (DESIGN 8.51):
//    * BCryptSetProperty(hAlg, BCRYPT_DH_PARAMETERS, ...) on the DH provider answers
//      STATUS_NOT_SUPPORTED (0xC00000BB) - there is no way to install a custom group;
//    * BCryptImportKeyPair(BCRYPT_DH_PRIVATE_BLOB) with a caller-supplied exponent
//      answers STATUS_INVALID_PARAMETER (0xC000000D) when the public value in the
//      blob is not the correct g^x mod p, i.e. it validates the pair - so the one
//      operation CNG would have to do for us is the one it insists we already did.
//
//  So: Montgomery arithmetic over 32-bit limbs, which is the standard way to do a
//  modular exponentiation without a division routine.  It is verified against an
//  independent implementation (PowerShell System.Numerics.BigInteger, see
//  src/run_authselftest.ps1) for the exact group the protocol uses - not against
//  itself, and not against a toy group.
// ===========================================================================
#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <bcrypt.h>

namespace nvmeof_bignum {

// Enough for the largest group NVMe-oF defines (8192 bits = 256 limbs).
static const int kMaxLimbs = 256;

struct Big {
    uint32_t v[kMaxLimbs];   // little-endian limbs
    int      n;              // limbs in use (the modulus's size fixes this)
};

static inline void setZero(Big& a, int n) { memset(a.v, 0, sizeof(uint32_t) * n); a.n = n; }

// Big-endian bytes -> limbs.  Fixed width: the caller states how many bytes belong to
// the value, and leading zeros are part of it (a DH value is a fixed-width field).
static inline void fromBytes(Big& a, const uint8_t* be, int bytes, int limbs) {
    setZero(a, limbs);
    for (int i = 0; i < bytes; i++) {
        int bitIndex = (bytes - 1 - i) * 8;
        int limb = bitIndex / 32, sh = bitIndex % 32;
        if (limb < limbs) a.v[limb] |= (uint32_t)be[i] << sh;
    }
}

static inline void toBytes(const Big& a, uint8_t* be, int bytes) {
    memset(be, 0, bytes);
    for (int i = 0; i < bytes; i++) {
        int bitIndex = (bytes - 1 - i) * 8;
        int limb = bitIndex / 32, sh = bitIndex % 32;
        uint32_t val = (limb < a.n) ? (a.v[limb] >> sh) : 0;
        if (sh && limb + 1 < a.n) val |= a.v[limb + 1] << (32 - sh);
        be[i] = (uint8_t)(val & 0xff);
    }
}

static inline int cmp(const Big& a, const Big& b) {
    for (int i = a.n - 1; i >= 0; i--) {
        if (a.v[i] != b.v[i]) return (a.v[i] > b.v[i]) ? 1 : -1;
    }
    return 0;
}

static inline bool isZero(const Big& a) {
    for (int i = 0; i < a.n; i++) if (a.v[i]) return false;
    return true;
}

// a -= b, requires a >= b.
static inline void subInPlace(Big& a, const Big& b) {
    uint64_t borrow = 0;
    for (int i = 0; i < a.n; i++) {
        uint64_t bi = (i < b.n) ? b.v[i] : 0;
        uint64_t t = (uint64_t)a.v[i] - bi - borrow;
        a.v[i] = (uint32_t)t;
        borrow = (t >> 32) & 1;
    }
    (void)borrow;   // caller guarantees a >= b
}

// a = a*2 mod m  (used only to build R and R^2 mod m without a division routine)
//
// The doubling is done into an n+1 limb temporary, because 2a can need the extra bit
// (a < m and m can be just under 2^(32n)).  The first version compared the top BIT of
// the reduced value to decide whether the subtraction had "underflowed" - which is not
// what that bit means, and for a modulus like ffdhe2048 (top bit set) it threw the
// reduction away almost every time, so every modexp returned garbage.
static inline void dblMod(Big& a, const Big& m) {
    uint32_t t[kMaxLimbs + 1] = {};
    uint32_t carry = 0;
    for (int i = 0; i < m.n; i++) {
        t[i] = (a.v[i] << 1) | carry;
        carry = a.v[i] >> 31;
    }
    t[m.n] = carry;
    // 2a < 2m, so at most one subtraction is needed.  When the carry is set the true
    // value is t + 2^(32n), which is certainly >= m, and the n-limb result of the
    // subtraction is still correct modulo 2^(32n).
    bool ge = (carry != 0);
    if (!ge) {
        ge = true;
        for (int i = m.n - 1; i >= 0; i--) {
            if (t[i] != m.v[i]) { ge = (t[i] > m.v[i]); break; }
        }
    }
    if (ge) {
        uint64_t borrow = 0;
        for (int i = 0; i < m.n; i++) {
            uint64_t x = (uint64_t)t[i] - m.v[i] - borrow;
            t[i] = (uint32_t)x;
            borrow = (x >> 32) & 1;
        }
        // borrow must be 0 here: 2a - m < m <= 2^(32n)
    }
    memcpy(a.v, t, sizeof(uint32_t) * m.n);
}

// out = a*b (schoolbook), out has 2n limbs.
static inline void mulFull(const Big& a, const Big& b, uint32_t* out, int n) {
    memset(out, 0, sizeof(uint32_t) * 2 * n);
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t t = (uint64_t)a.v[i] * b.v[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        int k = i + n;
        while (carry && k < 2 * n) {
            uint64_t t = (uint64_t)out[k] + carry;
            out[k] = (uint32_t)t;
            carry = t >> 32;
            k++;
        }
    }
}

// Montgomery context: n0inv = -m^-1 mod 2^32.
struct Mont {
    Big      m;
    uint32_t n0inv = 0;
    int      n = 0;

    // m must be odd (every DH prime is).
    bool init(const Big& modulus) {
        m = modulus;
        n = modulus.n;
        uint32_t inv = 1;
        for (int i = 0; i < 5; i++) inv *= 2u - m.v[0] * inv;   // Newton: x_{k+1} = x*(2-mx)
        n0inv = (uint32_t)(0u - inv);                            // -m^-1 mod 2^32
        return (m.v[0] & 1u) != 0;
    }

    // out = a*b*R^-1 mod m   (R = 2^(32n)) - the classic "multiply then reduce" form.
    void mul(const Big& a, const Big& b, Big& out) const {
        uint32_t t[2 * kMaxLimbs + 1] = {};
        mulFull(a, b, t, n);
        t[2 * n] = 0;
        for (int i = 0; i < n; i++) {
            uint32_t m32 = (uint32_t)((uint64_t)t[i] * n0inv);
            uint64_t carry = 0;
            for (int j = 0; j < n; j++) {
                uint64_t x = (uint64_t)m32 * m.v[j] + t[i + j] + carry;
                t[i + j] = (uint32_t)x;
                carry = x >> 32;
            }
            int k = i + n;
            while (carry) {
                uint64_t x = (uint64_t)t[k] + carry;
                t[k] = (uint32_t)x;
                carry = x >> 32;
                k++;
            }
        }
        Big res;
        setZero(res, n);
        memcpy(res.v, t + n, sizeof(uint32_t) * n);
        // The value is < 2m (standard Montgomery bound), so one subtraction is enough
        // - and when the extra limb t[2n] is set the n-limb difference is still the
        // right value modulo 2^(32n).
        if (t[2 * n] || cmp(res, m) >= 0) subInPlace(res, m);
        out = res;
        out.n = n;
    }
};

// out = base^exp mod m, all fixed width `bytes` big-endian.
// `exp` may be shorter than the modulus; its length is `expBytes`.
static inline bool modexp(const uint8_t* baseBe, int bytes,
                          const uint8_t* expBe, int expBytes,
                          const uint8_t* modBe, uint8_t* outBe) {
    if (bytes <= 0 || bytes > kMaxLimbs * 4 || expBytes <= 0) return false;
    int n = (bytes + 3) / 4;
    Big m, a;
    fromBytes(m, modBe, bytes, n);
    if ((m.v[0] & 1u) == 0) return false;          // Montgomery needs an odd modulus
    Mont ctx;
    if (!ctx.init(m)) return false;
    fromBytes(a, baseBe, bytes, n);

    // R2 = 2^(64n) mod m, built by repeated doubling (no division routine needed).
    Big one, r2;
    setZero(one, n); one.v[0] = 1;
    r2 = one;
    for (int i = 0; i < 64 * n; i++) dblMod(r2, m);
    Big aMont;
    ctx.mul(a, r2, aMont);                          // a*R mod m

    // Square-and-multiply, MSB first.
    Big acc;
    setZero(acc, n); acc.v[0] = 1;                  // 1 in Montgomery form is R mod m...
    Big rModM = one;
    for (int i = 0; i < 32 * n; i++) dblMod(rModM, m);
    acc = rModM;                                    // ...which is R mod m: acc*acc*R^-1 = R
    for (int i = expBytes * 8 - 1; i >= 0; i--) {
        Big sq;
        ctx.mul(acc, acc, sq);
        acc = sq;
        if ((expBe[(expBytes - 1 - i / 8)] >> (i % 8)) & 1u) {
            Big pr;
            ctx.mul(acc, aMont, pr);
            acc = pr;
        }
    }
    Big res;
    Big oneMont;
    setZero(oneMont, n); oneMont.v[0] = 1;
    ctx.mul(acc, oneMont, res);                      // convert out of Montgomery form
    toBytes(res, outBe, bytes);
    return true;
}

}   // namespace nvmeof_bignum
