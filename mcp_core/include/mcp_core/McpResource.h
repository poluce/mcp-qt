#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief Describes a resource exposed by an MCP server.
 */
struct McpResource {
    std::string uri;            // Unique identifier for the resource
    std::string name;           // Human-readable name
    std::string description;    // Description of what the resource represents
    std::string mimeType;       // MIME type of the resource content

    json toJson() const {
        json j = {{"uri", uri}, {"name", name}};
        if (!description.empty()) j["description"] = description;
        if (!mimeType.empty()) j["mimeType"] = mimeType;
        return j;
    }

    static McpResource fromJson(const json& j) {
        McpResource r;
        r.uri = j.value("uri", "");
        r.name = j.value("name", "");
        r.description = j.value("description", "");
        r.mimeType = j.value("mimeType", "");
        return r;
    }
};

/**
 * @brief Describes a resource template (URI with variables) exposed by an MCP server.
 */
struct McpResourceTemplate {
    std::string uriTemplate;    // URI template with variables (RFC 6570)
    std::string name;           // Human-readable name
    std::string description;    // Description of the resource template
    std::string mimeType;       // MIME type of resources created from this template

    json toJson() const {
        json j = {{"uriTemplate", uriTemplate}, {"name", name}};
        if (!description.empty()) j["description"] = description;
        if (!mimeType.empty()) j["mimeType"] = mimeType;
        return j;
    }

    static McpResourceTemplate fromJson(const json& j) {
        McpResourceTemplate t;
        t.uriTemplate = j.value("uriTemplate", "");
        t.name = j.value("name", "");
        t.description = j.value("description", "");
        t.mimeType = j.value("mimeType", "");
        return t;
    }
};

/**
 * @brief Contents of a resource read from the server.
 */
struct McpResourceContent {
    std::string uri;            // URI of the resource
    std::string mimeType;       // MIME type of this specific content
    std::string text;           // Text content (for text resources)
    std::string blob;           // Base64-encoded binary content (for binary resources)

    json toJson() const {
        json j = {{"uri", uri}};
        if (!mimeType.empty()) j["mimeType"] = mimeType;
        if (!text.empty()) j["text"] = text;
        if (!blob.empty()) j["blob"] = blob;
        return j;
    }

    static McpResourceContent fromJson(const json& j) {
        McpResourceContent c;
        c.uri = j.value("uri", "");
        c.mimeType = j.value("mimeType", "");
        c.text = j.value("text", "");
        c.blob = j.value("blob", "");
        return c;
    }
};

} // namespace mcp
