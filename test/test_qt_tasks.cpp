#include "tests/common.h"
#include "mcp_core/McpClientSession.h"
#include "mcp_core/McpTask.h"
#include "mcp_qt_client/McpQtClient.h"
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <string>
#include <memory>
#include <vector>
#include <map>

using json = nlohmann::json;

// ============================================================================
// TasksMockTransport：内存 Mock 传输，支持按 method 脚本化自动响应
// ============================================================================
class TasksMockTransport : public mcp::IMcpTransport {
public:
    std::vector<std::string> sentMessages;
    std::function<void(const std::string&)> onMsgCb;
    // 脚本：method -> 依次消费的 result 队列（空队列 = 不自动响应）
    std::map<std::string, std::vector<json>> script;

    bool send(const std::string& message) override {
        sentMessages.push_back(message);
        json req;
        try {
            req = json::parse(message);
        } catch (...) {
            return true;
        }
        std::string method = req.value("method", "");
        auto it = script.find(method);
        if (it != script.end() && !it->second.empty()) {
            json result = it->second.front();
            it->second.erase(it->second.begin());
            json resp = {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", result}};
            if (onMsgCb) onMsgCb(resp.dump());
        }
        return true;
    }
    void setOnMessage(std::function<void(const std::string&)> cb) override { onMsgCb = std::move(cb); }
    void setOnClose(std::function<void()>) override {}
    void setOnError(std::function<void(const std::string&)>) override {}
    bool start() override { return true; }
    void close() override {}
};

// 等待异步回调完成（带超时保护）
static bool waitForFlag(bool* flag, int timeoutMs = 3000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    while (!*flag) {
        loop.processEvents(QEventLoop::AllEvents, 50);
        if (!timer.isActive()) break;  // 超时
    }
    return *flag;
}

// ============================================================================
// 1. 能力声明：extensions.io.modelcontextprotocol/tasks 注入 _meta
// ============================================================================
void test_qt_tasks_capability_declaration() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mock);
    session->init();
    session->start();
    session->setStatelessMode(true);

    // 声明 Tasks 扩展能力（SEP-2133 扩展框架）
    session->registerCapabilities({{"extensions", {{"io.modelcontextprotocol/tasks", json::object()}}}});

    session->listTools([](const std::vector<mcp::McpTool>&, const json&) {});

    TM_ASSERT_TRUE(mock->sentMessages.size() == 1, "one request should be sent");
    auto req = json::parse(mock->sentMessages.back());
    auto meta = req["params"]["_meta"];
    TM_ASSERT_TRUE(meta.contains("io.modelcontextprotocol/clientCapabilities"),
                   "_meta should carry clientCapabilities");
    auto caps = meta["io.modelcontextprotocol/clientCapabilities"];
    TM_ASSERT_TRUE(caps.contains("extensions"), "clientCapabilities should contain extensions");
    TM_ASSERT_TRUE(caps["extensions"].contains("io.modelcontextprotocol/tasks"),
                   "extensions should declare io.modelcontextprotocol/tasks");
}

// ============================================================================
// 2. tasks/get：wire 格式 + DetailedTask 各状态解析
// ============================================================================
void test_qt_tasks_get_wire_and_parse() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mock);
    session->init();
    session->start();
    session->setStatelessMode(true);

    bool got = false;
    mcp::McpTask parsed;
    session->getTask("task-1", [&got, &parsed](const mcp::McpTask& task, const json& error) {
        got = true;
        parsed = task;
        TM_ASSERT_TRUE(error.empty(), "getTask should not report error");
    });

    TM_ASSERT_TRUE(mock->sentMessages.size() == 1, "one tasks/get request should be sent");
    auto req = json::parse(mock->sentMessages.back());
    TM_ASSERT_TRUE(req["method"] == "tasks/get", "method should be tasks/get");
    TM_ASSERT_TRUE(req["params"]["taskId"] == "task-1", "params.taskId should match");

    // 注入 completed 终态响应（resultType MUST 为 complete）
    json completedResp = {
        {"jsonrpc", "2.0"},
        {"id", req["id"]},
        {"result", {
            {"resultType", "complete"},
            {"taskId", "task-1"},
            {"status", "completed"},
            {"statusMessage", "Done"},
            {"createdAt", "2026-07-28T10:00:00Z"},
            {"lastUpdatedAt", "2026-07-28T10:01:00Z"},
            {"ttlMs", 60000},
            {"pollIntervalMs", 5000},
            {"result", {
                {"content", json::array({{
                    {"type", "text"},
                    {"text", "Hello, Luca!"}
                }})},
                {"isError", false}
            }}
        }}
    };
    mock->onMsgCb(completedResp.dump());

    TM_ASSERT_TRUE(got, "getTask callback should fire");
    TM_ASSERT_TRUE(parsed.taskId == "task-1", "taskId should be parsed");
    TM_ASSERT_TRUE(parsed.status == mcp::McpTask::Status::Completed, "status should be completed");
    TM_ASSERT_TRUE(parsed.isTerminal(), "completed is terminal");
    TM_ASSERT_TRUE(parsed.ttlMs == 60000, "ttlMs should be parsed");
    TM_ASSERT_TRUE(parsed.pollIntervalMs == 5000, "pollIntervalMs should be parsed");
    TM_ASSERT_TRUE(parsed.result.contains("content"), "completed task should carry result");
    TM_ASSERT_TRUE(parsed.raw.contains("resultType"), "raw should preserve full JSON");

    // input_required 变体：携带 inputRequests
    bool got2 = false;
    mcp::McpTask parsed2;
    session->getTask("task-2", [&got2, &parsed2](const mcp::McpTask& task, const json&) {
        got2 = true;
        parsed2 = task;
    });
    auto req2 = json::parse(mock->sentMessages.back());
    json inputRequiredResp = {
        {"jsonrpc", "2.0"},
        {"id", req2["id"]},
        {"result", {
            {"resultType", "complete"},
            {"taskId", "task-2"},
            {"status", "input_required"},
            {"createdAt", "2026-07-28T10:00:00Z"},
            {"lastUpdatedAt", "2026-07-28T10:00:30Z"},
            {"inputRequests", {
                {"name", {
                    {"method", "elicitation/create"},
                    {"params", {
                        {"mode", "form"},
                        {"message", "Please enter your name."},
                        {"requestedSchema", {
                            {"type", "object"},
                            {"properties", {{"name", {{"type", "string"}}}}},
                            {"required", {"name"}}
                        }}
                    }}
                }}
            }}
        }}
    };
    mock->onMsgCb(inputRequiredResp.dump());

    TM_ASSERT_TRUE(got2, "second getTask callback should fire");
    TM_ASSERT_TRUE(parsed2.status == mcp::McpTask::Status::InputRequired, "status should be input_required");
    TM_ASSERT_TRUE(!parsed2.isTerminal(), "input_required is not terminal");
    TM_ASSERT_TRUE(parsed2.inputRequests.contains("name"), "inputRequests should be parsed");
    TM_ASSERT_TRUE(parsed2.inputRequests["name"]["method"] == "elicitation/create",
                   "inputRequest method should be parsed");

    // failed 变体：携带 JSON-RPC error
    bool got3 = false;
    mcp::McpTask parsed3;
    session->getTask("task-3", [&got3, &parsed3](const mcp::McpTask& task, const json&) {
        got3 = true;
        parsed3 = task;
    });
    auto req3 = json::parse(mock->sentMessages.back());
    json failedResp = {
        {"jsonrpc", "2.0"},
        {"id", req3["id"]},
        {"result", {
            {"resultType", "complete"},
            {"taskId", "task-3"},
            {"status", "failed"},
            {"statusMessage", "Tool execution failed: API rate limit exceeded"},
            {"createdAt", "2026-07-28T10:00:00Z"},
            {"lastUpdatedAt", "2026-07-28T10:00:40Z"},
            {"error", {{"code", -32603}, {"message", "API rate limit exceeded"}}}
        }}
    };
    mock->onMsgCb(failedResp.dump());

    TM_ASSERT_TRUE(got3, "third getTask callback should fire");
    TM_ASSERT_TRUE(parsed3.status == mcp::McpTask::Status::Failed, "status should be failed");
    TM_ASSERT_TRUE(parsed3.isTerminal(), "failed is terminal");
    TM_ASSERT_TRUE(parsed3.error["code"] == -32603, "error code should be parsed");
}

// ============================================================================
// 3. tasks/update + tasks/cancel：wire 格式 + ack-only 确认
// ============================================================================
void test_qt_tasks_update_cancel_wire() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mock);
    session->init();
    session->start();
    session->setStatelessMode(true);

    // tasks/update：提交 inputResponses
    bool updated = false;
    session->updateTask("task-1", {{"name", {{"action", "accept"}, {"content", {{"input", "Luca"}}}}}},
                        [&updated](bool success, const json& error) {
        updated = success;
        TM_ASSERT_TRUE(error.empty(), "updateTask should not report error");
    });

    TM_ASSERT_TRUE(mock->sentMessages.size() == 1, "one tasks/update request should be sent");
    auto req = json::parse(mock->sentMessages.back());
    TM_ASSERT_TRUE(req["method"] == "tasks/update", "method should be tasks/update");
    TM_ASSERT_TRUE(req["params"]["taskId"] == "task-1", "params.taskId should match");
    TM_ASSERT_TRUE(req["params"]["inputResponses"]["name"]["action"] == "accept",
                   "inputResponses should be carried");

    // ack-only 确认（resultType: complete，空结果）
    json ack = {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", {{"resultType", "complete"}}}};
    mock->onMsgCb(ack.dump());
    TM_ASSERT_TRUE(updated, "updateTask should succeed on ack");

    // tasks/cancel
    bool cancelled = false;
    session->cancelTask("task-1", [&cancelled](bool success, const json& error) {
        cancelled = success;
        TM_ASSERT_TRUE(error.empty(), "cancelTask should not report error");
    });

    TM_ASSERT_TRUE(mock->sentMessages.size() == 2, "one tasks/cancel request should be sent");
    auto req2 = json::parse(mock->sentMessages.back());
    TM_ASSERT_TRUE(req2["method"] == "tasks/cancel", "method should be tasks/cancel");
    TM_ASSERT_TRUE(req2["params"]["taskId"] == "task-1", "params.taskId should match");

    json ack2 = {{"jsonrpc", "2.0"}, {"id", req2["id"]}, {"result", {{"resultType", "complete"}}}};
    mock->onMsgCb(ack2.dump());
    TM_ASSERT_TRUE(cancelled, "cancelTask should succeed on ack");

    // 未知 taskId：-32602 协议错误
    bool errGot = false;
    session->getTask("ghost", [&errGot](const mcp::McpTask&, const json& error) {
        errGot = !error.empty();
    });
    auto req3 = json::parse(mock->sentMessages.back());
    json errResp = {{"jsonrpc", "2.0"}, {"id", req3["id"]},
                    {"error", {{"code", -32602}, {"message", "Failed to retrieve task: Task not found"}}}};
    mock->onMsgCb(errResp.dump());
    TM_ASSERT_TRUE(errGot, "unknown taskId should surface protocol error");
}

// ============================================================================
// 4. notifications/tasks：任务状态通知派发
// ============================================================================
void test_qt_tasks_notifications() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mock);
    session->init();
    session->start();
    session->setStatelessMode(true);

    bool notified = false;
    json notifParams;
    session->registerNotificationHandler("notifications/tasks", [&notified, &notifParams](const json& params) {
        notified = true;
        notifParams = params;
    });

    mock->onMsgCb(R"({
        "jsonrpc": "2.0",
        "method": "notifications/tasks",
        "params": {
            "taskId": "786512e2-9e0d-44bd-8f29-789f320fe840",
            "status": "completed",
            "createdAt": "2025-11-25T10:30:00Z",
            "lastUpdatedAt": "2025-11-25T10:50:00Z",
            "ttlMs": 60000,
            "pollIntervalMs": 5000,
            "result": {
                "content": [{"type": "text", "text": "Operation completed successfully."}],
                "isError": false
            }
        }
    })");

    TM_ASSERT_TRUE(notified, "notifications/tasks handler should fire");
    TM_ASSERT_TRUE(notifParams["taskId"] == "786512e2-9e0d-44bd-8f29-789f320fe840", "taskId should match");
    TM_ASSERT_TRUE(notifParams["status"] == "completed", "status should be completed");
    TM_ASSERT_TRUE(notifParams["result"]["content"][0]["text"] == "Operation completed successfully.",
                   "notification should carry full task state");
}

// ============================================================================
// 5. 客户端透明轮询：callTool 收到 CreateTaskResult 后自动轮询到终态
// ============================================================================
void test_qt_tasks_calltool_transparent_poll() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto client = mcp_qt::McpQtClient::createForTest();
    client->setStatelessMode(true);
    client->registerMcpTaskCapabilities();

    // server/discover（连接握手，stateless 下 OPTIONAL 但立即响应避免 3s 等待）
    mock->script["server/discover"] = {{
        {"resultType", "complete"},
        {"supportedVersions", {"2026-07-28"}},
        {"capabilities", {{"tools", json::object()}}}
    }};
    // tools/call → CreateTaskResult（working，pollIntervalMs=10 加速测试）
    mock->script["tools/call"] = {{
        {"resultType", "task"},
        {"taskId", "task-poll-1"},
        {"status", "working"},
        {"createdAt", "2026-07-28T10:00:00Z"},
        {"lastUpdatedAt", "2026-07-28T10:00:00Z"},
        {"ttlMs", 60000},
        {"pollIntervalMs", 10}
    }};
    // tasks/get 轮询：第一次 working，第二次 completed
    mock->script["tasks/get"] = {
        {
            {"resultType", "complete"},
            {"taskId", "task-poll-1"},
            {"status", "working"},
            {"createdAt", "2026-07-28T10:00:00Z"},
            {"lastUpdatedAt", "2026-07-28T10:00:01Z"},
            {"pollIntervalMs", 10}
        },
        {
            {"resultType", "complete"},
            {"taskId", "task-poll-1"},
            {"status", "completed"},
            {"createdAt", "2026-07-28T10:00:00Z"},
            {"lastUpdatedAt", "2026-07-28T10:00:02Z"},
            {"result", {
                {"content", json::array({{{"type", "text"}, {"text", "Final result after polling"}}})},
                {"isError", false}
            }}
        }
    };

    QString err;
    bool connected = client->connectToTransportAndWait(mock, QStringLiteral("tasks-test"), QStringLiteral("1.0"), 5000, &err);
    TM_ASSERT_TRUE(connected, "client should connect to mock transport");

    bool done = false;
    mcp_qt::McpResult finalRes;
    client->callToolAsync(QStringLiteral("long_task"), QJsonObject{}, [&done, &finalRes](mcp_qt::McpResult r) {
        done = true;
        finalRes = r;
    });

    TM_ASSERT_TRUE(waitForFlag(&done), "transparent polling should complete");
    TM_ASSERT_TRUE(!finalRes.isError, "final result should not be an error");
    TM_ASSERT_TRUE(finalRes.data.value(QStringLiteral("content")).toArray().size() == 1,
                   "final result should carry tool result content");
    TM_ASSERT_TRUE(finalRes.contents.size() == 1, "contents should be parsed");
    TM_ASSERT_TRUE(finalRes.contents[0].text == QStringLiteral("Final result after polling"),
                   "final text should match completed task result");

    // 验证轮询确实发生：tools/call + 2 次 tasks/get
    int tasksGetCount = 0;
    for (const auto& msg : mock->sentMessages) {
        auto j = json::parse(msg);
        if (j.value("method", "") == "tasks/get") tasksGetCount++;
    }
    TM_ASSERT_TRUE(tasksGetCount == 2, "client should poll tasks/get until terminal (got " + std::to_string(tasksGetCount) + ")");
}

// ============================================================================
// 6. callToolTaskAsync：直接返回任务句柄，不自动轮询
// ============================================================================
void test_qt_tasks_calltool_task_handle() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto client = mcp_qt::McpQtClient::createForTest();
    client->setStatelessMode(true);
    client->registerMcpTaskCapabilities();

    mock->script["server/discover"] = {{
        {"resultType", "complete"},
        {"supportedVersions", {"2026-07-28"}},
        {"capabilities", {{"tools", json::object()}}}
    }};
    mock->script["tools/call"] = {{
        {"resultType", "task"},
        {"taskId", "task-handle-1"},
        {"status", "working"},
        {"createdAt", "2026-07-28T10:00:00Z"},
        {"lastUpdatedAt", "2026-07-28T10:00:00Z"},
        {"ttlMs", 3600000},
        {"pollIntervalMs", 5000}
    }};

    QString err;
    bool connected = client->connectToTransportAndWait(mock, QStringLiteral("tasks-test"), QStringLiteral("1.0"), 5000, &err);
    TM_ASSERT_TRUE(connected, "client should connect to mock transport");

    bool done = false;
    mcp_qt::McpQtTask handle;
    client->callToolTaskAsync(QStringLiteral("long_task"), QJsonObject{},
        [&done, &handle](const mcp_qt::McpQtTask& task, const mcp_qt::McpResult&, const QString&) {
            done = true;
            handle = task;
        });

    TM_ASSERT_TRUE(waitForFlag(&done), "callToolTaskAsync should complete");
    TM_ASSERT_TRUE(!handle.empty(), "task handle should be returned");
    TM_ASSERT_TRUE(handle.taskId == QStringLiteral("task-handle-1"), "taskId should match");
    TM_ASSERT_TRUE(handle.status == QStringLiteral("working"), "status should be working");
    TM_ASSERT_TRUE(!handle.isTerminal(), "working is not terminal");

    // 不应发生任何 tasks/get 轮询
    for (const auto& msg : mock->sentMessages) {
        auto j = json::parse(msg);
        TM_ASSERT_TRUE(j.value("method", "") != "tasks/get", "callToolTaskAsync must not poll");
    }
}

// ============================================================================
// 7. 客户端同步 API：getTask / updateTask / cancelTask
// ============================================================================
void test_qt_tasks_sync_apis() {
    auto mock = std::make_shared<TasksMockTransport>();
    auto client = mcp_qt::McpQtClient::createForTest();
    client->setStatelessMode(true);
    client->registerMcpTaskCapabilities();

    mock->script["server/discover"] = {{
        {"resultType", "complete"},
        {"supportedVersions", {"2026-07-28"}},
        {"capabilities", {{"tools", json::object()}}}
    }};
    mock->script["tasks/get"] = {{
        {"resultType", "complete"},
        {"taskId", "task-sync-1"},
        {"status", "completed"},
        {"createdAt", "2026-07-28T10:00:00Z"},
        {"lastUpdatedAt", "2026-07-28T10:00:05Z"},
        {"result", {
            {"content", json::array({{{"type", "text"}, {"text", "sync done"}}})},
            {"isError", false}
        }}
    }};
    mock->script["tasks/update"] = {{{"resultType", "complete"}}};
    mock->script["tasks/cancel"] = {{{"resultType", "complete"}}};

    QString err;
    bool connected = client->connectToTransportAndWait(mock, QStringLiteral("tasks-test"), QStringLiteral("1.0"), 5000, &err);
    TM_ASSERT_TRUE(connected, "client should connect to mock transport");

    mcp_qt::McpQtTask task = client->getTask(QStringLiteral("task-sync-1"), 5000);
    TM_ASSERT_TRUE(task.taskId == QStringLiteral("task-sync-1"), "sync getTask should return task");
    TM_ASSERT_TRUE(task.status == QStringLiteral("completed"), "sync getTask should parse status");
    TM_ASSERT_TRUE(task.isTerminal(), "sync getTask should be terminal");

    bool updated = client->updateTask(QStringLiteral("task-sync-1"),
                                      QJsonObject{{QStringLiteral("name"), QJsonObject{{QStringLiteral("action"), QStringLiteral("accept")}}}},
                                      5000);
    TM_ASSERT_TRUE(updated, "sync updateTask should succeed");

    bool cancelled = client->cancelTask(QStringLiteral("task-sync-1"), 5000);
    TM_ASSERT_TRUE(cancelled, "sync cancelTask should succeed");
}
