#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mcp_qt {
namespace jwt {

/// P-256 公钥（大端 X||Y，各 32 字节）。
struct P256PublicKey {
    std::array<uint8_t, 32> x{};
    std::array<uint8_t, 32> y{};
};

/// 用 PEM 编码的 P-256 私钥（PKCS#8 "BEGIN PRIVATE KEY" 或 SEC1 "BEGIN EC PRIVATE KEY"）
/// 生成 RFC 7519 / RFC 7523 ES256 client assertion（JWT）。
///
/// JWT header: {"alg":"ES256","typ":"JWT"}
/// JWT payload: {iss=clientId, sub=clientId, aud=tokenEndpoint, jti, iat, exp}
/// 签名算法：ES256（ECDSA P-256 + SHA-256），签名输出 RFC 7515 IEEE P1363 原始 R||S。
///
/// @param privateKeyPem  PEM 私钥文本
/// @param clientId       JWT iss/sub（RFC 7523 §3：OAuth client_id）
/// @param tokenEndpoint  JWT aud（授权服务器 token endpoint）
/// @param outJwt         输出的紧凑 JWT（header.payload.signature）
/// @param errorOut       失败时返回可读错误信息（可为 nullptr）
/// @return true 成功；false 失败（PEM 非法 / 非 P-256 密钥 / 平台加密失败）
bool buildEs256ClientAssertion(const std::string& privateKeyPem,
                               const std::string& clientId,
                               const std::string& tokenEndpoint,
                               std::string& outJwt,
                               std::string* errorOut = nullptr);

/// 从 PEM P-256 私钥派生公钥（X||Y 大端）。测试与验签用。
bool deriveP256PublicKey(const std::string& privateKeyPem,
                         P256PublicKey& outKey,
                         std::string* errorOut = nullptr);

/// 验证 ES256 原始签名（R||S，IEEE P1363，64 字节）。
/// @param signingInput  JWT 签名输入（"header.payload"）
/// @param rawSignature  64 字节原始签名
/// @param publicKey     P-256 公钥
bool verifyEs256Signature(const std::string& signingInput,
                          const std::vector<uint8_t>& rawSignature,
                          const P256PublicKey& publicKey,
                          std::string* errorOut = nullptr);

} // namespace jwt
} // namespace mcp_qt
