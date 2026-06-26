#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief An argument that a prompt template accepts.
 */
struct McpPromptArgument {
    std::string name;           // Argument name
    std::string description;    // What the argument is for
    bool required = false;      // Whether the argument is required

    json toJson() const {
        json j = {{"name", name}};
        if (!description.empty()) j["description"] = description;
        if (required) j["required"] = true;
        return j;
    }

    static McpPromptArgument fromJson(const json& j) {
        McpPromptArgument a;
        a.name = j.value("name", "");
        a.description = j.value("description", "");
        a.required = j.value("required", false);
        return a;
    }
};

/**
 * @brief Describes a prompt template exposed by an MCP server.
 */
struct McpPrompt {
    std::string name;                           // Unique identifier for the prompt
    std::string description;                    // Description of what the prompt does
    std::vector<McpPromptArgument> arguments;   // Arguments the prompt accepts

    json toJson() const {
        json j = {{"name", name}};
        if (!description.empty()) j["description"] = description;
        if (!arguments.empty()) {
            json args = json::array();
            for (const auto& arg : arguments) {
                args.push_back(arg.toJson());
            }
            j["arguments"] = args;
        }
        return j;
    }

    static McpPrompt fromJson(const json& j) {
        McpPrompt p;
        p.name = j.value("name", "");
        p.description = j.value("description", "");
        if (j.contains("arguments") && j["arguments"].is_array()) {
            for (const auto& arg : j["arguments"]) {
                p.arguments.push_back(McpPromptArgument::fromJson(arg));
            }
        }
        return p;
    }
};

/**
 * @brief A message within a prompt result.
 */
struct McpPromptMessage {
    std::string role;   // "user" or "assistant"
    json content;       // TextContent, ImageContent, EmbeddedResource, etc.

    json toJson() const {
        return {{"role", role}, {"content", content}};
    }

    static McpPromptMessage fromJson(const json& j) {
        McpPromptMessage m;
        m.role = j.value("role", "user");
        m.content = j.contains("content") ? j["content"] : json::object();
        return m;
    }
};

/**
 * @brief Result of a prompts/get request.
 */
struct McpPromptResult {
    std::string description;                    // Description of the prompt
    std::vector<McpPromptMessage> messages;     // The prompt messages

    json toJson() const {
        json j;
        if (!description.empty()) j["description"] = description;
        json msgs = json::array();
        for (const auto& msg : messages) {
            msgs.push_back(msg.toJson());
        }
        j["messages"] = msgs;
        return j;
    }

    static McpPromptResult fromJson(const json& j) {
        McpPromptResult r;
        r.description = j.value("description", "");
        if (j.contains("messages") && j["messages"].is_array()) {
            for (const auto& msg : j["messages"]) {
                r.messages.push_back(McpPromptMessage::fromJson(msg));
            }
        }
        return r;
    }
};

} // namespace mcp
