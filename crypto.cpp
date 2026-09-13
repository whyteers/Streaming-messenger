#include "crypto.h"

#include <array>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace Crypto {
namespace {



inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t bitLen = 0;
    uint8_t buf[64] = {};
    size_t bufLen = 0;
};

void sha256_transform(Sha256Ctx& ctx, const uint8_t* chunk) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(chunk[i*4]) << 24) | (uint32_t(chunk[i*4+1]) << 16) |
               (uint32_t(chunk[i*4+2]) << 8) | uint32_t(chunk[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=ctx.h[0],b=ctx.h[1],c=ctx.h[2],d=ctx.h[3],
             e=ctx.h[4],f=ctx.h[5],g=ctx.h[6],h=ctx.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t ch = (e&f)^(~e&g);
        uint32_t t1 = h+S1+ch+K[i]+w[i];
        uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t maj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx.h[0]+=a; ctx.h[1]+=b; ctx.h[2]+=c; ctx.h[3]+=d;
    ctx.h[4]+=e; ctx.h[5]+=f; ctx.h[6]+=g; ctx.h[7]+=h;
}

void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    ctx.bitLen += uint64_t(len) * 8;
    while (len > 0) {
        size_t take = 64 - ctx.bufLen;
        if (take > len) take = len;
        memcpy(ctx.buf + ctx.bufLen, data, take);
        ctx.bufLen += take; data += take; len -= take;
        if (ctx.bufLen == 64) { sha256_transform(ctx, ctx.buf); ctx.bufLen = 0; }
    }
}

std::array<uint8_t,32> sha256_final(Sha256Ctx& ctx) {
    uint64_t bitLen = ctx.bitLen;
    ctx.buf[ctx.bufLen++] = 0x80;
    if (ctx.bufLen > 56) {
        while (ctx.bufLen < 64) ctx.buf[ctx.bufLen++] = 0;
        sha256_transform(ctx, ctx.buf); ctx.bufLen = 0;
    }
    while (ctx.bufLen < 56) ctx.buf[ctx.bufLen++] = 0;
    for (int i = 7; i >= 0; --i) ctx.buf[ctx.bufLen++] = uint8_t((bitLen >> (i*8)) & 0xFF);
    sha256_transform(ctx, ctx.buf);
    std::array<uint8_t,32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i*4+0] = uint8_t((ctx.h[i] >> 24) & 0xFF);
        out[i*4+1] = uint8_t((ctx.h[i] >> 16) & 0xFF);
        out[i*4+2] = uint8_t((ctx.h[i] >> 8) & 0xFF);
        out[i*4+3] = uint8_t(ctx.h[i] & 0xFF);
    }
    return out;
}

std::array<uint8_t,32> sha256_raw(const uint8_t* data, size_t len) {
    Sha256Ctx ctx;
    ctx.h[0]=0x6a09e667; ctx.h[1]=0xbb67ae85; ctx.h[2]=0x3c6ef372; ctx.h[3]=0xa54ff53a;
    ctx.h[4]=0x510e527f; ctx.h[5]=0x9b05688c; ctx.h[6]=0x1f83d9ab; ctx.h[7]=0x5be0cd19;
    sha256_update(ctx, data, len);
    return sha256_final(ctx);
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s; s.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s[i*2+0] = digits[(data[i] >> 4) & 0xF];
        s[i*2+1] = digits[data[i] & 0xF];
    }
    return s;
}

bool randomBytes(uint8_t* out, size_t len) {
#ifdef _WIN32
    if (len > 0 && BCryptGenRandom(nullptr, out, (ULONG)len,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return true;
#endif
    try {
        std::random_device rd;
        for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)(rd() & 0xFF);
        return true;
    } catch (...) { return false; }
}

static const int kIterations = 20000;

std::string stretchHash(const std::string& saltHex, const std::string& password) {

    std::string cur = toHex(sha256_raw(
        reinterpret_cast<const uint8_t*>((saltHex + ":" + password).data()),
        saltHex.size() + 1 + password.size()).data(), 32);
    std::string input;
    input.reserve(64 + 1 + saltHex.size());
    for (int i = 1; i < kIterations; ++i) {
        input.assign(cur); input.push_back(':'); input.append(saltHex);
        cur = toHex(sha256_raw(reinterpret_cast<const uint8_t*>(input.data()),
                               input.size()).data(), 32);
    }
    return cur;
}

}

bool init() { return true; }

std::string sha256(const uint8_t* data, size_t len) {
    auto raw = sha256_raw(data ? data : reinterpret_cast<const uint8_t*>(""), len);
    return toHex(raw.data(), raw.size());
}

std::string sha256(const std::string& input) {
    return sha256(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::string generateToken(size_t bytesCount) {
    if (bytesCount == 0) bytesCount = 32;
    if (bytesCount > 256) bytesCount = 256;
    std::vector<uint8_t> buf(bytesCount);
    if (!randomBytes(buf.data(), buf.size())) {

        std::mt19937_64 rng((uint64_t)time(nullptr) ^ 0x9E3779B97F4A7C15ull);
        for (auto& b : buf) b = (uint8_t)(rng() & 0xFF);
    }
    return toHex(buf.data(), buf.size());
}

std::string hashPassword(const std::string& password) {
    uint8_t saltRaw[16];
    if (!randomBytes(saltRaw, sizeof(saltRaw))) {
        std::mt19937_64 rng((uint64_t)time(nullptr) ^ 0x12345678ull);
        for (auto& b : saltRaw) b = (uint8_t)(rng() & 0xFF);
    }
    std::string saltHex = toHex(saltRaw, sizeof(saltRaw));
    std::string h = stretchHash(saltHex, password);
    return "v1$" + saltHex + "$" + h;
}

bool verifyPassword(const std::string& passwordHash, const std::string& password) noexcept {
    try {

        if (passwordHash.rfind("v1$", 0) != 0) return false;
        size_t p2 = passwordHash.find('$', 3);
        if (p2 == std::string::npos) return false;
        std::string saltHex = passwordHash.substr(3, p2 - 3);
        std::string expect = passwordHash.substr(p2 + 1);
        if (saltHex.size() != 32 || expect.size() != 64) return false;
        std::string got = stretchHash(saltHex, password);
        if (got.size() != expect.size()) return false;

        volatile unsigned diff = 0;
        for (size_t i = 0; i < got.size(); ++i) diff |= (unsigned)(got[i] ^ expect[i]);
        return diff == 0;
    } catch (...) { return false; }
}

}
