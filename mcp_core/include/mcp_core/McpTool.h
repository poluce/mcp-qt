#pragma once
#include <string>
#include <regex>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief Optional annotations for an MCP Tool, providing metadata about tool behavior.
 */
struct ToolAnnotations {
    std::string title;                  // Human-readable display title
    bool readOnlyHint = false;          // Tool does not modify its environment
    bool destructiveHint = true;        // Tool may perform destructive updates
    bool idempotentHint = false;        // Repeated calls have no additional effect
    bool openWorldHint = true;          // Tool interacts with external entities
    std::string description;            // Additional description for the tool

    json toJson() const {
        json j;
        if (!title.empty()) j["title"] = title;
        j["readOnlyHint"] = readOnlyHint;
        j["destructiveHint"] = destructiveHint;
        j["idempotentHint"] = idempotentHint;
        j["openWorldHint"] = openWorldHint;
        if (!description.empty()) j["description"] = description;
        return j;
    }

    static ToolAnnotations fromJson(const json& j) {
        ToolAnnotations a;
        if (j.contains("title")) a.title = j["title"].get<std::string>();
        if (j.contains("readOnlyHint")) a.readOnlyHint = j["readOnlyHint"].get<bool>();
        if (j.contains("destructiveHint")) a.destructiveHint = j["destructiveHint"].get<bool>();
        if (j.contains("idempotentHint")) a.idempotentHint = j["idempotentHint"].get<bool>();
        if (j.contains("openWorldHint")) a.openWorldHint = j["openWorldHint"].get<bool>();
        if (j.contains("description")) a.description = j["description"].get<std::string>();
        return a;
    }
};

/**
 * @brief Representation of an MCP Tool that can be called by an LLM Client.
 */
struct McpTool {
    std::string name;
    std::string description;
    json inputSchema; // JSON schema describing parameters
    ToolAnnotations annotations; // Optional behavioral annotations
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
    // 仅在 annotations 非空时序列化，避免污染无 annotations 的工具
    if (!tool.annotations.title.empty() || tool.annotations.readOnlyHint ||
        !tool.annotations.destructiveHint || tool.annotations.idempotentHint ||
        !tool.annotations.openWorldHint || !tool.annotations.description.empty()) {
        j["annotations"] = tool.annotations.toJson();
    }
}

inline void from_json(const json& j, McpTool& tool) {
    j.at("name").get_to(tool.name);
    j.at("description").get_to(tool.description);
    if (j.contains("inputSchema")) {
        tool.inputSchema = j.at("inputSchema");
    } else {
        tool.inputSchema = json::object();
    }
    if (j.contains("annotations")) {
        tool.annotations = ToolAnnotations::fromJson(j["annotations"]);
    }
}

} // namespace mcp

