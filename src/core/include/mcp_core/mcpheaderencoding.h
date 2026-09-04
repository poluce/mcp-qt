#pragma once
#include <string>

namespace mcp {

/**
 * @brief MCP 2026-07-28 HTTP header 值编码工具（SEP-2243 Value Encoding）。
 *
 * 规范要求 Mcp-Name / Mcp-Param-{Name} 在值不能安全表示为纯 ASCII header
 * 时使用 Base64 sentinel 格式：`=?base64?<Base64EncodedValue>?=`。
 */

/**
 * @brief 标准 Base64 编码（RFC 4648）。
 */
std::string base64Encode(const std::string& data);

/**
 * @brief 判断字符串是否可安全地直接作为 HTTP header 值原样发送。
 *
 * RFC 9110 规定 header field value 只能包含可见 ASCII (0x21-0x7E)、
 * space (0x20) 与 horizontal tab (0x09)。此外，命中 sentinel 模式
 * （以 `=?base64?` 开头且以 `?=` 结尾）的纯 ASCII 值也必须编码以避免歧义。
 */
bool isHeaderSafeAscii(const std::string& value);

/**
 * @brief 对 MCP header 值进行规范编码：安全 ASCII 原样返回，否则 Base64 sentinel。
 */
std::string mcpHeaderEncodeValue(const std::string& value);

} // namespace mcp
