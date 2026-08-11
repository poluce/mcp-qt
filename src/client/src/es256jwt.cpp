// Es256Jwt.cpp — RFC 7519 / RFC 7523 ES256 JWT client assertion 生成
//
// 设计说明（2026 依赖取舍）：
// 仓库（mcp-qt）依赖仅 Qt6 + nlohmann/json，无第三方加密库。ES256 = ECDSA P-256 +
// SHA-256 签名。Windows CNG（BCrypt）的 BCRYPT_ECCPRIVATE_BLOB 需要 X/Y/d 三个分量，
// 而 PEM 私钥只含标量 d，因此本文件实现：
//   1. 最小的 P-256 椭圆曲线标量乘法（自含 256 位模 p 运算，逐位 shift-add 模乘，
//      Fermat 小定理模逆），用 d 恢复公钥 (X, Y)；
//   2. Windows 用系统 BCrypt 完成 SHA-256 / ECDSA 签名与验签（零新增依赖，
//      与 src/core/mcpoauthclient.cpp 的 PKCE SHA-256 方案一致）；
//   3. 非 Windows 沿用仓库既有 OpenSSL 依赖（mcpoauthclient.cpp 已 include
//      <openssl/sha.h>）。
// 该最小实现仅用于本地 OAuth client assertion 生成，不通用替代加密库。
#include "mcp_qt_client/es256jwt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#endif

namespace mcp_qt {
namespace jwt {

namespace {

// ============================================================================
// Base64
// ============================================================================

static const char kBase64UrlTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string b64urlEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kBase64UrlTable[(v >> 18) & 63]);
        out.push_back(kBase64UrlTable[(v >> 12) & 63]);
        out.push_back(kBase64UrlTable[(v >> 6) & 63]);
        out.push_back(kBase64UrlTable[v & 63]);
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kBase64UrlTable[(v >> 18) & 63]);
        out.push_back(kBase64UrlTable[(v >> 12) & 63]);
    } else if (len - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kBase64UrlTable[(v >> 18) & 63]);
        out.push_back(kBase64UrlTable[(v >> 12) & 63]);
        out.push_back(kBase64UrlTable[(v >> 6) & 63]);
    }
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static bool b64Decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    int val = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }
    return true;
}

// ============================================================================
// P-256 域运算（256 位、模 p）
//
// 表示：std::array<uint64_t,4>（little-endian words），值恒小于 p。
// 模乘采用逐位 shift-add（正确性优先，性能足够本地 OAuth 使用）。
// ============================================================================

using Word = uint64_t;
using Big = std::array<Word, 4>;

static const Big kP = []() -> Big {
    // p = FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
    const uint8_t b[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Big r{};
    for (int k = 0; k < 4; ++k) {
        Word w = 0;
        for (int j = 0; j < 8; ++j) w = (w << 8) | b[k * 8 + j];
        r[3 - k] = w;
    }
    return r;
}();

// delta = 2^256 - p = 2^224 - 2^192 - 2^96 + 1（carry 后补差用）
static const Big kDelta = []() -> Big {
    // 从 kP 计算：2^256 - p（kP < 2^256，delta < 2^256）
    Big r{};
    // 手工构造：word = {1, 0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE}
    r[0] = 0x0000000000000001ULL;
    r[1] = 0xFFFFFFFF00000000ULL;
    r[2] = 0xFFFFFFFFFFFFFFFFULL;
    r[3] = 0x00000000FFFFFFFEULL;
    return r;
}();

static Big bigFromBytesBE(const uint8_t* b) {
    Big r{};
    for (int k = 0; k < 4; ++k) {
        Word w = 0;
        for (int j = 0; j < 8; ++j) w = (w << 8) | b[k * 8 + j];
        r[3 - k] = w;
    }
    return r;
}

static void bigToBytesBE(const Big& a, uint8_t* out) {
    for (int k = 0; k < 4; ++k) {
        Word w = a[3 - k];
        for (int j = 0; j < 8; ++j) out[k * 8 + j] = static_cast<uint8_t>(w >> (56 - 8 * j));
    }
}

static bool bigGe(const Big& a, const Big& b) {
    for (int i = 3; i >= 0; --i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return true;
}

static Big bigAdd(const Big& a, const Big& b, Word* carryOut) {
    Big r{};
    Word carry = 0;
    for (int i = 0; i < 4; ++i) {
        Word sum = a[i] + b[i];
        Word c1 = (sum < a[i]) ? 1 : 0;
        r[i] = sum + carry;
        Word c2 = (r[i] < sum) ? 1 : 0;
        carry = c1 | c2;
    }
    *carryOut = carry;
    return r;
}

static Big bigSub(const Big& a, const Big& b, Word* borrowOut) {
    Big r{};
    Word borrow = 0;
    for (int i = 0; i < 4; ++i) {
        Word sub = a[i] - b[i];
        Word b1 = (a[i] < b[i]) ? 1 : 0;
        Word v = sub - borrow;
        Word b2 = (sub < borrow) ? 1 : 0;
        r[i] = v;
        borrow = b1 | b2;
    }
    *borrowOut = borrow;
    return r;
}

static Big modAddP(const Big& x, const Big& y) {
    Word carry;
    Big r = bigAdd(x, y, &carry);
    if (carry) {
        // x+y = 2^256 + r，模 p 后 = r + (2^256 - p)；已知 r + delta < p（x,y<p => x+y<2p）
        Word c2;
        Big r2 = bigAdd(r, kDelta, &c2);
        return r2;
    }
    if (bigGe(r, kP)) {
        Word b;
        r = bigSub(r, kP, &b);
    }
    return r;
}

static Big modSubP(const Big& x, const Big& y) {
    Word borrow;
    Big r = bigSub(x, y, &borrow);
    if (borrow) {
        Word carry;
        r = bigAdd(r, kP, &carry);
    }
    return r;
}

static Big modDoubleP(const Big& x) { return modAddP(x, x); }

static Big modMulP(const Big& a, const Big& b) {
    Big result{0, 0, 0, 0};
    Big addend = b;
    for (int bit = 0; bit < 256; ++bit) {
        if ((a[bit >> 6] >> (bit & 63)) & 1) result = modAddP(result, addend);
        addend = modDoubleP(addend);
    }
    return result;
}

// Fermat 小定理逆元：a^(p-2) mod p
static Big modInvP(const Big& a) {
    // p - 2（LE words）
    const Big exp{0xFFFFFFFFFFFFFFFDULL, 0x00000000FFFFFFFFULL,
                  0x0000000000000000ULL, 0xFFFFFFFF00000001ULL};
    Big base = a;
    Big result{1, 0, 0, 0};
    for (int bit = 0; bit < 256; ++bit) {
        if ((exp[bit >> 6] >> (bit & 63)) & 1) result = modMulP(result, base);
        base = modMulP(base, base);
    }
    return result;
}

// ============================================================================
// P-256 椭圆曲线（Jacobian 坐标）
// ============================================================================

struct PointJ {
    Big x, y, z;
    bool inf{true};
};

static const Big kGx = []() -> Big {
    const uint8_t b[32] = {
        0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47,
        0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
        0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
        0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96};
    return bigFromBytesBE(b);
}();

static const Big kGy = []() -> Big {
    const uint8_t b[32] = {
        0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B,
        0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
        0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
        0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5};
    return bigFromBytesBE(b);
}();

static Big bigZero() { return Big{0, 0, 0, 0}; }
static Big bigOne() { return Big{1, 0, 0, 0}; }

// 点倍（Jacobian；P-256 的 a = -3，M = 3*X1^2 - 3*Z1^4）
static PointJ pointDouble(const PointJ& P) {
    if (P.inf) return P;
    const Big A = modMulP(P.x, P.x);
    const Big B = modMulP(P.y, P.y);
    const Big C = modMulP(B, B);
    // D = 2*((X+B)^2 - A - C) = 4*X*Y^2
    Big t1 = modAddP(P.x, B);
    t1 = modMulP(t1, t1);
    t1 = modSubP(t1, A);
    t1 = modSubP(t1, C);
    const Big D = modDoubleP(t1);
    // E = M = 3*(X1^2 - Z1^4)
    Big z2 = modMulP(P.z, P.z);
    z2 = modMulP(z2, z2);
    Big E = modSubP(A, z2);
    E = modAddP(modDoubleP(E), E);
    const Big F = modMulP(E, E);
    // X3 = F - 2*D
    PointJ R;
    R.x = modSubP(F, modDoubleP(D));
    // Y3 = E*(D - X3) - 8*C
    Big t2 = modSubP(D, R.x);
    t2 = modMulP(E, t2);
    Big t3 = modDoubleP(C);
    t3 = modDoubleP(t3);
    t3 = modDoubleP(t3);
    R.y = modSubP(t2, t3);
    // Z3 = 2*Y*Z
    R.z = modDoubleP(modMulP(P.y, P.z));
    R.inf = false;
    return R;
}

// 点加（Jacobian + Jacobian，通用公式）
static PointJ pointAdd(const PointJ& P, const PointJ& Q) {
    if (P.inf) return Q;
    if (Q.inf) return P;
    const Big z1z1 = modMulP(P.z, P.z);
    const Big z2z2 = modMulP(Q.z, Q.z);
    const Big u1 = modMulP(P.x, z2z2);
    const Big u2 = modMulP(Q.x, z1z1);
    const Big s1 = modMulP(modMulP(P.y, Q.z), z2z2);
    const Big s2 = modMulP(modMulP(Q.y, P.z), z1z1);

    if (u1[0] == u2[0] && u1[1] == u2[1] && u1[2] == u2[2] && u1[3] == u2[3]) {
        if (s1[0] == s2[0] && s1[1] == s2[1] && s1[2] == s2[2] && s1[3] == s2[3]) {
            return pointDouble(P);
        }
        return PointJ{};  // P + (-P) = infinity
    }

    const Big h = modSubP(u2, u1);
    const Big h2 = modMulP(h, h);
    const Big h3 = modMulP(h2, h);
    const Big rr = modSubP(s2, s1);
    const Big rr2 = modMulP(rr, rr);
    const Big u1h2 = modMulP(u1, h2);

    PointJ R;
    // X3 = R^2 - H^3 - 2*U1*H^2
    R.x = modSubP(modSubP(rr2, h3), modDoubleP(u1h2));
    // Y3 = R*(U1*H^2 - X3) - S1*H^3
    const Big u1h2mx3 = modSubP(u1h2, R.x);
    R.y = modSubP(modMulP(rr, u1h2mx3), modMulP(s1, h3));
    // Z3 = H*Z1*Z2
    R.z = modMulP(h, modMulP(P.z, Q.z));
    R.inf = false;
    return R;
}

// 标量乘法（MSB→LSB double-and-add）
static PointJ scalarMul(const Big& k, const PointJ& P) {
    PointJ R{};  // infinity
    for (int bit = 255; bit >= 0; --bit) {
        R = pointDouble(R);
        if ((k[bit >> 6] >> (bit & 63)) & 1) R = pointAdd(R, P);
    }
    return R;
}

// Jacobian → Affine
static bool jacobianToAffine(const PointJ& P, Big& ax, Big& ay) {
    if (P.inf) return false;
    const Big zInv = modInvP(P.z);
    const Big zInv2 = modMulP(zInv, zInv);
    const Big zInv3 = modMulP(zInv2, zInv);
    ax = modMulP(P.x, zInv2);
    ay = modMulP(P.y, zInv3);
    return true;
}

// ============================================================================
// PEM / DER 解析
// ============================================================================

struct DerElem {
    uint8_t tag{0};
    std::vector<uint8_t> value;
};

static bool derReadElement(const uint8_t* data, size_t len, size_t& pos, DerElem& out) {
    if (pos + 2 > len) return false;
    uint8_t tag = data[pos++];
    uint8_t lenByte = data[pos++];
    size_t vlen;
    if (lenByte & 0x80) {
        int n = lenByte & 0x7F;
        if (n == 0 || n > 4 || pos + static_cast<size_t>(n) > len) return false;
        vlen = 0;
        for (int i = 0; i < n; ++i) vlen = (vlen << 8) | data[pos++];
    } else {
        vlen = lenByte;
    }
    if (pos + vlen > len) return false;
    out.tag = tag;
    out.value.assign(data + pos, data + pos + vlen);
    pos += vlen;
    return true;
}

// 从 DER 提取 P-256 私钥标量 d（支持 SEC1 ECPrivateKey 与 PKCS#8 PrivateKeyInfo）
static bool extractP256Scalar(const std::vector<uint8_t>& der, std::array<uint8_t, 32>& dOut) {
    size_t pos = 0;
    DerElem outer;
    if (!derReadElement(der.data(), der.size(), pos, outer) || outer.tag != 0x30) return false;

    const uint8_t* p = outer.value.data();
    const size_t n = outer.value.size();
    pos = 0;
    DerElem e1;
    if (!derReadElement(p, n, pos, e1) || e1.tag != 0x02) return false;  // version

    DerElem e2;
    if (!derReadElement(p, n, pos, e2)) return false;

    std::vector<uint8_t> dBytes;
    if (e2.tag == 0x30) {
        // PKCS#8：e2=AlgorithmIdentifier，e3=OCTET STRING(内嵌 ECPrivateKey)
        DerElem e3;
        if (!derReadElement(p, n, pos, e3) || e3.tag != 0x04) return false;
        size_t ip = 0;
        DerElem innerSeq;
        if (!derReadElement(e3.value.data(), e3.value.size(), ip, innerSeq) ||
            innerSeq.tag != 0x30)
            return false;
        size_t ip2 = 0;
        DerElem iv1, iv2;
        if (!derReadElement(innerSeq.value.data(), innerSeq.value.size(), ip2, iv1) ||
            iv1.tag != 0x02)
            return false;
        if (!derReadElement(innerSeq.value.data(), innerSeq.value.size(), ip2, iv2) ||
            iv2.tag != 0x04)
            return false;
        dBytes = iv2.value;
    } else if (e2.tag == 0x04) {
        // SEC1：e2=OCTET STRING(privateKey d)
        dBytes = e2.value;
    } else {
        return false;
    }

    if (dBytes.size() > 32) return false;
    std::fill(dOut.begin(), dOut.end(), 0);
    std::copy(dBytes.begin(), dBytes.end(), dOut.end() - dBytes.size());
    return true;
}

static bool parsePemPrivateKey(const std::string& pem, std::array<uint8_t, 32>& dOut,
                               std::string* err) {
    std::string body;
    {
        std::istringstream iss(pem);
        std::string line;
        bool inBlock = false;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            auto last = line.find_last_not_of(" \t");
            line = line.substr(first, last - first + 1);
            if (line.rfind("-----BEGIN", 0) == 0) {
                inBlock = true;
                continue;
            }
            if (line.rfind("-----END", 0) == 0) break;
            if (inBlock) body += line;
        }
    }
    if (body.empty()) {
        if (err) *err = "PEM: no base64 body found";
        return false;
    }
    std::vector<uint8_t> der;
    if (!b64Decode(body, der)) {
        if (err) *err = "PEM: invalid base64";
        return false;
    }
    if (!extractP256Scalar(der, dOut)) {
        if (err) *err = "PEM: not a P-256 private key (expected PKCS#8 or SEC1)";
        return false;
    }
    return true;
}

// ============================================================================
// SHA-256（平台后端）
// ============================================================================

static bool sha256(const uint8_t* data, size_t len, std::array<uint8_t, 32>& out) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    NTSTATUS st =
        BCryptHash(hAlg, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(len),
                   out.data(), static_cast<ULONG>(out.size()));
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return st >= 0;
#else
    return SHA256(data, len, out.data()) != nullptr;
#endif
}

// ============================================================================
// 随机数（jti）
// ============================================================================

static std::string randomHex(size_t nBytes) {
    std::string s;
    s.reserve(nBytes * 2);
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    for (size_t i = 0; i < nBytes; ++i) {
        unsigned char c = static_cast<unsigned char>(rd() & 0xFF);
        s.push_back(hex[c >> 4]);
        s.push_back(hex[c & 0xF]);
    }
    return s;
}

// ============================================================================
// JSON 转义（payload 手写紧凑 JSON，避免引入额外依赖）
// ============================================================================

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char>(ch) >> 4) & 0xF];
                    out += hex[static_cast<unsigned char>(ch) & 0xF];
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

// ============================================================================
// 平台 ES256 签名 / 验签
// ============================================================================

#ifdef _WIN32
static uint32_t eccMagic(bool isPrivate) {
    return isPrivate ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
}
#endif

static bool es256Sign(const std::array<uint8_t, 32>& d, const std::array<uint8_t, 32>& x,
                      const std::array<uint8_t, 32>& y, const std::string& signingInput,
                      std::vector<uint8_t>& rawOut, std::string* err) {
#ifdef _WIN32
    struct EccPrivateBlob {
        BCRYPT_ECCKEY_BLOB header;
        uint8_t xy[64];  // X || Y（大端）
        uint8_t d[32];   // d（大端）
    } blob;
    blob.header.dwMagic = eccMagic(true);
    blob.header.cbKey = 32;
    std::memcpy(blob.xy, x.data(), 32);
    std::memcpy(blob.xy + 32, y.data(), 32);
    std::memcpy(blob.d, d.data(), 32);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) {
        if (err) *err = "BCryptOpenAlgorithmProvider(ECDSA_P256) failed";
        return false;
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS st =
        BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &hKey,
                            reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    if (st < 0) {
        if (err) *err = "BCryptImportKeyPair failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    std::array<uint8_t, 32> hash{};
    if (!sha256(reinterpret_cast<const uint8_t*>(signingInput.data()), signingInput.size(), hash)) {
        if (err) *err = "SHA-256 failed";
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    rawOut.assign(64, 0);
    ULONG cb = 0;
    st = BCryptSignHash(hKey, nullptr, hash.data(), static_cast<ULONG>(hash.size()),
                        rawOut.data(), static_cast<ULONG>(rawOut.size()), &cb, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0 || cb != 64) {
        if (err) *err = "BCryptSignHash failed";
        return false;
    }
    return true;
#else
    // OpenSSL EVP：输出 DER 签名，转 IEEE P1363 R||S
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key) {
        if (err) *err = "OpenSSL: EC_KEY_new_by_curve_name failed";
        return false;
    }
    BIGNUM* bd = BN_bin2bn(d.data(), 32, nullptr);
    if (!bd || EC_KEY_set_private_key(key, bd) != 1) {
        if (err) *err = "OpenSSL: set private key failed";
        if (bd) BN_free(bd);
        EC_KEY_free(key);
        return false;
    }
    BN_free(bd);
    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_assign_EC_KEY(pkey, key);  // 所有权转移给 pkey

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        if (err) *err = "OpenSSL: DigestSignInit failed";
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    if (EVP_DigestSignUpdate(ctx, signingInput.data(), signingInput.size()) != 1) {
        if (err) *err = "OpenSSL: DigestSignUpdate failed";
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    size_t derLen = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &derLen) != 1 || derLen == 0) {
        if (err) *err = "OpenSSL: DigestSignFinal(length) failed";
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    std::vector<uint8_t> der(derLen);
    if (EVP_DigestSignFinal(ctx, der.data(), &derLen) != 1) {
        if (err) *err = "OpenSSL: DigestSignFinal failed";
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    der.resize(derLen);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    // DER(SEQUENCE{INTEGER r, INTEGER s}) → R||S
    size_t pos = 0;
    DerElem seq;
    if (!derReadElement(der.data(), der.size(), pos, seq) || seq.tag != 0x30) {
        if (err) *err = "OpenSSL: unexpected DER signature";
        return false;
    }
    pos = 0;
    DerElem re, se;
    if (!derReadElement(seq.value.data(), seq.value.size(), pos, re) || re.tag != 0x02 ||
        !derReadElement(seq.value.data(), seq.value.size(), pos, se) || se.tag != 0x02) {
        if (err) *err = "OpenSSL: invalid DER signature";
        return false;
    }
    auto to32 = [](const std::vector<uint8_t>& v, std::array<uint8_t, 32>& out) {
        size_t start = 0;
        while (start < v.size() && v[start] == 0x00) ++start;
        size_t n = v.size() - start;
        if (n > 32) return false;
        std::fill(out.begin(), out.end(), 0);
        std::copy(v.begin() + static_cast<std::ptrdiff_t>(start), v.end(),
                  out.end() - static_cast<std::ptrdiff_t>(n));
        return true;
    };
    std::array<uint8_t, 32> rb{}, sb{};
    if (!to32(re.value, rb) || !to32(se.value, sb)) {
        if (err) *err = "OpenSSL: signature integer too large";
        return false;
    }
    rawOut.assign(rb.begin(), rb.end());
    rawOut.insert(rawOut.end(), sb.begin(), sb.end());
    return true;
#endif
}

}  // namespace

// ============================================================================
// 公开 API
// ============================================================================

bool deriveP256PublicKey(const std::string& privateKeyPem, P256PublicKey& outKey,
                         std::string* errorOut) {
    std::array<uint8_t, 32> d{};
    if (!parsePemPrivateKey(privateKeyPem, d, errorOut)) return false;

    PointJ g;
    g.x = kGx;
    g.y = kGy;
    g.z = bigOne();
    g.inf = false;
    Big dBig = bigFromBytesBE(d.data());

    PointJ q = scalarMul(dBig, g);
    Big ax, ay;
    if (!jacobianToAffine(q, ax, ay)) {
        if (errorOut) *errorOut = "P-256: point at infinity";
        return false;
    }
    bigToBytesBE(ax, outKey.x.data());
    bigToBytesBE(ay, outKey.y.data());
    return true;
}

bool buildEs256ClientAssertion(const std::string& privateKeyPem, const std::string& clientId,
                               const std::string& tokenEndpoint, std::string& outJwt,
                               std::string* errorOut) {
    P256PublicKey pub;
    std::array<uint8_t, 32> d{};
    if (!parsePemPrivateKey(privateKeyPem, d, errorOut)) return false;

    PointJ g;
    g.x = kGx;
    g.y = kGy;
    g.z = bigOne();
    g.inf = false;
    Big dBig = bigFromBytesBE(d.data());
    PointJ q = scalarMul(dBig, g);
    Big ax, ay;
    if (!jacobianToAffine(q, ax, ay)) {
        if (errorOut) *errorOut = "P-256: point at infinity";
        return false;
    }
    bigToBytesBE(ax, pub.x.data());
    bigToBytesBE(ay, pub.y.data());

    // header（紧凑 JSON，固定）
    const std::string header = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    // payload（RFC 7523 §3 / RFC 7519）
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const int64_t iat = static_cast<int64_t>(now);
    const int64_t exp = iat + 300;  // 5 分钟有效期
    const std::string jti = randomHex(16);
    std::string payload = "{\"iss\":\"" + jsonEscape(clientId) + "\",\"sub\":\"" +
                          jsonEscape(clientId) + "\",\"aud\":\"" + jsonEscape(tokenEndpoint) +
                          "\",\"jti\":\"" + jti + "\",\"iat\":" + std::to_string(iat) +
                          ",\"exp\":" + std::to_string(exp) + "}";

    const std::string headerB64 = b64urlEncode(reinterpret_cast<const uint8_t*>(header.data()), header.size());
    const std::string payloadB64 = b64urlEncode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    const std::string signingInput = headerB64 + "." + payloadB64;

    std::vector<uint8_t> rawSig;
    if (!es256Sign(d, pub.x, pub.y, signingInput, rawSig, errorOut)) return false;

    outJwt = signingInput + "." + b64urlEncode(rawSig.data(), rawSig.size());
    return true;
}

bool verifyEs256Signature(const std::string& signingInput, const std::vector<uint8_t>& rawSignature,
                          const P256PublicKey& publicKey, std::string* errorOut) {
    if (rawSignature.size() != 64) {
        if (errorOut) *errorOut = "ES256 raw signature must be 64 bytes";
        return false;
    }
#ifdef _WIN32
    struct EccPublicBlob {
        BCRYPT_ECCKEY_BLOB header;
        uint8_t xy[64];
    } pub;
    pub.header.dwMagic = eccMagic(false);
    pub.header.cbKey = 32;
    std::memcpy(pub.xy, publicKey.x.data(), 32);
    std::memcpy(pub.xy + 32, publicKey.y.data(), 32);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) {
        if (errorOut) *errorOut = "BCryptOpenAlgorithmProvider(ECDSA_P256) failed";
        return false;
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hKey,
                            reinterpret_cast<PUCHAR>(&pub), sizeof(pub), 0) < 0) {
        if (errorOut) *errorOut = "BCryptImportKeyPair(pub) failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    std::array<uint8_t, 32> hash{};
    if (!sha256(reinterpret_cast<const uint8_t*>(signingInput.data()), signingInput.size(), hash)) {
        if (errorOut) *errorOut = "SHA-256 failed";
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    NTSTATUS st = BCryptVerifySignature(
        hKey, nullptr, hash.data(), static_cast<ULONG>(hash.size()),
        const_cast<PUCHAR>(rawSignature.data()), static_cast<ULONG>(rawSignature.size()), 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != 0) {
        if (errorOut) *errorOut = "ECDSA signature verification failed";
        return false;
    }
    return true;
#else
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key) {
        if (errorOut) *errorOut = "OpenSSL: EC_KEY_new_by_curve_name failed";
        return false;
    }
    BIGNUM* bx = BN_bin2bn(publicKey.x.data(), 32, nullptr);
    BIGNUM* by = BN_bin2bn(publicKey.y.data(), 32, nullptr);
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* point = EC_POINT_new(group);
    if (!bx || !by || !point ||
        EC_POINT_set_affine_coordinates_GFp(group, point, bx, by, nullptr) != 1 ||
        EC_KEY_set_public_key(key, point) != 1) {
        if (errorOut) *errorOut = "OpenSSL: set public key failed";
        if (bx) BN_free(bx);
        if (by) BN_free(by);
        if (point) EC_POINT_free(point);
        EC_KEY_free(key);
        return false;
    }
    if (bx) BN_free(bx);
    if (by) BN_free(by);
    EC_POINT_free(point);

    // R||S → DER
    BIGNUM* br = BN_bin2bn(rawSignature.data(), 32, nullptr);
    BIGNUM* bs = BN_bin2bn(rawSignature.data() + 32, 32, nullptr);
    ECDSA_SIG* sig = ECDSA_SIG_new();
    if (!br || !bs || !sig || ECDSA_SIG_set0(sig, br, bs) != 1) {
        if (errorOut) *errorOut = "OpenSSL: build signature failed";
        if (br) BN_free(br);
        if (bs) BN_free(bs);
        if (sig) ECDSA_SIG_free(sig);
        EC_KEY_free(key);
        return false;
    }
    std::array<uint8_t, 32> hash{};
    if (!sha256(reinterpret_cast<const uint8_t*>(signingInput.data()), signingInput.size(), hash)) {
        if (errorOut) *errorOut = "SHA-256 failed";
        ECDSA_SIG_free(sig);
        EC_KEY_free(key);
        return false;
    }
    int ok = ECDSA_do_verify(hash.data(), static_cast<int>(hash.size()), sig, key);
    ECDSA_SIG_free(sig);
    EC_KEY_free(key);
    if (ok != 1) {
        if (errorOut) *errorOut = "ECDSA signature verification failed";
        return false;
    }
    return true;
#endif
}

}  // namespace jwt
}  // namespace mcp_qt
