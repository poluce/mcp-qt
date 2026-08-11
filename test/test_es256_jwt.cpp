// ES256 JWT client assertion 测试
//
// 覆盖：
//   1. RFC 6979 A.2.5 固定 P-256 私钥 → deriveP256PublicKey 公钥一致（验证点运算）
//   2. PKCS#8 / SEC1 PEM → buildEs256ClientAssertion 生成 JWT 结构/声明/验签闭环
//   3. 篡改 payload 后验签失败
//   4. 非法 PEM 报错
#include "mcp_qt_client/es256jwt.h"
#include "tests/common.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ============================================================================
// 测试固定密钥（RFC 6979 A.2.5：NIST P-256, SHA-256）
// ============================================================================
const std::array<uint8_t, 32> kTestD = {
    0xC9, 0xAF, 0xA9, 0xD8, 0x45, 0xBA, 0x75, 0x16, 0x6B, 0x5C, 0x21,
    0x57, 0x67, 0xB1, 0xD6, 0x93, 0x4E, 0x50, 0xC3, 0xDB, 0x36, 0xE8,
    0x9B, 0x12, 0x7B, 0x8A, 0x62, 0x2B, 0x12, 0x0F, 0x67, 0x21};

const std::array<uint8_t, 32> kTestQx = {
    0x60, 0xFE, 0xD4, 0xBA, 0x25, 0x5A, 0x9D, 0x31, 0xC9, 0x61, 0xEB,
    0x74, 0xC6, 0x35, 0x6D, 0x68, 0xC0, 0x49, 0xB8, 0x92, 0x3B, 0x61,
    0xFA, 0x6C, 0xE6, 0x69, 0x62, 0x2E, 0x60, 0xF2, 0x9F, 0xB6};

const std::array<uint8_t, 32> kTestQy = {
    0x79, 0x03, 0xFE, 0x10, 0x08, 0xB8, 0xBC, 0x99, 0xA4, 0x1A, 0xE9,
    0xE9, 0x56, 0x28, 0xBC, 0x64, 0xF2, 0xF1, 0xB2, 0x0C, 0x2D, 0x7E,
    0x9F, 0x51, 0x77, 0xA3, 0xC2, 0x94, 0xD4, 0x46, 0x22, 0x99};

const char* kClientId = "conformance-test-client-123";
const char* kTokenEndpoint = "https://auth.example.com/token";

// ============================================================================
// DER / PEM 构造（测试专用）
// ============================================================================

static std::vector<uint8_t> derLen(size_t len) {
    std::vector<uint8_t> out;
    if (len < 0x80) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len < 0x100) {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    return out;
}

static std::vector<uint8_t> derSeq(const std::vector<std::vector<uint8_t>>& elems) {
    std::vector<uint8_t> body;
    for (const auto& e : elems) body.insert(body.end(), e.begin(), e.end());
    std::vector<uint8_t> out{0x30};
    auto len = derLen(body.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static std::vector<uint8_t> derInt(const std::vector<uint8_t>& val) {
    std::vector<uint8_t> v = val;
    while (v.size() > 1 && v[0] == 0x00) v.erase(v.begin());
    if (v.empty()) v.push_back(0x00);
    if (v[0] & 0x80) v.insert(v.begin(), 0x00);
    std::vector<uint8_t> out{0x02};
    auto len = derLen(v.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static std::vector<uint8_t> derOctet(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out{0x04};
    auto len = derLen(v.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static std::vector<uint8_t> derOid(const std::vector<uint8_t>& content) {
    std::vector<uint8_t> out{0x06};
    auto len = derLen(content.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

static std::vector<uint8_t> derCtx0(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out{0xA0};
    auto len = derLen(v.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static std::vector<uint8_t> derCtx1(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out{0xA1};
    auto len = derLen(v.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), v.begin(), v.end());
    return out;
}

static std::vector<uint8_t> derBitString(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out{0x03};
    std::vector<uint8_t> content;
    content.push_back(0x00);  // unused bits
    content.insert(content.end(), v.begin(), v.end());
    auto len = derLen(content.size());
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

static std::vector<uint8_t> sec1Der() {
    std::vector<uint8_t> pub;
    pub.push_back(0x04);  // uncompressed point
    pub.insert(pub.end(), kTestQx.begin(), kTestQx.end());
    pub.insert(pub.end(), kTestQy.begin(), kTestQy.end());
    std::vector<uint8_t> d(kTestD.begin(), kTestD.end());
    std::vector<uint8_t> params = derCtx0(derOid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07}));
    std::vector<uint8_t> pubkey = derCtx1(derBitString(pub));
    return derSeq({derInt({0x01}), derOctet(d), params, pubkey});
}

static std::vector<uint8_t> pkcs8Der(const std::vector<uint8_t>& sec1) {
    std::vector<uint8_t> algid = derSeq({
        derOid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01}),         // id-ecPublicKey
        derOid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07})});  // prime256v1
    return derSeq({derInt({0x00}), algid, derOctet(sec1)});
}

static std::string b64Std(const std::vector<uint8_t>& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (data.size() - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (data.size() - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

static std::string pemWrap(const std::string& label, const std::vector<uint8_t>& der) {
    std::string b64 = b64Std(der);
    std::string body;
    for (size_t i = 0; i < b64.size(); i += 64) body += b64.substr(i, 64) + "\n";
    return "-----BEGIN " + label + "-----\n" + body + "-----END " + label + "-----\n";
}

// ============================================================================
// JWT 解析辅助（测试专用）
// ============================================================================

static bool splitJwt(const std::string& jwt, std::string& h, std::string& p, std::string& s) {
    auto d1 = jwt.find('.');
    if (d1 == std::string::npos) return false;
    auto d2 = jwt.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    if (jwt.find('.', d2 + 1) != std::string::npos) return false;
    h = jwt.substr(0, d1);
    p = jwt.substr(d1 + 1, d2 - d1 - 1);
    s = jwt.substr(d2 + 1);
    return true;
}

static std::string b64urlEncode(const std::string& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    const size_t len = data.size();
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(static_cast<uint8_t>(data[i])) << 16) |
                     (uint32_t(static_cast<uint8_t>(data[i + 1])) << 8) |
                     static_cast<uint8_t>(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = uint32_t(static_cast<uint8_t>(data[i])) << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
    } else if (len - i == 2) {
        uint32_t v = (uint32_t(static_cast<uint8_t>(data[i])) << 16) |
                     (uint32_t(static_cast<uint8_t>(data[i + 1])) << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
    }
    return out;
}

static std::string b64urlDecode(const std::string& in) {
    std::string out;
    int val = 0, bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

void test_es256_jwt_derive_public_key_matches_rfc6979() {
    const std::string pem =
        pemWrap("PRIVATE KEY", pkcs8Der(sec1Der()));
    mcp_qt::jwt::P256PublicKey pub;
    std::string err;
    TM_ASSERT_TRUE(mcp_qt::jwt::deriveP256PublicKey(pem, pub, &err), err);
    TM_ASSERT_TRUE(pub.x == kTestQx, "X must match RFC 6979 A.2.5 public key");
    TM_ASSERT_TRUE(pub.y == kTestQy, "Y must match RFC 6979 A.2.5 public key");
}

void test_es256_jwt_pkcs8_build_and_verify() {
    const std::string pem = pemWrap("PRIVATE KEY", pkcs8Der(sec1Der()));
    std::string jwt, err;
    TM_ASSERT_TRUE(mcp_qt::jwt::buildEs256ClientAssertion(pem, kClientId, kTokenEndpoint, jwt, &err), err);

    std::string h, p, s;
    TM_ASSERT_TRUE(splitJwt(jwt, h, p, s), "JWT must have three dot-separated segments");

    const std::string header = b64urlDecode(h);
    const std::string payload = b64urlDecode(p);
    TM_ASSERT_STR_CONTAINS(header, "\"alg\":\"ES256\"", "header alg");
    TM_ASSERT_STR_CONTAINS(header, "\"typ\":\"JWT\"", "header typ");
    TM_ASSERT_STR_CONTAINS(payload, "\"iss\":\"" + std::string(kClientId) + "\"", "payload iss");
    TM_ASSERT_STR_CONTAINS(payload, "\"sub\":\"" + std::string(kClientId) + "\"", "payload sub");
    TM_ASSERT_STR_CONTAINS(payload, "\"aud\":\"" + std::string(kTokenEndpoint) + "\"", "payload aud");
    TM_ASSERT_STR_CONTAINS(payload, "\"jti\":\"", "payload jti present");
    TM_ASSERT_STR_CONTAINS(payload, "\"iat\":", "payload iat present");
    TM_ASSERT_STR_CONTAINS(payload, "\"exp\":", "payload exp present");

    // 验签
    mcp_qt::jwt::P256PublicKey pub;
    TM_ASSERT_TRUE(mcp_qt::jwt::deriveP256PublicKey(pem, pub, &err), err);
    const std::string sigDecoded = b64urlDecode(s);
    TM_ASSERT_TRUE(sigDecoded.size() == 64, "signature must be 64 bytes");
    std::vector<uint8_t> rawSig(sigDecoded.begin(), sigDecoded.end());
    const std::string signingInput = h + "." + p;
    TM_ASSERT_TRUE(mcp_qt::jwt::verifyEs256Signature(signingInput, rawSig, pub, &err), err);
}

void test_es256_jwt_sec1_build_and_verify() {
    const std::string pem = pemWrap("EC PRIVATE KEY", sec1Der());
    std::string jwt, err;
    TM_ASSERT_TRUE(mcp_qt::jwt::buildEs256ClientAssertion(pem, kClientId, kTokenEndpoint, jwt, &err), err);

    std::string h, p, s;
    TM_ASSERT_TRUE(splitJwt(jwt, h, p, s), "JWT must have three dot-separated segments");

    mcp_qt::jwt::P256PublicKey pub;
    TM_ASSERT_TRUE(mcp_qt::jwt::deriveP256PublicKey(pem, pub, &err), err);
    const std::string sigDecoded = b64urlDecode(s);
    std::vector<uint8_t> rawSig(sigDecoded.begin(), sigDecoded.end());
    TM_ASSERT_TRUE(mcp_qt::jwt::verifyEs256Signature(h + "." + p, rawSig, pub, &err), err);
}

void test_es256_jwt_tamper_detected() {
    const std::string pem = pemWrap("PRIVATE KEY", pkcs8Der(sec1Der()));
    std::string jwt, err;
    TM_ASSERT_TRUE(mcp_qt::jwt::buildEs256ClientAssertion(pem, kClientId, kTokenEndpoint, jwt, &err), err);

    // 篡改 payload 的 aud
    std::string h, p, s;
    TM_ASSERT_TRUE(splitJwt(jwt, h, p, s), "JWT split");
    std::string tampered = b64urlDecode(p);
    const std::string evil = "https://evil.example.com/token";
    auto pos = tampered.find(kTokenEndpoint);
    TM_ASSERT_TRUE(pos != std::string::npos, "aud claim found in payload");
    tampered.replace(pos, std::strlen(kTokenEndpoint), evil);
    const std::string tamperedPayloadB64 = b64urlEncode(tampered);

    mcp_qt::jwt::P256PublicKey pub;
    TM_ASSERT_TRUE(mcp_qt::jwt::deriveP256PublicKey(pem, pub, &err), err);
    const std::string sigDecoded = b64urlDecode(s);
    std::vector<uint8_t> rawSig(sigDecoded.begin(), sigDecoded.end());
    // 篡改后的 payload 重新编码，用原签名验签必须失败
    TM_ASSERT_FALSE(
        mcp_qt::jwt::verifyEs256Signature(h + "." + tamperedPayloadB64, rawSig, pub, &err),
        "signature must not verify against altered payload");
}

void test_es256_jwt_invalid_pem_rejected() {
    std::string jwt, err;
    TM_ASSERT_FALSE(mcp_qt::jwt::buildEs256ClientAssertion("not-a-key", kClientId, kTokenEndpoint, jwt, &err), "garbage input");
    TM_ASSERT_FALSE(mcp_qt::jwt::buildEs256ClientAssertion(
                        "-----BEGIN PUBLIC KEY-----\nMIIB\n-----END PUBLIC KEY-----\n",
                        kClientId, kTokenEndpoint, jwt, &err),
                    "public key (no private scalar)");
}

void test_es256_jwt_claims_times_and_jti() {
    const std::string pem = pemWrap("PRIVATE KEY", pkcs8Der(sec1Der()));
    std::string jwt, err;
    TM_ASSERT_TRUE(mcp_qt::jwt::buildEs256ClientAssertion(pem, kClientId, kTokenEndpoint, jwt, &err), err);
    std::string h, p, s;
    TM_ASSERT_TRUE(splitJwt(jwt, h, p, s), "JWT split");
    const std::string payload = b64urlDecode(p);

    // iat/exp 解析（近似：exp - iat == 300）
    auto findNum = [&payload](const std::string& key) -> long long {
        std::string needle = "\"" + key + "\":";
        auto pos = payload.find(needle);
        if (pos == std::string::npos) return -1;
        pos += needle.size();
        long long v = 0;
        while (pos < payload.size() && payload[pos] >= '0' && payload[pos] <= '9') {
            v = v * 10 + (payload[pos] - '0');
            ++pos;
        }
        return v;
    };
    long long iat = findNum("iat");
    long long exp = findNum("exp");
    TM_ASSERT_TRUE(iat > 0, "iat positive");
    TM_ASSERT_TRUE(exp - iat == 300, "exp - iat == 300 (5 min TTL)");
    TM_ASSERT_TRUE(exp > iat, "exp > iat");
}
