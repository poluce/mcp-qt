#pragma once
#include <string>
#include <regex>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief Representation of an MCP Tool that can be called by an LLM Client.
 */
struct McpTool {
    std::string name;
    std::string description;
    json inputSchema; // JSON schema describing parameters
};

inline bool isValidToolName(const std::string& name) {
    if (name.empty() || name.length() > 128) {
        return false;
    }
    static const std::regex pattern("^[a-zA-Z0-9_.-]+$");
    return std::regex_match(name, pattern);
}

inline void to_json(json& j, const McpTool& tool) {
    j = json{
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", tool.inputSchema}
    };
}

inline void from_json(const json& j, McpTool& tool) {
    j.at("name").get_to(tool.name);
    j.at("description").get_to(tool.description);
    if (j.contains("inputSchema")) {
        tool.inputSchema = j.at("inputSchema");
    } else {
        tool.inputSchema = json::object();
    }
}

} // namespace mcp

