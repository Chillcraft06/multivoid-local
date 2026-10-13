// coop/net/signaling_crypto.cpp -- see signaling_crypto.h for the design.
//
// Primitive sources, so the audit trail is complete:
// - SHA-512 / RNG: Windows CNG (bcrypt), already linked for GNS's BCrypt backend -- the same
//   no-new-dependency rule peer_identity.cpp follows.
// - X25519: Montgomery ladder per RFC 7748 section 5, over a 5x51-bit field arithmetic written
//   for this file (schoolbook multiply with 128-bit intermediates, exact and reviewable).
// - Ed25519->X25519: libsodium's documented conversion (clamped SHA-512(seed) low half;
//   u = (1+y)/(1-y)).
// - ChaCha20-Poly1305: RFC 8439 sections 2.3, 2.5, 2.8.
// Every one is pinned to its RFC vector in RunSelftest below. This code is NOT written for
// side-channel hardness (no secret-dependent branches are avoided beyond the ladder's
// cswap); the threat model is a network observer, not a local one.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "signaling_crypto.h"

#include "coop/net/peer_identity.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::net::signaling_crypto {
namespace {

// ---------------------------------------------------------------------------
// SHA-256 / SHA-512 via CNG (mirrors peer_identity.cpp's Sha256).
// ---------------------------------------------------------------------------

bool Sha512(const void* data, size_t len, uint8_t out[64]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok)
        ok = ::BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                              static_cast<ULONG>(len), 0) == 0;
    if (ok) ok = ::BCryptFinishHash(hash, out, 64, 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool Sha256(const void* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok)
        ok = ::BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                              static_cast<ULONG>(len), 0) == 0;
    if (ok) ok = ::BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

void SecureZero(void* p, size_t n) { ::SecureZeroMemory(p, n); }

// ---------------------------------------------------------------------------
// fe25519: 5 limbs of 51 bits, each < 2^51 when reduced. p = 2^255 - 19.
// ---------------------------------------------------------------------------

struct Fe {
    uint64_t v[5]{};
};

constexpr uint64_t kMask51 = (UINT64_C(1) << 51) - 1;
// p's limbs (2^255 - 19): limb0 = 2^51 - 19, the rest 2^51 - 1.
constexpr uint64_t kP[5] = {(UINT64_C(1) << 51) - 19, (UINT64_C(1) << 51) - 1,
                            (UINT64_C(1) << 51) - 1, (UINT64_C(1) << 51) - 1,
                            (UINT64_C(1) << 51) - 1};

// h < 2p  ->  h mod p. Caller promises the bound.
void fe_sub_p_cond(Fe& h) {
    uint64_t d[5];
    uint64_t borrow = 0;
    for (int i = 0; i < 5; i++) {
        const uint64_t sub = kP[i] + borrow;
        d[i] = h.v[i] - sub;
        borrow = (h.v[i] < sub) ? 1 : 0;
    }
    // Not constant-time, and does not need to be (see the file header): the values here are
    // public points or the shared secret's intermediates, and the codebase makes no
    // side-channel claim anywhere in this lane.
    if (!borrow)
        for (int i = 0; i < 5; i++) h.v[i] = d[i];
}

void fe_carry(Fe& h) {
    uint64_t carry = 0;
    for (int i = 0; i < 5; i++) {
        const uint64_t sum = h.v[i] + carry;
        h.v[i] = sum & kMask51;
        carry = sum >> 51;
    }
}

void fe_add(Fe& h, const Fe& a, const Fe& b) {  // a, b < p
    uint64_t carry = 0;
    for (int i = 0; i < 5; i++) {
        const uint64_t sum = a.v[i] + b.v[i] + carry;
        h.v[i] = sum & kMask51;
        carry = sum >> 51;
    }
    fe_sub_p_cond(h);  // < 2p
}

void fe_sub(Fe& h, const Fe& a, const Fe& b) {  // a, b < p
    int64_t borrow = 0;
    for (int i = 0; i < 5; i++) {
        int64_t cur = static_cast<int64_t>(a.v[i]) - static_cast<int64_t>(b.v[i]) + borrow;
        if (cur < 0) {
            cur += static_cast<int64_t>(UINT64_C(1) << 51);
            borrow = -1;
        } else {
            borrow = 0;
        }
        h.v[i] = static_cast<uint64_t>(cur);
    }
    if (borrow) {  // add p back
        uint64_t carry = 0;
        for (int i = 0; i < 5; i++) {
            const uint64_t sum = h.v[i] + kP[i] + carry;
            h.v[i] = sum & kMask51;
            carry = sum >> 51;
        }
    }
}

void fe_mul(Fe& h, const Fe& a, const Fe& b) {  // a, b < p; h < p
    // Schoolbook into 128-bit accumulators: each product < 2^102, five per coefficient < 2^105.
    __uint128_t t[10] = {};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++) t[i + j] += (__uint128_t)a.v[i] * b.v[j];
    for (int i = 0; i < 9; i++) {
        t[i + 1] += t[i] >> 51;
        t[i] &= kMask51;
    }
    // Fold the high half with 2^255 == 19 (mod p).
    for (int i = 5; i < 10; i++) {
        t[i - 5] += (__uint128_t)19 * t[i];
        t[i] = 0;
    }
    for (int i = 0; i < 5; i++) {
        t[i + 1] += t[i] >> 51;
        t[i] &= kMask51;
    }
    t[0] += (__uint128_t)19 * t[5];
    t[5] = 0;
    for (int i = 0; i < 5; i++) {
        t[i + 1] += t[i] >> 51;
        t[i] &= kMask51;
    }
    t[0] += (__uint128_t)19 * t[5];
    uint64_t carry = 0;
    for (int i = 0; i < 5; i++) {
        const __uint128_t s = t[i] + carry;
        h.v[i] = (uint64_t)(s & kMask51);
        carry = (uint64_t)(s >> 51);
    }
    fe_sub_p_cond(h);  // < 2^255 + 19*2^13 < 2p
}

void fe_sq(Fe& h, const Fe& a) { fe_mul(h, a, a); }

// h = a * n, n < 2^20. Used for the ladder's 121665 * E.
void fe_mul_small(Fe& h, const Fe& a, uint64_t n) {
    __uint128_t carry = 0;
    for (int i = 0; i < 5; i++) {
        carry += (__uint128_t)a.v[i] * n;
        h.v[i] = (uint64_t)(carry & kMask51);
        carry >>= 51;
    }
    h.v[0] += (uint64_t)(19 * carry);  // 2^255 == 19; carry < 2^20
    carry = 0;
    for (int i = 0; i < 5; i++) {
        const __uint128_t s = h.v[i] + carry;
        h.v[i] = (uint64_t)(s & kMask51);
        carry = s >> 51;
    }
    fe_sub_p_cond(h);
}

void fe_cswap(Fe& a, Fe& b, uint64_t swap) {  // swap is 0 or 1
    const uint64_t mask = 0u - swap;
    for (int i = 0; i < 5; i++) {
        const uint64_t d = mask & (a.v[i] ^ b.v[i]);
        a.v[i] ^= d;
        b.v[i] ^= d;
    }
}

void fe_1(Fe& h) {
    h.v[0] = 1;
    h.v[1] = h.v[2] = h.v[3] = h.v[4] = 0;
}

bool fe_iszero(const Fe& a) {
    uint64_t acc = 0;
    for (int i = 0; i < 5; i++) acc |= a.v[i];
    return acc == 0;
}

// z^(2^255 - 21) = z^(p-2): the donna addition chain.
void fe_inv(Fe& out, const Fe& z) {
    Fe t0, t1;
    auto sq = [](Fe& h, const Fe& a, int n) {
        for (int i = 0; i < n; i++) fe_sq(h, i == 0 ? a : h);
    };
    fe_sq(t0, z);        // 2^1
    fe_sq(t1, t0);        // 2^2
    sq(t1, t1, 1);        // 2^3
    fe_mul(t1, z, t1);    // 2^3 - 2^0
    fe_mul(t0, t0, t1);   // 2^4 - 2^0
    fe_sq(t0, t0);        // 2^5 - 2^1
    fe_mul(t1, t1, t0);   // 2^5 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 4);        // 2^10 - 2^5
    fe_mul(t1, t1, t0);   // 2^10 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 9);        // 2^20 - 2^10
    fe_mul(t1, t1, t0);   // 2^20 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 19);       // 2^40 - 2^20
    fe_mul(t1, t1, t0);   // 2^40 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 39);       // 2^80 - 2^40
    fe_mul(t1, t1, t0);   // 2^80 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 79);       // 2^160 - 2^80
    fe_mul(t1, t1, t0);   // 2^160 - 2^0
    sq(t0, t1, 1);
    sq(t0, t0, 89);       // 2^250 - 2^90
    fe_mul(t1, t1, t0);   // 2^250 - 2^0
    sq(t0, t1, 5);        // 2^255 - 2^5
    fe_mul(out, t0, z);   // 2^255 - 21
}

// Load 32 little-endian bytes; bit 255 (the Ed25519 sign bit position) is folded via
// 2^255 == 19 so a u-coordinate with the high bit set still reduces correctly.
void fe_frombytes(Fe& h, const uint8_t s[32]) {
    uint8_t t[32];
    std::memcpy(t, s, 32);
    const uint64_t top = (t[31] >> 7) & 1u;
    t[31] &= 0x7f;
    uint64_t acc = 0;
    int accbits = 0, limb = 0;
    for (int i = 0; i < 32; i++) {
        acc |= (uint64_t)t[i] << accbits;
        accbits += 8;
        if (accbits >= 51 && limb < 5) {
            h.v[limb++] = acc & kMask51;
            acc >>= 51;
            accbits -= 51;
        }
    }
    h.v[limb] = acc;  // the last 51 bits
    h.v[0] += 19 * top;
    fe_carry(h);
    fe_sub_p_cond(h);
}

void fe_tobytes(uint8_t s[32], const Fe& h) {
    Fe t = h;
    fe_carry(t);
    fe_sub_p_cond(t);
    uint64_t acc = 0;
    int accbits = 0, k = 0;
    for (int i = 0; i < 5; i++) {
        acc |= t.v[i] << accbits;
        accbits += 51;
        while (accbits >= 8) {
            s[k++] = (uint8_t)(acc & 0xff);
            acc >>= 8;
            accbits -= 8;
        }
    }
    s[k] = (uint8_t)(acc & 0x7f);  // the last 7 bits; bit 255 is always clear
}

// ---------------------------------------------------------------------------
// X25519: RFC 7748 section 5 Montgomery ladder.
// ---------------------------------------------------------------------------

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]) {
    uint8_t k[32];
    std::memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    Fe x1, x2, z2, x3, z3, A, AA, B, BB, E, C, D, DA, CB, T;
    fe_frombytes(x1, u);
    fe_1(x2);
    std::memset(&z2, 0, sizeof z2);
    x3 = x1;
    fe_1(z3);

    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        const uint64_t kt = (k[t >> 3] >> (t & 7)) & 1u;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        fe_add(A, x2, z2);
        fe_sq(AA, A);
        fe_sub(B, x2, z2);
        fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);
        fe_add(T, DA, CB);
        fe_sq(x3, T);
        fe_sub(T, DA, CB);
        fe_sq(T, T);
        fe_mul(z3, x1, T);
        fe_mul(x2, AA, BB);
        fe_mul_small(T, E, 121665);
        fe_add(T, T, AA);
        fe_mul(z2, E, T);
    }
    Fe z2i;
    fe_inv(z2i, z2);
    fe_mul(x2, x2, z2i);
    fe_tobytes(out, x2);
    SecureZero(k, sizeof k);
}

// ---------------------------------------------------------------------------
// Ed25519 -> X25519 (libsodium's conversion).
// ---------------------------------------------------------------------------

// x-secret = clamp(SHA-512(seed)[0..32]).
bool ed25519_seed_to_x25519(uint8_t x[32], const uint8_t seed[32]) {
    uint8_t h[64];
    if (!Sha512(seed, 32, h)) return false;
    std::memcpy(x, h, 32);
    x[0] &= 248;
    x[31] &= 127;
    x[31] |= 64;
    SecureZero(h, sizeof h);
    return true;
}

// u = (1 + y) / (1 - y), y = the Ed25519 public key with the sign bit cleared.
bool ed25519_pub_to_x25519(uint8_t x[32], const uint8_t ed[32]) {
    uint8_t t[32];
    std::memcpy(t, ed, 32);
    t[31] &= 0x7f;
    Fe y, one, num, den, deni, u;
    fe_frombytes(y, t);
    fe_1(one);
    fe_add(num, one, y);
    fe_sub(den, one, y);
    if (fe_iszero(den)) return false;  // y == 1: degenerate, refuse
    fe_inv(deni, den);
    fe_mul(u, num, deni);
    fe_tobytes(x, u);
    return true;
}

// ---------------------------------------------------------------------------
// ChaCha20 (RFC 8439 section 2.3).
// ---------------------------------------------------------------------------

uint32_t RotL32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b;
    d ^= a;
    d = RotL32(d, 16);
    c += d;
    b ^= c;
    b = RotL32(b, 12);
    a += b;
    d ^= a;
    d = RotL32(d, 8);
    c += d;
    b ^= c;
    b = RotL32(b, 7);
}

uint32_t Load32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void Store32LE(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void ChaChaBlock(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                 uint8_t out[64]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) s[4 + i] = Load32LE(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = Load32LE(nonce + 4 * i);
    uint32_t w[16];
    std::memcpy(w, s, sizeof w);
    for (int i = 0; i < 10; i++) {
        QuarterRound(w[0], w[4], w[8], w[12]);
        QuarterRound(w[1], w[5], w[9], w[13]);
        QuarterRound(w[2], w[6], w[10], w[14]);
        QuarterRound(w[3], w[7], w[11], w[15]);
        QuarterRound(w[0], w[5], w[10], w[15]);
        QuarterRound(w[1], w[6], w[11], w[12]);
        QuarterRound(w[2], w[7], w[8], w[13]);
        QuarterRound(w[3], w[4], w[9], w[14]);
    }
    for (int i = 0; i < 16; i++) Store32LE(out + 4 * i, w[i] + s[i]);
}

// ---------------------------------------------------------------------------
// Poly1305 (RFC 8439 section 2.5): 5x32-bit limbs, radix 2^32; the accumulator stays
// loosely reduced between blocks and is fully reduced once at the end.
// ---------------------------------------------------------------------------

struct P1305 {
    uint32_t h[5]{};
};

// h = h * r mod (2^130 - 5). h may be loosely reduced; the result is loosely reduced.
void p1305_mul(P1305& h, const uint32_t r[4]) {
    uint64_t t[8] = {};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) t[i + j] += (uint64_t)h.h[i] * r[j];
    for (int i = 0; i < 7; i++) {
        t[i + 1] += t[i] >> 32;
        t[i] &= 0xffffffffu;
    }
    // Fold everything at and above 2^130: 2^130 == 5 (mod 2^130 - 5).
    // t[4] holds bits 128..; hi = t[4]>>2 covers bits 130+, plus 2^32*t[5] etc.
    __uint128_t hi = (__uint128_t)(t[4] >> 2) + ((__uint128_t)t[5] << 30) +
                    ((__uint128_t)t[6] << 62) + ((__uint128_t)t[7] << 94);
    t[4] &= 3u;
    __uint128_t c = 5 * hi;
    for (int i = 0; i < 4; i++) {
        c += t[i];
        t[i] = (uint64_t)(c & 0xffffffffu);
        c >>= 32;
    }
    t[4] += (uint64_t)c;
    // One more fold: t[4] may again reach past bit 130.
    hi = t[4] >> 2;
    t[4] &= 3u;
    t[0] += 5 * hi;
    for (int i = 0; i < 4; i++) {
        t[i + 1] += t[i] >> 32;
        t[i] &= 0xffffffffu;
    }
    for (int i = 0; i < 5; i++) h.h[i] = (uint32_t)t[i];
}

// Fully reduce h mod (2^130 - 5).
void p1305_reduce_full(P1305& h) {
    uint64_t t[5];
    for (int i = 0; i < 5; i++) t[i] = h.h[i];
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        t[i] += c;
        c = t[i] >> 32;
        t[i] &= 0xffffffffu;
    }
    t[4] += c;
    while (t[4] >= 4) {
        const uint64_t hi = t[4] >> 2;
        t[4] &= 3u;
        t[0] += 5 * hi;
        for (int i = 0; i < 4; i++) {
            t[i + 1] += t[i] >> 32;
            t[i] &= 0xffffffffu;
        }
    }
    // Subtract (2^130 - 5) = {0xfffffffb, 0xffffffff, 0xffffffff, 0xffffffff, 3} once if >=.
    static const uint64_t kP130[5] = {0xfffffffbull, 0xffffffffull, 0xffffffffull,
                                     0xffffffffull, 3ull};
    uint64_t d[5];
    uint64_t b = 0;
    for (int i = 0; i < 5; i++) {
        const uint64_t cur = t[i] - kP130[i] - b;
        d[i] = cur & 0xffffffffull;
        b = (cur >> 63) & 1u;  // set iff the subtraction underflowed
    }
    if (!b)
        for (int i = 0; i < 5; i++) t[i] = d[i];
    for (int i = 0; i < 5; i++) h.h[i] = (uint32_t)t[i];
}

void p1305_blocks(P1305& h, const uint32_t r[4], const uint8_t* m, size_t len) {
    while (len > 0) {
        const size_t bl = len < 16 ? len : 16;
        uint8_t block[16] = {};
        std::memcpy(block, m, bl);
        uint64_t c = 0;
        for (int i = 0; i < 4; i++) {
            c += (uint64_t)h.h[i] + Load32LE(block + 4 * i);
            h.h[i] = (uint32_t)c;
            c >>= 32;
        }
        // The 0x01 byte at bit 8*bl (2^128 for a full block), plus the addition carry.
        const unsigned bit = (unsigned)(8 * bl);
        h.h[bit / 32] += 1u << (bit % 32);
        h.h[4] += (uint32_t)c;
        p1305_mul(h, r);
        m += bl;
        len -= bl;
    }
}

// ---------------------------------------------------------------------------
// AEAD_CHACHA20_POLY1305 (RFC 8439 section 2.8), empty AAD.
// ---------------------------------------------------------------------------

void AeadMac(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ct, size_t ctLen,
             uint8_t tag[16]) {
    uint8_t block0[64];
    ChaChaBlock(key, 0, nonce, block0);
    uint32_t r[4], s[4];
    for (int i = 0; i < 4; i++) {
        r[i] = Load32LE(block0 + 4 * i);
        s[i] = Load32LE(block0 + 16 + 4 * i);
    }
    r[0] &= 0x0fffffffu;
    r[1] &= 0x0ffffffcu;
    r[2] &= 0x0ffffffcu;
    r[3] &= 0x0ffffffcu;

    // mac_data = ct || pad16(ct) || le64(0) || le64(ctLen): AAD is empty.
    std::vector<uint8_t> mac;
    mac.reserve(ctLen + 32);
    mac.insert(mac.end(), ct, ct + ctLen);
    while (mac.size() % 16 != 0) mac.push_back(0);
    for (int i = 0; i < 8; i++) mac.push_back(0);  // le64(len(aad) = 0)
    uint64_t l = ctLen;
    for (int i = 0; i < 8; i++) {
        mac.push_back((uint8_t)l);
        l >>= 8;
    }

    P1305 h;
    p1305_blocks(h, r, mac.data(), mac.size());
    p1305_reduce_full(h);
    // tag = (h + s) mod 2^128: the low 128 bits of the sum.
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        c += (uint64_t)h.h[i] + s[i];
        Store32LE(tag + 4 * i, (uint32_t)c);
        c >>= 32;
    }
    SecureZero(block0, sizeof block0);
}

bool AeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* pt, size_t ptLen,
              std::string& ctTagOut) {
    ctTagOut.assign(ptLen + 16, '\0');
    uint8_t* ct = reinterpret_cast<uint8_t*>(&ctTagOut[0]);
    uint32_t counter = 1;
    size_t off = 0;
    while (off < ptLen) {
        uint8_t ks[64];
        ChaChaBlock(key, counter++, nonce, ks);
        const size_t bl = ptLen - off < 64 ? ptLen - off : 64;
        for (size_t i = 0; i < bl; i++) ct[off + i] = pt[off + i] ^ ks[i];
        SecureZero(ks, sizeof ks);
        off += bl;
    }
    uint8_t tag[16];
    AeadMac(key, nonce, ct, ptLen, tag);
    std::memcpy(ct + ptLen, tag, 16);
    SecureZero(tag, sizeof tag);
    return true;
}

bool AeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ct, size_t ctLen,
              std::string& ptOut) {
    if (ctLen < 16) return false;
    const size_t bodyLen = ctLen - 16;
    const uint8_t* tag = ct + bodyLen;
    uint8_t expect[16];
    AeadMac(key, nonce, ct, bodyLen, expect);
    // Constant-time compare: the RFC mandates it, and it costs nothing here.
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= expect[i] ^ tag[i];
    SecureZero(expect, sizeof expect);
    if (diff != 0) return false;
    ptOut.assign(bodyLen, '\0');
    uint8_t* pt = reinterpret_cast<uint8_t*>(&ptOut[0]);
    uint32_t counter = 1;
    size_t off = 0;
    while (off < bodyLen) {
        uint8_t ks[64];
        ChaChaBlock(key, counter++, nonce, ks);
        const size_t bl = bodyLen - off < 64 ? bodyLen - off : 64;
        for (size_t i = 0; i < bl; i++) pt[off + i] = ct[off + i] ^ ks[i];
        SecureZero(ks, sizeof ks);
        off += bl;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Envelope + key derivation.
// ---------------------------------------------------------------------------

constexpr char kMagic0 = 'M';
constexpr char kMagic1 = 'V';
constexpr uint8_t kVersion = 0x01;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kHeaderLen = 3;  // magic(2) + version(1)
// "multivoid-signaling-enc-v1" is 26 bytes.
constexpr char kKdfTag[] = "multivoid-signaling-enc-v1";

// Testable core of DeriveSessionKey: the seed and both public keys are parameters, so the
// selftest can exercise commutativity without the key file.
bool DeriveFor(const uint8_t seed[32], const uint8_t selfPub[32], const uint8_t peerPub[32],
               SessionKey& out) {
    uint8_t xPriv[32], xPub[32], shared[32];
    if (!ed25519_seed_to_x25519(xPriv, seed)) return false;
    if (!ed25519_pub_to_x25519(xPub, peerPub)) {
        SecureZero(xPriv, sizeof xPriv);
        return false;
    }
    x25519(shared, xPriv, xPub);
    SecureZero(xPriv, sizeof xPriv);
    SecureZero(xPub, sizeof xPub);
    // RFC 7748 section 6.1: an all-zero DH output means a small-order peer point --
    // contribute nothing, refuse.
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared[i];
    if (acc == 0) {
        SecureZero(shared, sizeof shared);
        return false;
    }
    // K = SHA-256(tag || id_lo || id_hi || shared), identities in lexicographic order.
    const int cmp = std::memcmp(selfPub, peerPub, 32);
    const uint8_t* lo = cmp <= 0 ? selfPub : peerPub;
    const uint8_t* hi = cmp <= 0 ? peerPub : selfPub;
    uint8_t kdfIn[sizeof(kKdfTag) - 1 + 32 + 32 + 32];
    uint8_t* p = kdfIn;
    std::memcpy(p, kKdfTag, sizeof(kKdfTag) - 1);
    p += sizeof(kKdfTag) - 1;
    std::memcpy(p, lo, 32);
    p += 32;
    std::memcpy(p, hi, 32);
    p += 32;
    std::memcpy(p, shared, 32);
    const bool ok = Sha256(kdfIn, sizeof kdfIn, out.bytes);
    SecureZero(kdfIn, sizeof kdfIn);
    SecureZero(shared, sizeof shared);
    return ok;
}

}  // namespace

bool LooksLikeEnvelope(const uint8_t* env, size_t envLen);  // fwd, defined below

bool DeriveSessionKey(const std::string& peerIdentity, SessionKey& out) {
    peer_identity::PubKey peerPub;
    if (!peer_identity::PublicKeyFromIdentityString(peerIdentity, peerPub)) return false;
    uint8_t seed[32];
    // LocalPrivateSeed does not exist yet: the peer_identity owner adds it (see the header).
    if (!peer_identity::LocalPrivateSeed(seed)) {
        SecureZero(seed, sizeof seed);
        return false;
    }
    const bool ok =
        DeriveFor(seed, peer_identity::LocalPublicKey().data(), peerPub.data(), out);
    SecureZero(seed, sizeof seed);
    return ok;
}

bool Seal(const SessionKey& key, const uint8_t* pt, size_t ptLen, std::string& envelopeOut) {
    uint8_t nonce[kNonceLen];
    if (!peer_identity::RandomBytes(nonce, sizeof nonce)) return false;
    std::string ctTag;
    AeadSeal(key.bytes, nonce, pt, ptLen, ctTag);
    envelopeOut.clear();
    envelopeOut.reserve(kHeaderLen + kNonceLen + ctTag.size());
    envelopeOut.push_back(kMagic0);
    envelopeOut.push_back(kMagic1);
    envelopeOut.push_back(static_cast<char>(kVersion));
    envelopeOut.append(reinterpret_cast<const char*>(nonce), kNonceLen);
    envelopeOut += ctTag;
    SecureZero(nonce, sizeof nonce);
    return true;
}

bool LooksLikeEnvelope(const uint8_t* env, size_t envLen) {
    return envLen >= kHeaderLen + kNonceLen + kTagLen && env[0] == kMagic0 && env[1] == kMagic1;
}

bool Open(const SessionKey& key, const uint8_t* env, size_t envLen, std::string& ptOut) {
    if (!LooksLikeEnvelope(env, envLen)) return false;
    if (env[2] != kVersion) return false;  // a newer envelope version: refuse, do not guess
    const uint8_t* nonce = env + kHeaderLen;
    const uint8_t* ct = env + kHeaderLen + kNonceLen;
    const size_t ctLen = envLen - kHeaderLen - kNonceLen;
    return AeadOpen(key.bytes, nonce, ct, ctLen, ptOut);
}

// ---------------------------------------------------------------------------
// Selftest.
// ---------------------------------------------------------------------------

namespace {

bool FromHex(const char* hex, uint8_t* out, size_t n) {
    auto nib = [](char c) -> int {
        if ('0' <= c && c <= '9') return c - '0';
        if ('a' <= c && c <= 'f') return c - 'a' + 10;
        if ('A' <= c && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i++) {
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool HexEq(const uint8_t* got, const char* wantHex, size_t n) {
    uint8_t want[256];
    if (n > sizeof want || !FromHex(wantHex, want, n)) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= got[i] ^ want[i];
    return diff == 0;
}

}  // namespace

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) {
            ++pass;
            return;
        }
        UE_LOGE("signaling_crypto selftest FAIL: %s", what);
    };

    // 1: SHA-512 against the FIPS 180-4 vector -- the seed->x25519 conversion hashes with it,
    // so a wrong digest would still "work" between two of our builds but break the documented
    // libsodium conversion.
    {
        uint8_t d[64]{};
        check(Sha512("abc", 3, d) &&
                  HexEq(d,
                        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
                        64),
              "SHA-512(\"abc\") != the FIPS 180-4 digest");
    }

    // 2-3: X25519 against the two RFC 7748 section 5.2 vectors. Vector 2's u-coordinate has
    // its high bit set, which is what pins the no-masking decode.
    {
        uint8_t scalar[32], u[32], out[32];
        check(FromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                      scalar, 32) &&
                  FromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                          u, 32),
              "selftest vector failed to parse");
        x25519(out, scalar, u);
        check(HexEq(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                    32),
              "X25519 != RFC 7748 section 5.2 vector 1");
        check(FromHex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
                      scalar, 32) &&
                  FromHex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
                          u, 32),
              "selftest vector failed to parse");
        x25519(out, scalar, u);
        check(HexEq(out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                    32),
              "X25519 != RFC 7748 section 5.2 vector 2");
    }

    // 4: DH commutativity with fixed scalars, and non-degeneracy.
    {
        uint8_t a[32], b[32], pa[32], pb[32], sab[32], sba[32];
        const uint8_t base[32] = {9};
        FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fbba1db92f30", a, 32);
        FromHex("5dab087e624a8a4b79e17f8b83800ee66af925d965a2e6c99b84c8f3367336c", b, 32);
        x25519(pa, a, base);
        x25519(pb, b, base);
        x25519(sab, a, pb);
        x25519(sba, b, pa);
        uint8_t diff = 0, zero = 0;
        for (int i = 0; i < 32; i++) {
            diff |= sab[i] ^ sba[i];
            zero |= sab[i];
        }
        check(diff == 0, "X25519(a, X25519(b, 9)) != X25519(b, X25519(a, 9))");
        check(zero != 0, "the DH shared secret came out all-zero");
    }

    // 5: the all-zero u-coordinate gives the all-zero output (RFC 7748 section 6.1), which
    // DeriveFor must refuse.
    {
        uint8_t scalar[32], zero[32] = {}, out[32];
        FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fbba1db92f30",
                scalar, 32);
        x25519(out, scalar, zero);
        uint8_t acc = 0;
        for (int i = 0; i < 32; i++) acc |= out[i];
        check(acc == 0, "X25519 against the all-zero point was not all-zero");
        SessionKey k;
        check(!DeriveFor(scalar, zero, zero, k),
              "DeriveFor accepted a degenerate (all-zero) DH result");
    }

    // 6-8: the Ed25519->X25519 conversion, via the RFC 8032 section 7.1 vector-1 keypair
    // (already pinned in peer_identity's selftest): the two conversion paths must agree, and
    // two converted keypairs must DH to the same secret both ways.
    {
        uint8_t seedA[32], edPubA[32], seedB[32], edPubB[32];
        check(FromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                      seedA, 32) &&
                  FromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                          edPubA, 32) &&
                  FromHex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
                          seedB, 32) &&
                  FromHex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                          edPubB, 32),
              "selftest vector failed to parse");
        uint8_t xPrivA[32], xPubA1[32], xPubA2[32];
        check(ed25519_seed_to_x25519(xPrivA, seedA) &&
                  ed25519_pub_to_x25519(xPubA2, edPubA),
              "the Ed25519->X25519 conversion failed");
        const uint8_t base[32] = {9};
        x25519(xPubA1, xPrivA, base);
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= xPubA1[i] ^ xPubA2[i];
        check(diff == 0, "x25519(priv-converted) != pub-converted for the same Ed25519 key");
        SessionKey kab, kba;
        check(DeriveFor(seedA, edPubA, edPubB, kab) && DeriveFor(seedB, edPubB, edPubA, kba),
              "DeriveFor failed for converted keypairs");
        diff = 0;
        for (int i = 0; i < 32; i++) diff |= kab.bytes[i] ^ kba.bytes[i];
        check(diff == 0, "the two peers did not derive the same session key");
        // And the session key is bound to the pair: a different peer gives a different key.
        SessionKey kaa;
        DeriveFor(seedA, edPubA, edPubA, kaa);
        diff = 0;
        for (int i = 0; i < 32; i++) diff |= kab.bytes[i] ^ kaa.bytes[i];
        check(diff != 0, "the session key did not change with the peer");
    }

    // 9: Poly1305 against the RFC 8439 section 2.5.2 vector.
    {
        uint8_t key[32], tag[16];
        const char* msg = "Cryptographic Forum Research Group";
        check(FromHex("85d6be4557ea1fe11fd6e6f6d5d787f85d6be4557ea1fe11fd6e6f6d5d787f",
                      key, 32),
              "selftest vector failed to parse");
        uint32_t r[4], s[4];
        for (int i = 0; i < 4; i++) {
            r[i] = Load32LE(key + 4 * i);
            s[i] = Load32LE(key + 16 + 4 * i);
        }
        r[0] &= 0x0fffffffu;
        r[1] &= 0x0ffffffcu;
        r[2] &= 0x0ffffffcu;
        r[3] &= 0x0ffffffcu;
        P1305 h;
        p1305_blocks(h, r, reinterpret_cast<const uint8_t*>(msg), 34);
        p1305_reduce_full(h);
        uint64_t c = 0;
        for (int i = 0; i < 4; i++) {
            c += (uint64_t)h.h[i] + s[i];
            Store32LE(tag + 4 * i, (uint32_t)c);
            c >>= 32;
        }
        check(HexEq(tag, "a8061dc1305136c6c22b8baf0c0127a9", 16),
              "Poly1305 != RFC 8439 section 2.5.2 vector");
    }

    // 10-11: AEAD_CHACHA20_POLY1305 against the RFC 8439 section 2.8.2 vector (pins the
    // ChaCha20 block function and the composition at once), plus a tampered tag refusing.
    {
        uint8_t key[32], nonce[12];
        const char* pt =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
            "for the future, sunscreen would be it.";
        check(FromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
                      key, 32) &&
                  FromHex("070000004041424344454647", nonce, 12),
              "selftest vector failed to parse");
        std::string ctTag;
        AeadSeal(key, nonce, reinterpret_cast<const uint8_t*>(pt), 114, ctTag);
        check(ctTag.size() == 114 + 16 &&
                  HexEq(reinterpret_cast<const uint8_t*>(ctTag.data()),
                        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
                        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                        "3ff4def08e4b7a9de576d26586cec64b6116",
                        114),
              "ChaCha20-Poly1305 ciphertext != RFC 8439 section 2.8.2");
        check(HexEq(reinterpret_cast<const uint8_t*>(ctTag.data()) + 114,
                    "1ae10b594f09e26a7e902ecbd0600691", 16),
              "ChaCha20-Poly1305 tag != RFC 8439 section 2.8.2");
        std::string back;
        check(AeadOpen(key, nonce, reinterpret_cast<const uint8_t*>(ctTag.data()),
                       ctTag.size(), back) &&
                  back.size() == 114 && std::memcmp(back.data(), pt, 114) == 0,
              "ChaCha20-Poly1305 did not round-trip its own output");
        ctTag[ctTag.size() - 1] ^= 0x01;
        check(!AeadOpen(key, nonce, reinterpret_cast<const uint8_t*>(ctTag.data()),
                        ctTag.size(), back),
              "ChaCha20-Poly1305 ACCEPTED a tampered tag");
    }

    // 12-16: envelope properties -- shape, round trip, and every refusal.
    {
        SessionKey k1, k2;
        uint8_t seed[32] = {7};
        uint8_t pubA[32] = {1}, pubB[32] = {2};
        check(DeriveFor(seed, pubA, pubB, k1) && DeriveFor(seed, pubA, pubB, k2),
              "DeriveFor failed on test inputs");
        const char* msg = "fake GNS signaling bytes";
        std::string env;
        check(Seal(k1, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), env),
              "Seal failed");
        check(env.size() == 3 + 12 + std::strlen(msg) + 16 &&
                  (uint8_t)env[0] == 'M' && (uint8_t)env[1] == 'V' &&
                  (uint8_t)env[2] == 0x01 && LooksLikeEnvelope(
                      reinterpret_cast<const uint8_t*>(env.data()), env.size()),
              "the envelope does not have the documented shape");
        std::string back;
        check(Open(k2, reinterpret_cast<const uint8_t*>(env.data()), env.size(), back) &&
                  back == msg,
              "Open did not recover what Seal produced");
        // A tampered byte anywhere refuses.
        std::string tampered = env;
        tampered[10] ^= 0x40;
        check(!Open(k2, reinterpret_cast<const uint8_t*>(tampered.data()), tampered.size(),
                    back),
              "Open ACCEPTED a tampered envelope");
        // Plaintext is not an envelope.
        check(!LooksLikeEnvelope(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg)),
              "plaintext classified as an envelope");
        check(!Open(k2, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), back),
              "Open ACCEPTED plaintext");
        // Truncation refuses.
        check(!Open(k2, reinterpret_cast<const uint8_t*>(env.data()), 10, back),
              "Open ACCEPTED a truncated envelope");
        // A wrong version refuses rather than decrypting.
        std::string v2 = env;
        v2[2] = 0x02;
        check(!Open(k2, reinterpret_cast<const uint8_t*>(v2.data()), v2.size(), back),
              "Open ACCEPTED an envelope with a wrong version");
        // A different key refuses.
        SessionKey k3;
        uint8_t pubC[32] = {3};
        DeriveFor(seed, pubA, pubC, k3);
        check(!Open(k3, reinterpret_cast<const uint8_t*>(env.data()), env.size(), back),
              "Open ACCEPTED an envelope under the wrong key");
        // Empty plaintext round-trips (and stays an envelope on the wire).
        std::string emptyEnv, emptyBack;
        check(Seal(k1, nullptr, 0, emptyEnv) &&
                  Open(k2, reinterpret_cast<const uint8_t*>(emptyEnv.data()),
                       emptyEnv.size(), emptyBack) &&
                  emptyBack.empty(),
              "an empty plaintext did not round-trip");
        // Two seals of the same plaintext differ (random nonces).
        std::string env2;
        Seal(k1, reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), env2);
        check(env != env2, "two seals of one plaintext produced the same envelope");
    }

    if (pass == total) {
        UE_LOGI("signaling_crypto selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("signaling_crypto selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::signaling_crypto
