#include "mcp_core/McpHeaderEncoding.h"

namespace mcp {

std::string base64Encode(const std::string& data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    const size_t n = data.size();
    while (i + 3 <= n) {
        unsigned int v = (static_cast<unsigned char>(data[i]) << 16)
                       | (static_cast<unsigned char>(data[i + 1]) << 8)
                       | static_cast<unsigned char>(data[i + 2]);
        out.push_back(table[(v >> 18) & 0x3F]);
        out.push_back(table[(v >> 12) & 0x3F]);
        out.push_back(table[(v >> 6) & 0x3F]);
        out.push_back(table[v & 0x3F]);
        i += 3;
    }

    const size_t remain = n - i;
    if (remain == 1) {
        unsigned int v = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(table[(v >> 18) & 0x3F]);
        out.push_back(table[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remain == 2) {
        unsigned int v = (static_cast<unsigned char>(data[i]) << 16)
                       | (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(table[(v >> 18) & 0x3F]);
        out.push_back(table[(v >> 12) & 0x3F]);
        out.push_back(table[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool isHeaderSafeAscii(const std::string& value) {
    // 命中 sentinel 模式的值必须编码以避免歧义
    if (value.size() >= 9 &&
        value.compare(0, 8, "=?base64?") == 0 &&
        value.compare(value.size() - 2, 2, "?=") == 0) {
        return false;
    }
    // 规范 SEP-2243：leading/trailing whitespace 必须 Base64 编码
    if (!value.empty()) {
        const unsigned char front = static_cast<unsigned char>(value.front());
        const unsigned char back = static_cast<unsigned char>(value.back());
        if (front == 0x20 || front == 0x09 || back == 0x20 || back == 0x09) {
            return false;
        }
    }
    for (const char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        // visible ASCII (0x21-0x7E), space (0x20), horizontal tab (0x09)
        if (!(uc == 0x20 || uc == 0x09 || (uc >= 0x21 && uc <= 0x7E))) {
            return false;
        }
    }
    return true;
}

std::string mcpHeaderEncodeValue(const std::string& value) {
    if (isHeaderSafeAscii(value)) {
        return value;
    }
    return "=?base64?" + base64Encode(value) + "?=";
}

} // namespace mcp
