#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief MCP Tasks 扩展（SEP-2663, io.modelcontextprotocol/tasks）的任务值类型。
 *
 * 服务器可在受支持请求（当前为 tools/call）的响应中返回
 * `CreateTaskResult`（resultType: "task"）代替标准结果，表示该请求将异步执行。
 * 客户端随后通过 tasks/get 轮询、tasks/update 提交输入、tasks/cancel 取消。
 *
 * 状态机（Task Status）：
 *   working        — 正在处理
 *   input_required — 等待客户端输入（tasks/get 响应携带 inputRequests）
 *   completed      — 成功完成（result 字段携带最终结果；工具级 isError:true 也属此态）
 *   failed         — JSON-RPC 协议错误（error 字段携带错误详情）
 *   cancelled      — 已取消
 *
 * 派生形态（DetailedTask）按状态内联不同字段：
 *   WorkingTask / InputRequiredTask(+inputRequests) / CompletedTask(+result) /
 *   FailedTask(+error) / CancelledTask
 */
struct McpTask {
    enum class Status {
        Working,
        InputRequired,
        Completed,
        Cancelled,
        Failed,
        Unknown
    };

    std::string taskId;            // 服务器生成的稳定任务标识
    Status status{Status::Unknown};
    std::string statusMessage;     // 可选：当前状态描述（进度/阻塞原因/失败详情等）
    std::string createdAt;         // ISO 8601 创建时间
    std::string lastUpdatedAt;     // ISO 8601 最后更新时间
    int64_t ttlMs{-1};             // 自创建起的存活时长（毫秒）；-1 = null（无限制）
    int64_t pollIntervalMs{-1};    // 建议轮询间隔（毫秒）；-1 = 未提供
    json inputRequests;            // status == input_required：InputRequests map（key -> {method, params}）
    json result;                   // status == completed：最终结果（结构同原请求的标准结果）
    json error;                    // status == failed：JSON-RPC 错误对象
    json raw;                      // 完整原始 JSON（永不丢弃）

    static Status statusFromString(const std::string& s) {
        if (s == "working") return Status::Working;
        if (s == "input_required") return Status::InputRequired;
        if (s == "completed") return Status::Completed;
        if (s == "cancelled") return Status::Cancelled;
        if (s == "failed") return Status::Failed;
        return Status::Unknown;
    }

    static std::string statusToString(Status s) {
        switch (s) {
            case Status::Working: return "working";
            case Status::InputRequired: return "input_required";
            case Status::Completed: return "completed";
            case Status::Cancelled: return "cancelled";
            case Status::Failed: return "failed";
            default: return "unknown";
        }
    }

    /// 终态：completed / cancelled / failed。到达终态后客户端可停止轮询。
    bool isTerminal() const {
        return status == Status::Completed || status == Status::Cancelled || status == Status::Failed;
    }

    bool empty() const { return taskId.empty(); }

    json toJson() const {
        json j;
        if (!taskId.empty()) j["taskId"] = taskId;
        j["status"] = statusToString(status);
        if (!statusMessage.empty()) j["statusMessage"] = statusMessage;
        if (!createdAt.empty()) j["createdAt"] = createdAt;
        if (!lastUpdatedAt.empty()) j["lastUpdatedAt"] = lastUpdatedAt;
        if (ttlMs >= 0) j["ttlMs"] = ttlMs;
        if (pollIntervalMs >= 0) j["pollIntervalMs"] = pollIntervalMs;
        if (!inputRequests.empty()) j["inputRequests"] = inputRequests;
        if (!result.empty()) j["result"] = result;
        if (!error.empty()) j["error"] = error;
        return j;
    }

    static McpTask fromJson(const json& j) {
        McpTask t;
        if (!j.is_object()) return t;
        t.raw = j;
        if (j.contains("taskId") && j["taskId"].is_string()) t.taskId = j["taskId"].get<std::string>();
        if (j.contains("status") && j["status"].is_string()) t.status = statusFromString(j["status"].get<std::string>());
        if (j.contains("statusMessage") && j["statusMessage"].is_string()) t.statusMessage = j["statusMessage"].get<std::string>();
        if (j.contains("createdAt") && j["createdAt"].is_string()) t.createdAt = j["createdAt"].get<std::string>();
        if (j.contains("lastUpdatedAt") && j["lastUpdatedAt"].is_string()) t.lastUpdatedAt = j["lastUpdatedAt"].get<std::string>();
        if (j.contains("ttlMs") && j["ttlMs"].is_number_integer()) t.ttlMs = j["ttlMs"].get<int64_t>();
        if (j.contains("pollIntervalMs") && j["pollIntervalMs"].is_number_integer()) t.pollIntervalMs = j["pollIntervalMs"].get<int64_t>();
        if (j.contains("inputRequests") && j["inputRequests"].is_object()) t.inputRequests = j["inputRequests"];
        if (j.contains("result") && j["result"].is_object()) t.result = j["result"];
        if (j.contains("error") && j["error"].is_object()) t.error = j["error"];
        return t;
    }
};

} // namespace mcp
