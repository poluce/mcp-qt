# mcp-qt API 参考手册

## 目录

- [快速开始](#快速开始)
- [McpQtClient —— 主客户端](#mcpqtclient--主客户端)
- [McpQtClientBuilder —— 构造器](#mcpqtclientbuilder--构造器)
- [同步 API](#同步-api)
- [异步 API](#异步-api)
- [类型化结果](#类型化结果)
- [双向能力](#双向能力)
- [MVC 模型绑定](#mvc-模型绑定)
- [聚合类 API](#聚合类-api)
  - [McpHost](#mcphost)
  - [McpServerManager](#mcpservermanager)
  - [McpToolRouter](#mcptoolrouter)
  - [McpPromptRouter](#mcppromptrouter)
  - [McpResourceRouter](#mcpresourcerouter)
  - [McpServerConfig](#mcpserverconfig)
  - [IMcpConfigLoader / McpJsonConfigLoader](#imcpconfigloader--mcpjsonconfigloader)
  - [McpResourceSubscriptionRouter](#mcpresourcesubscriptionrouter)
  - [McpDiagnosticReporter](#mcpdiagnosticreporter)
- [重连与自愈](#重连与自愈)
- [LLM 格式导出](#llm-格式导出)
- [错误处理](#错误处理)
- [完整示例](#完整示例)

---

## 快速开始

### 三种连接方式

```cpp
#include <mcp_qt_client/McpQtClient.h>

// --- HTTP/SSE 连接 ---
auto client = mcp_qt::McpQtClient::connectHttpAndWait(
    "http://localhost:8080/mcp"              // 服务器 URL
);

// --- Stdio 子进程连接 ---
auto client = mcp_qt::McpQtClient::connectStdioAndWait(
    "python",                                 // 命令
    {"server.py"}                             // 参数
);

// --- OAuth 认证连接 ---
mcp_qt::McpQtClient::OAuthConfig oa;
oa.serverUrl    = "https://secure-server.com/mcp";
oa.clientId     = "my-client-id";
oa.clientSecret = "my-secret";
auto client = mcp_qt::McpQtClient::connectWithOAuthAndWait(oa);
```

`*AndWait` 方法阻塞当前线程直到初始化握手完成。**仅限非 GUI 线程使用**。

GUI 线程用纯异步版本：

```cpp
// 异步连接（不阻塞）
auto client = mcp_qt::McpQtClient::connectHttpAsync("http://localhost:8080/mcp");
QObject::connect(client.get(), &McpQtClient::connected, []{
    // 连接就绪，可以开始调用 API
});
```

---

## McpQtClient —— 主客户端

`mcp_qt::McpQtClient` 继承 `QObject`，是 SDK 的唯一入口。每个客户端实例对应一个 MCP 服务器的连接。

**头文件**：`#include <mcp_qt_client/McpQtClient.h>`

### 工厂方法

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `connectHttpAndWait(url, name?, ver?, timeout?, err?)` | `Ptr` | HTTP/SSE 同步连接 |
| `connectHttpAsync(url, name?, ver?)` | `Ptr` | HTTP/SSE 异步连接 |
| `connectStdioAndWait(cmd, args?, name?, ver?, timeout?, err?)` | `Ptr` | Stdio 同步连接 |
| `connectStdioAsync(cmd, args?, name?, ver?)` | `Ptr` | Stdio 异步连接 |
| `connectWithOAuthAndWait(oauth, name?, ver?, timeout?)` | `Ptr` | OAuth 同步连接 |
| `connectWithOAuthAsync(oauth, name?, ver?)` | `Ptr` | OAuth 异步连接 |

`Ptr` = `std::shared_ptr<McpQtClient>`

### 信号

| 信号 | 参数 | 说明 |
|------|------|------|
| `connected()` | — | 连接已建立并完成初始化 |
| `disconnected()` | — | 连接已断开 |
| `errorOccurred(error)` | `McpError` | 传输或协议错误 |
| `notificationReceived(method, params)` | `QString`, `QJsonObject` | 收到服务端通知 |
| `toolsChanged(tools)` | `vector<McpQtTool>` | 服务端工具列表变更 |
| `toolsReady(tools)` | `vector<McpQtTool>` | `fetchAllToolsAsync()` 完成 |
| `resourcesChanged()` | — | 资源列表变更 |
| `promptsChanged()` | — | 提示词列表变更 |
| `toolCalled(name, args)` | `QString`, `QJsonObject` | 工具调用开始 |
| `toolFinished(name, result)` | `QString`, `McpResult` | 工具调用完成 |
| `progressReported(name, prog, total, msg)` | `QString`, `float`, `float`, `QString` | 工具进度更新 |
| `reconnecting()` | — | 开始重连 |
| `reconnected()` | — | 重连成功 |
| `recoveryFailed(msg)` | `QString` | 重连失败 |

### 服务器信息

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `serverInfo()` | `QJsonObject` | 服务器名称和版本 |
| `serverCapabilities()` | `QJsonObject` | 服务器能力列表 |
| `negotiatedProtocolVersion()` | `QString` | 协商后的协议版本 |
| `instructions()` | `QString` | 服务器使用说明 |
| `hasToolsCapability()` | `bool` | 服务器支持 tools |
| `hasPromptsCapability()` | `bool` | 服务器支持 prompts |
| `hasResourcesCapability()` | `bool` | 服务器支持 resources |
| `isConnected()` | `bool` | 连接是否活跃 |

---

## McpQtClientBuilder —— 构造器

当需要配置 HTTP headers、代理、重连策略、超时等时使用 Builder 模式：

```cpp
McpQtClientBuilder builder;
builder.setTransportHttp("http://localhost:8080/mcp")
       .setClientInfo("my-app", "1.0.0")
       .setHttpHeaders({{"Authorization", "Bearer xxx"}})
       .setHttpProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "proxy", 8080))
       .setTimeout(15000)
       .setReconnectPolicy(policy)
       .setEnvironment({{"PATH", "/usr/bin"}});

// 同步构建并连接
QString error;
auto client = builder.buildAndConnectAndWait(&error);

// 异步构建并连接
auto client = builder.buildAndConnectAsync();
```

| 方法 | 参数 | 说明 |
|------|------|------|
| `setTransportHttp(url)` | `QString` | 使用 HTTP/SSE 传输 |
| `setTransportStatelessHttp(url)` | `QString` | 使用无状态 HTTP 传输 |
| `setTransportStdio(cmd, args)` | `QString`, `QStringList` | 使用 Stdio 传输 |
| `setEnvironment(env)` | `QMap<QString,QString>` | 子进程环境变量 |
| `setNamespace(ns)` | `QString` | 工具名前缀命名空间 |
| `setClientInfo(name, ver)` | `QString`, `QString` | 客户端标识 |
| `setTimeout(ms)` | `int` | 默认超时毫秒数 |
| `setHttpHeaders(headers)` | `QMap<QString,QString>` | 自定义 HTTP 请求头 |
| `setHttpProxy(proxy)` | `QNetworkProxy` | HTTP 代理服务器 |
| `setReconnectPolicy(policy)` | `McpReconnectPolicy` | 重连策略 |
| `buildAndConnectAndWait(err)` | → `Ptr` | 同步构建并连接 |
| `buildAndConnectAsync()` | → `Ptr` | 异步构建并连接 |

---

## 同步 API

> ⚠️ **重要警告：不可在 GUI 主线程调用同步 API**
>
> 所有同步方法内部通过 `QEventLoop::exec()` 嵌套阻塞当前线程，等待网络响应或超时返回。
> 如果在 GUI 主线程（`QApplication`/`QGuiApplication` 的事件循环线程）上调用：
>
> - **UI 冻结** — 应用窗口无响应、无法重绘、点击事件堆积
> - **嵌套事件循环风险** — `exec()` 中可能意外处理本不该在此刻处理的事件，导致 reentrancy 和数据不一致
> - **Debug 模式下有 `qWarning` 日志警告**，但 Release 模式无检测（`QT_NO_DEBUG` 时 `assertNotMainGuiThread()` 为空函数）
>
> **正确做法**：
> - GUI 主线程 → 使用[异步 API](#异步-api)（`callToolAsync`、`callToolFuture`、回调、信号槽）
> - 非 GUI 线程（`QThread::create()`、`QtConcurrent::run()`、后台 worker）→ 同步 API 完全安全

所有同步方法在内部运行局部 `QEventLoop` 阻塞等待，**不可在 GUI 线程调用**。

### Tools

```cpp
// 获取工具列表（单页）
std::vector<McpQtTool> tools = client->listTools(timeoutMs);

// 分页获取
QString nextCursor;
auto page1 = client->listTools("", &nextCursor);
auto page2 = client->listTools(nextCursor, &nextCursor);

// 自动拉取全部页
std::vector<McpQtTool> all = client->fetchAllTools(timeoutMs);

// 获取缓存（不触发网络请求）
std::vector<McpQtTool> cached = client->cachedTools();
```

`McpQtTool` 结构体：

```cpp
struct McpQtTool {
    QString name;           // 工具名称
    QString description;    // 描述
    QJsonObject inputSchema; // 参数 JSON Schema
};
```

### 调用工具

```cpp
// 基本调用
McpResult result = client->callTool("calculate_add",
    {{"a", 5}, {"b", 3}});

qDebug() << result.data;        // QJsonObject 返回值
qDebug() << result.isError;     // bool
qDebug() << result.errorString; // QString

// 带进度回调的调用
McpResult result = client->callTool("long_task", args,
    [](float progress, float total, const QString& msg) {
        qDebug() << progress/total*100 << "%:" << msg;
    });

// 类型化结果（自动解析 Base64 图片等）
McpQtToolResult typed = client->callToolTyped("gen_image", args);
```

### 参数校验

```cpp
QString error;
bool valid = client->validateToolArguments("calculate_add",
    {{"a", 5}, {"b", 3}}, &error);
```

### Resources

```cpp
// 列出资源
QJsonObject resources = client->listResources();

// 分页
QString cursor;
auto page = client->listResources("", &cursor);

// 拉取全部
QJsonObject allResources = client->fetchAllResources();

// 读取资源
QJsonObject content = client->readResource("file:///data/config.json");

// 订阅资源更新
bool ok = client->subscribeResource("file:///data/config.json");

// 取消订阅
bool ok = client->unsubscribeResource("file:///data/config.json");
```

### 资源订阅（回调版本）

```cpp
// 注册更新回调并订阅
int token = client->subscribeResource(
    "file:///data/config.json",
    [](const QString& uri, const QJsonObject& params) {
        qDebug() << "Resource updated:" << uri;
    });

// 通过 token 撤销
client->unsubscribeResourceByToken("file:///data/config.json", token);
```

### Resource Templates

```cpp
// 列出资源模板
auto templates = client->listResourceTemplates();

// 分页
QString cursor;
auto page = client->listResourceTemplates("", &cursor);

// 拉取全部
auto all = client->fetchAllResourceTemplates();
```

### Prompts

```cpp
// 列出提示词
QJsonObject prompts = client->listPrompts();

// 分页
QString cursor;
auto page = client->listPrompts("", &cursor);

// 拉取全部
QJsonObject all = client->fetchAllPrompts();

// 获取提示词模板
QJsonObject prompt = client->getPrompt("greeting", {{"name", "World"}});
```

### 其他

```cpp
// Ping 检测连通性
bool alive = client->ping(5000);

// 自动补全
QJsonObject completion = client->complete(
    {{"name", "my_template"}, {"argument", {{"name", "p", "value", "te"}}}});

// 设置服务端日志级别（2026-07-28 下自动改用 per-request logLevel）
bool ok = client->setLoggingLevel("debug");

// 🌟 MCP 2026-07-28 per-request logLevel（SEP-2577）：
// 在后续每个请求的 _meta 注入 io.modelcontextprotocol/logLevel，
// 服务端据此决定是否在该请求响应流上回传 notifications/message 日志。
// 传空字符串停止注入。仅 stateless / 2026-07-28 模式生效。
client->setRequestLogLevel("debug");   // 启用，请求级别 debug
client->setRequestLogLevel("");        // 停止注入

// 🌟 W3C Trace Context（SEP-414 / W3C Trace-Context）：
// 设置后 traceparent/tracestate/baggage 随每个 HTTP 请求发送，
// 使 MCP 服务器 span 嵌套进调用方已有的分布式 trace。
mcp_qt::McpTraceContext trace;
trace.traceparent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
trace.tracestate  = "congo=t61rcWkgMzE";
trace.baggage     = "userId=alice";
client->setTraceContext(trace);   // 可随时调用，立即更新后续请求头

// Builder 中也可配置：
McpQtClientBuilder builder;
builder.setRequestLogLevel("info")
       .setTraceContext(trace);

// 流量追踪（调试用）
client->setTrafficLogger([](const QJsonObject& event) {
    qDebug() << event["direction"].toString()
             << event["kind"].toString()
             << event["payload"].toObject();
});

// 注册自定义能力
client->registerCapability("my_feature", {{"enabled", true}});
```

### 自定义请求/通知

```cpp
// 发送任意 JSON-RPC 请求
int64_t id = client->sendRequest("custom/method", params,
    [](const QJsonObject& result, const QJsonObject& error) {
        if (!error.isEmpty()) { /* 处理错误 */ }
        else { /* 处理 result */ }
    });

// 取消请求
client->cancelRequest(id);

// 发送通知（无响应）
client->sendNotification("notifications/custom", params);
```

---

## 异步 API

所有异步方法不阻塞当前线程，适合 GUI 线程使用。

### 工具列表

```cpp
// 异步分页获取
client->listToolsAsync("",
    [](const std::vector<McpQtTool>& tools,
       const QString& nextCursor,
       const QString& error) {
        if (!error.isEmpty()) return;
        // 处理 tools
    });

// 自动拉取全部（完成后触发 toolsReady 信号）
client->fetchAllToolsAsync();

// 带回调的拉取全部
client->fetchAllToolsAsync([](const std::vector<McpQtTool>& tools) {
    // 所有工具已就绪
});
```

### 调用工具

```cpp
// 回调风格
client->callToolAsync("calculate_add", {{"a",5},{"b",3}},
    [](McpResult result) {
        qDebug() << result.data;
    });

// 带 context 生命周期保护（context 销毁后回调不执行）
client->callToolAsync("calculate_add", args,
    listView,  // QObject* context
    [](McpResult result) { /* ... */ });

// QFuture 风格
QFuture<McpResult> future = client->callToolFuture("query", args);

// 带进度的回调
client->callToolAsync("long_task", args,
    [](McpResult r) { qDebug() << "done"; },
    [](float p, float t, const QString& msg) {
        qDebug() << p/t*100 << "%";
    });

// 类型化异步
client->callToolTypedAsync("gen_image", args,
    [](McpQtToolResult result) {
        for (auto& c : result.content) {
            if (c.kind == McpQtContentKind::Image) {
                QImage img = QImage::fromData(c.binary);
            }
        }
    });
```

### 并发调用

```cpp
// 同时调用多个工具
std::vector<McpBatchCallRequest> requests = {
    {"tool_a", {{"x", 1}}},
    {"tool_b", {{"y", 2}}}
};

// 异步并发
client->callToolsConcurrentAsync(requests,
    [](const std::vector<McpBatchCallResult>& results) {
        for (auto& r : results) {
            qDebug() << r.name << "=>" << r.result.data;
        }
    });

// 同步并发（阻塞）
auto results = client->callToolsConcurrent(requests);
```

### 资源

```cpp
// 异步获取资源列表
client->listResourcesAsync("",
    [](const QJsonObject& result, const QString& nextCursor,
       const QString& error) { /* ... */ });

// 异步读资源
client->readResourceAsync("file:///data/config.json",
    [](const QJsonObject& result, const QString& error) { /* ... */ });

// 异步订阅
client->subscribeResourceAsync("file:///data/config.json",
    [](bool success, const QString& error) { /* ... */ });
```

### 提示词

```cpp
client->listPromptsAsync("",
    [](const QJsonObject& result, const QString& nextCursor,
       const QString& error) { /* ... */ });

client->getPromptAsync("greeting", {{"name", "World"}},
    [](const QJsonObject& result, const QString& error) { /* ... */ });
```

### Ping / Complete

```cpp
client->pingAsync([](bool success, const QString& error) {
    qDebug() << "Ping:" << success;
});

client->completeAsync(ref, argument,
    [](const QJsonObject& completion, const QString& error) { /* ... */ });
```

---

## 类型化结果

### McpResult

```cpp
struct McpResult {
    bool isError{false};           // 是否为工具错误
    QJsonObject data;              // 工具返回的完整 JSON
    QString errorString;           // 错误描述
    QList<McpContent> contents;    // 解析后的内容列表
};
```

### McpQtToolResult

```cpp
struct McpQtToolResult {
    QList<McpQtContent> content;       // 内容项列表
    QJsonObject structuredContent;     // MCP 2025-11-25 结构化输出
    QJsonObject raw;                   // 原始 JSON（永不丢弃）
    bool isError{false};
    QString errorString;
};
```

### McpQtContent

```cpp
struct McpQtContent {
    McpQtContentKind kind;  // Text / Image / EmbeddedResource / Unknown
    QString mimeType;       // MIME 类型
    QString text;           // 类型为 Text 时有效
    QByteArray binary;      // 类型为 Image 时有效（Base64 已自动解码）
    QJsonObject raw;        // 原始 JSON 始终保留
};
```

---

## 双向能力

### Sampling（服务端请求 LLM 推理）

```cpp
client->setSamplingHandler(
    [](const QJsonObject& params,
       std::function<void(const QJsonObject&, const QJsonObject&)> respond) {
        // params 包含模型请求信息
        // 调用 respond(result, error) 回应服务端
        QJsonObject result;
        result["model"] = "claude-3";
        result["role"] = "assistant";
        result["content"] = QJsonObject{{"type", "text"}, {"text", "..."}};
        respond(result, QJsonObject{});
    });

// 带 context 生命周期保护
client->setSamplingHandler(listView, handler);
```

### Elicitation（服务端请求用户输入）

```cpp
client->setElicitationHandler(
    [](const QJsonObject& params,
       std::function<void(const QJsonObject&, const QJsonObject&)> respond) {
        // params 包含表单或 URL 信息
        QJsonObject result;
        result["action"] = "accept";
        result["content"] = QJsonObject{{"userInput", "..."}};
        respond(result, QJsonObject{});
    });
```

### Roots（暴露文件系统根目录）

```cpp
client->setRootsProvider(
    [](std::function<void(const QJsonArray&, const QJsonObject&)> respond) {
        QJsonArray roots;
        QJsonObject root;
        root["uri"] = "file:///home/user/projects";
        root["name"] = "Projects";
        roots.append(root);
        respond(roots, QJsonObject{});
    });

// 通知服务端根目录已变化
client->notifyRootsListChanged();
```

---

## MVC 模型绑定

4 个 `QAbstractListModel` 子类，可直接绑定到 `QListView` 或 QML `ListView`。

### McpToolsModel

```cpp
auto model = client->createToolsModel(parent);
// 或 manual: auto* model = new McpToolsModel(parent); model->setClient(client.get());

listView->setModel(model.get()); // QWidget
model->refresh();                 // 拉取数据

// QML: model.count, model.refresh()

// 角色访问
auto idx = model->index(0);
QString name = model->data(idx, McpToolsModel::NameRole).toString();
QString desc = model->data(idx, McpToolsModel::DescriptionRole).toString();
QJsonObject schema = model->data(idx, McpToolsModel::InputSchemaRole).toJsonObject();
```

### McpPromptsModel

```cpp
auto model = client->createPromptsModel(parent);
// Roles: NameRole, DescriptionRole, ArgumentsRole
```

### McpResourcesModel

```cpp
auto model = client->createResourcesModel(parent);
// Roles: UriRole, NameRole, DescriptionRole, MimeTypeRole
```

### McpResourceTemplatesModel

```cpp
auto model = client->createResourceTemplatesModel(parent);
// Roles: UriTemplateRole, NameRole, DescriptionRole, MimeTypeRole
```

**所有 Model 均支持**：
- `canFetchMore()` / `fetchMore()` 分页懒加载
- 服务端列表变更时自动更新（通过监听 `notifications/*/list-changed`）
- 面向 QML 的 `count` 属性和 `refresh()` 方法

---

## 聚合类 API

聚合类用于管理多个 MCP 服务器：配置加载、连接启动、跨服务器工具/资源/提示词路由、诊断报告。

### McpHost

`McpHost` 是外观模式一站式入口，组合了多服务器聚合所需的全部组件。它是**使用多服务器场景的唯一推荐入口**——不需要直接操作 `McpServerManager` 或 Router。

**头文件**：`#include <mcp_qt_client/McpHost.h>`

#### 内部架构

`McpHost` 内部组合了 5 个核心组件，全部为 QObject 父子关系自动管理生命周期：

```
McpHost (QObject 外观)
 ├── McpServerManager   — 多 client 生命周期管理、心跳保活、工具预热
 ├── McpToolRouter      — 跨服务器工具路由（serverName_ 前缀策略）
 ├── McpPromptRouter    — 跨服务器提示词路由
 ├── McpResourceRouter  — 跨服务器资源路由
 ├── McpDiagnosticReporter — 按 stage 收集 Info/Warning/Error 诊断
 └── QTimer (watchdog)  — 启动看门狗，超时触发 finishStartup(false)
```

信号转发链路：`McpServerManager` 的信号（clientToolsChanged、clientStateChanged、allToolsReady、clientErrorOccurred）全部被 `McpHost` 连接并重新发射为 `globalXxx` / `hostReady` 信号。

#### 启动流程（完整时序）

调用 `start()` 后的内部执行顺序：

1. **清空诊断** — `m_reporter->clear()`，重置上次运行的错误
2. **过滤启用服务器** — 遍历 `m_loadedConfigs`，跳过 `disabled=true` 的配置
3. **启动看门狗定时器** — 超时后强制 `finishStartup(false, "Timeout")`
4. **委托 McpServerManager** — 通过 `FilteredConfigLoader`（只传递启用的配置）调用 `loadServers()`
5. **McpServerManager 并发连接** — 对所有启用服务器并发建立传输、初始化、`fetchAllToolsAsync`（工具预热）
6. **终态检测**：
   - 路径 A：`allToolsReady` 信号 → `finishStartup(true, ...)` ✓
   - 路径 B：server 逐个到达终态（Ready/Error），`checkReadyCondition()` 检测所有启用服务器是否都停止在 Pending/Connecting 之外 → `finishStartup(hasError?, ...)` 
   - 路径 C：`watchdogTimer` 超时 → `finishStartup(false, "Timeout")` ✗
7. **发射 hostReady(success, summaryMsg)** — 调用方连接此信号获知启动结果

`m_isStarting` 标志位保证重入安全——`start()` 期间再次调用会被忽略。

#### 配置与生命周期

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `loadConfigFromFile(path)` | `QString` 文件路径 | `bool` | 从 JSON 配置文件加载服务器列表。内部记录 `m_lastConfigPath` 供热重载使用。异常安全：JSON 解析异常被 catch 并记录到 `McpDiagnosticReporter` |
| `loadConfigFromJson(obj)` | `QJsonObject` | `bool` | 从 JSON 对象加载（无需文件），同样异常安全 |
| `addServerConfig(config)` | `McpServerConfig` | `void` | 手动添加单个服务器配置，追加到 `m_loadedConfigs`，默认 `enabled=true` |
| `clearConfig()` | — | `void` | 清除所有已加载的配置和 enabled 映射 |
| `start(timeoutMs)` | `int`（默认 30000） | `void` | 异步启动：过滤已启用配置 → 委托 McpServerManager → 启动看门狗。结果通过 `hostReady` 信号通知 |
| `stop()` | — | `void` | 停止看门狗 → 关闭启动标志 → `m_manager->closeAll()` |
| `restart(timeoutMs)` | `int`（默认 30000） | `void` | `stop()` → `start()` 的便捷组合 |
| `reloadConfigAndRestart(timeoutMs)` | `int`（默认 30000） | `bool` | 三步操作：`stop()` → `clearConfig()` → `loadConfigFromFile(m_lastConfigPath)` → `start()`。必须在先前通过 `loadConfigFromFile` 加载过配置时才有效（依赖 `m_lastConfigPath`），否则返回 false |

#### 服务器管理

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `serverNames()` | — | `QStringList` | 所有已注册的服务器名称（从 `m_loadedConfigs` 提取） |
| `setServerEnabled(name, enabled)` | `QString`, `bool` | `void` | 设置启用/禁用标志。**仅在下次 `start()` 时生效**，不会主动关闭/启动已运行的服务 |
| `isServerEnabled(name)` | `QString` | `bool` | 查询服务器当前启用标志 |
| `serverState(name)` | `QString` | `McpServerState` | 直接透传 `McpServerManager::serverState()`。状态值：`Pending` → `Connecting` → `Ready` / `Error` |
| `serverErrorMessage(name)` | `QString` | `QString` | 当 `serverState == Error` 时返回通用描述；调用方应查看 `getDiagnosticReport()` 获取详细错误 |
| `serverToolCount(name)` | `QString` | `int` | 读取该服务器 client 的 `cachedTools().size()`，返回已缓存工具数（不触发网络请求） |
| `client(name)` | `QString` | `shared_ptr<McpQtClient>` | 获取某服务器的原始 client，用于需要直接调用 McpQtClient API 的场景 |

#### 统一路由

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `exportAllToolsToLlm(format)` | `LlmFormat`（默认 OpenAI） | `QJsonArray` | 直接委托 `m_toolRouter->exportAllToolsToLlmFormat()`。聚合所有已连接服务器的全部工具，自动加 `serverName_` 前缀，输出可直接赋值给 LLM API 的 `tools` 字段 |
| `callToolAsync(name, args, callback)` | `QString`, `QJsonObject`, `function` | `void` | 直接委托 `m_toolRouter->callToolAsync()`。路由策略：解析 `name` 中的 `serverName_` 前缀 → 剥离前缀 → 找到对应 client → 调用 `callToolAsync`。无前缀或找不到服务器时回调收到 error |
| `toolRouter()` | — | `McpToolRouter*` | 非 owning 指针（生命周期由 McpHost 管理）。用于需要直接调用 Router 的 `parseToolName()`、`callToolFuture()` 等高级方法 |
| `promptRouter()` | — | `McpPromptRouter*` | 同上，提示词路由 |
| `resourceRouter()` | — | `McpResourceRouter*` | 同上，资源路由 |

#### 诊断

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `getDiagnosticReport()` | — | `QString` | 拼接 `m_reporter->renderText()`（按 stage 分组的诊断项）+ `m_reporter->renderExecutionLog()`（时序日志） |
| `reporter()` | — | `McpDiagnosticReporter*` | 非 owning 指针。`McpDiagnosticReporter` 不是 QObject——在 `~McpHost()` 中手动 `delete` |

自动记录：`clientErrorOccurred` 信号自动追加到 reporter（Error 级别）；`start()` 时自动 `clear()`；超时/终态检测自动追加执行日志。

#### 信号

| 信号 | 参数 | 说明 |
|------|------|------|
| `hostReady(success, summaryMsg)` | `bool`, `QString` | **启动流程的最终通知**。success=true 表示所有启用服务器 Ready（路径 A）；success=false 表示有 Error 或超时（路径 B/C）。调用方应连接此信号作为"可以开始调用 API"的触发点 |
| `serverStateChanged(name, state)` | `QString`, `McpServerState` | 单个服务器状态变化时发射。可用于 UI 状态指示器。注意：`start()` 期间此信号也用于驱动 `checkReadyCondition()` 终态检测 |
| `globalToolsChanged()` | — | 任一服务器工具列表变更时发射。适合触发 UI 刷新或 LLM tool list 重新导出 |
| `globalPromptsChanged()` | — | 任一服务器提示词列表变更 |
| `globalResourcesChanged()` | — | 任一服务器资源列表变更 |
| `errorOccurred(serverName, error)` | `QString`, `McpError` | 某个服务器发生传输或协议错误。同时自动记录到 `McpDiagnosticReporter` |

#### 使用模式

**模式一：配置文件驱动（推荐）**

```cpp
#include <mcp_qt_client/McpHost.h>

McpHost host;

// 加载配置文件
if (!host.loadConfigFromFile("mcp_servers.json")) {
    qCritical() << "配置加载失败";
    return;
}

// 可选：在启动前禁用某些服务器
host.setServerEnabled("heavy_analytics", false);

// 连接启动完成信号
QObject::connect(&host, &McpHost::hostReady, [&](bool ok, const QString& msg) {
    if (!ok) {
        qWarning() << "部分服务器启动失败:" << msg;
        qDebug() << host.getDiagnosticReport();
    }

    // 聚合导出工具
    QJsonArray tools = host.exportAllToolsToLlm();
    // 发送给 LLM API ...

    // 路由调用
    host.callToolAsync("github_search_code", {{"q", "mcp-qt"}},
        [](McpResult r) { qDebug() << r.data; });
});

// 连接工具变更（自动刷新 LLM tool list）
QObject::connect(&host, &McpHost::globalToolsChanged, [&]{
    QJsonArray fresh = host.exportAllToolsToLlm();
    // 更新 LLM context ...
});

// 异步启动（看门狗 30 秒）
host.start(30000);
```

**模式二：编程式配置（无需文件）**

```cpp
McpHost host;

// 手动构建配置
McpServerConfig githubCfg;
githubCfg.serverName = "github";
githubCfg.type = "stdio";
githubCfg.command = "npx";
githubCfg.args = {"-y", "@modelcontextprotocol/server-github"};

McpServerConfig fsCfg;
fsCfg.serverName = "filesystem";
fsCfg.type = "http";
fsCfg.url = "http://localhost:8081/mcp";

host.addServerConfig(githubCfg);
host.addServerConfig(fsCfg);

// 启动流程同上
host.start();
```

**模式三：热重载配置**

```cpp
// 修改 mcp_servers.json 后
if (host.reloadConfigAndRestart()) {
    qDebug() << "热重载成功";
} else {
    qWarning() << "重载失败（可能从未通过文件加载配置）";
}
```

McpHost host;
host.loadConfigFromFile("mcp_servers.json");

QObject::connect(&host, &McpHost::hostReady, [&](bool ok, const QString& msg) {
    if (!ok) qWarning() << "部分服务器启动失败:" << msg;

    // 聚合导出所有工具
    QJsonArray tools = host.exportAllToolsToLlm();

    // 路由调用：github_search_code 自动路由到 github 服务器
    host.callToolAsync("github_search_code", {{"q", "mcp-qt"}},
        [](McpResult r) { qDebug() << r.data; });
});

host.start(30000);
```

---

### McpServerManager

负责多服务器生命周期管理、工具预热、心跳保活。

**头文件**：`#include <mcp_qt_client/McpServerManager.h>`

#### 配置与注册

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `loadServers(loader)` | `shared_ptr<IMcpConfigLoader>` | `bool` | 通过配置加载器批量加载 |
| `loadServers(configs)` | `QList<McpServerConfig>` | `bool` | 通过配置列表直接加载 |
| `registerClient(name, client)` | `QString`, `shared_ptr<McpQtClient>` | `void` | 手动注册一个已连接的 client |
| `unregisterClient(name)` | `QString` | `void` | 注销指定服务器 |

#### 查询

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `client(name)` | `QString` | `shared_ptr<McpQtClient>` | 获取指定服务器的 client |
| `clients()` | — | `QHash<QString, shared_ptr<McpQtClient>>` | 获取全部 client 映射表 |
| `serverNames()` | — | `QStringList` | 已注册的服务器名称列表 |
| `serverState(name)` | `QString` | `McpServerState` | Pending / Connecting / Ready / Error |
| `isAllToolsReady()` | — | `bool` | 所有服务器是否都完成了工具预热 |

#### 生命周期

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `closeAll(timeoutMs)` | `int`（默认 5000） | `void` | 关闭所有 client 连接 |
| `startHeartbeat(intervalMs)` | `int`（默认 30000） | `void` | 启动心跳保活（定期 ping 所有已连接客户端） |
| `stopHeartbeat()` | — | `void` | 停止心跳 |

#### 信号

| 信号 | 参数 | 说明 |
|------|------|------|
| `clientConnected(name)` | `QString` | 服务器连接成功并初始化完成 |
| `clientDisconnected(name)` | `QString` | 服务器断开连接 |
| `clientErrorOccurred(name, error)` | `QString`, `McpError` | 服务器发生错误 |
| `clientToolsChanged(name, tools)` | `QString`, `vector<McpQtTool>` | 服务器工具列表变更 |
| `clientPromptsChanged(name)` | `QString` | 服务器提示词列表变更 |
| `clientStateChanged(name, state)` | `QString`, `McpServerState` | 服务器状态变化 |
| `clientToolsReady(name, toolCount)` | `QString`, `int` | 单个服务器的工具预热完成 |
| `allToolsReady()` | — | 所有已注册服务器的工具预热全部完成 |

#### 使用示例

```cpp
McpServerManager manager;

// 加载配置
auto loader = McpJsonConfigLoader::fromFile("mcp_servers.json");
manager.loadServers(std::make_shared<McpJsonConfigLoader>(loader));

// 监听全部就绪
QObject::connect(&manager, &McpServerManager::allToolsReady, []{
    qDebug() << "所有服务器工具预热完成";
});

// 心跳保活
manager.startHeartbeat(30000);
```

---

### McpToolRouter

跨服务器工具路由。核心策略：为每个服务器的工具名加 `serverName_` 前缀 → 接收 LLM 调用时解析前缀 → 剥离前缀后转发到对应 client。

**头文件**：`#include <mcp_qt_client/McpToolRouter.h>`

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `exportAllToolsToLlmFormat(format)` | `LlmFormat` | `QJsonArray` | 聚合所有服务器工具，已加前缀，输出为 LLM 专有结构（可直接赋值给 API `tools` 字段） |
| `exportAllToolsAsMcpSchema()` | — | `QJsonArray` | 聚合工具为标准 MCP Schema（仅 name/description/inputSchema，不加 LLM 包装） |
| `callToolFuture(name, args)` | `QString`, `QJsonObject` | `QFuture<McpResult>` | QFuture 异步路由调用 |
| `callToolAsync(name, args, callback, onProgress?)` | `QString`, `QJsonObject`, `function`, `ProgressCallback?` | `void` | 回调风格异步路由调用，支持进度通知 |
| `parseToolName(name)` | `QString` | `QPair<QString,QString>` | 解析 `serverName_toolName` → `{serverName, originalName}`。解析失败返回空 QPair |

**命名规则**：工具名 = `{serverName}_{originalToolName}`，其中 `serverName` 来自配置文件中该服务器的 key。

#### 使用示例

```cpp
McpToolRouter router(&manager);

// 聚合导出（自动加前缀）
QJsonArray openaiTools = router.exportAllToolsToLlmFormat(McpQtClient::LlmFormat::OpenAI);
// 结果: [{"type":"function","function":{"name":"github_search_code",...}}, ...]

// 路由调用（自动解析前缀 → 剥离 → 转发）
router.callToolAsync("github_search_code", {{"q", "test"}},
    [](McpResult r) { qDebug() << r.data; });
```

---

### McpPromptRouter

跨服务器提示词路由。与 McpToolRouter 相同的前缀策略。

**头文件**：`#include <mcp_qt_client/McpPromptRouter.h>`

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `fetchAllPrompts(timeoutMs)` | `int` | `QJsonArray` | 同步获取所有服务器的所有提示词（已加前缀） |
| `parsePromptName(name)` | `QString` | `QPair<QString,QString>` | 解析 `serverName_promptName` → `{serverName, originalName}` |
| `getPrompt(name, args, timeoutMs)` | `QString`, `QJsonObject`, `int` | `QJsonObject` | 同步获取指定提示词 |
| `getPromptAsync(name, args, callback)` | `QString`, `QJsonObject`, `function` | `void` | 异步获取，回调 `(QJsonObject result, QString error)` |

**信号**：`promptsChanged()` — 任一服务器提示词列表变更。

---

### McpResourceRouter

跨服务器资源路由。URI 重写为 `mcp-{serverName}-{originalUri}` 避免冲突。

**头文件**：`#include <mcp_qt_client/McpResourceRouter.h>`

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `fetchAllResources(timeoutMs)` | `int` | `QJsonArray` | 同步获取所有服务器的所有资源（URI 已重写） |
| `fetchAllResourcesAsync(callback, timeoutMs)` | `function`, `int` | `void` | 异步获取，回调 `(QJsonArray resources)` |
| `parseResourceUri(uri)` | `QString` | `QPair<QString,QString>` | 解析 `mcp-{serverName}-{uri}` → `{serverName, originalUri}` |
| `readResource(uri, timeoutMs)` | `QString`, `int` | `QJsonObject` | 同步读资源（自动路由） |
| `readResourceAsync(uri, callback)` | `QString`, `function` | `void` | 异步读资源，回调 `(QJsonObject result, QString error)` |

---

### McpServerConfig

服务器配置值类型（纯数据结构，无 QObject）。

**头文件**：`#include <mcp_qt_client/McpServerConfig.h>`

| 字段 | 类型 | 说明 |
|------|------|------|
| `serverName` | `QString` | 服务器名称（作为工具名前缀） |
| `disabled` | `bool` | 是否禁用（默认 false） |
| `command` | `QString` | Stdio 模式的启动命令 |
| `args` | `QStringList` | Stdio 模式的命令行参数 |
| `url` | `QString` | HTTP/SSE 模式的服务器 URL |
| `type` | `QString` | 传输类型：`"http"` 或 `"stateless_http"` |
| `nameSpace` | `QString` | 自定义命名空间前缀（覆盖默认的 serverName） |
| `env` | `QMap<QString,QString>` | 环境变量（支持 `$VAR` 引用） |
| `headers` | `QMap<QString,QString>` | 自定义 HTTP 请求头 |

---

### IMcpConfigLoader / McpJsonConfigLoader

配置加载器抽象与 JSON 实现。

**头文件**：`#include <mcp_qt_client/McpJsonConfigLoader.h>`

#### IMcpConfigLoader（抽象接口）

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `load()` | — | `QList<McpServerConfig>` | 纯虚方法，子类实现具体的配置加载逻辑 |

#### McpJsonConfigLoader

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `McpJsonConfigLoader(obj)` | `QJsonObject` | 构造函数 | 从 QJsonObject 构造 |
| `fromFile(path)` | `QString` | `static McpJsonConfigLoader` | 静态工厂：从 JSON 文件构造 |
| `load()` | — | `QList<McpServerConfig>` | 解析并返回配置列表，支持 `$ENV_VAR` / `${ENV_VAR}` 环境变量插值 |

**JSON 格式**：
```json
{
  "mcpServers": {
    "server_name": {
      "type": "http",
      "url": "http://localhost:8080/mcp",
      "nameSpace": "custom_prefix",
      "disabled": false,
      "env": { "TOKEN": "$MY_TOKEN" },
      "headers": { "Authorization": "Bearer ${API_KEY}" }
    }
  }
}
```

---

### McpResourceSubscriptionRouter

线程安全的 URI → 回调列表映射器，用于精准派发 `notifications/resources/updated`。

**头文件**：`#include <mcp_qt_client/McpResourceSubscriptionRouter.h>`

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `subscribe(uri, callback)` | `QString`, `function(uri, params)` | `int`（token） | 注册回调，返回 token 用于后续撤销 |
| `unsubscribe(uri, token)` | `QString`, `int` | `bool` | 通过 token 撤销，返回是否找到并删除 |
| `unsubscribeAll(uri)` | `QString` | `void` | 撤销某个 URI 下的全部订阅 |
| `dispatch(uri, params)` | `QString`, `QJsonObject` | `int` | 派发通知到所有订阅者，返回实际调用的回调数 |
| `hasSubscribers(uri)` | `QString` | `bool` | 是否有至少一个活跃订阅 |
| `subscribedUris()` | — | `QList<QString>` | 已订阅的 URI 集合 |
| `clear()` | — | `void` | 清除所有订阅 |

**线程安全**：内部使用 `std::mutex` + 快照模式——`dispatch()` 在锁内拷贝回调列表到栈上，锁外遍历执行，防止回调内退订造成死锁。

---

### McpDiagnosticReporter

多服务器诊断信息收集器，按 stage 分组，支持三级日志。

**头文件**：`#include <mcp_qt_client/McpDiagnosticReporter.h>`

| 方法 | 参数 | 说明 |
|------|------|------|
| `addExecutionLogLine(line)` | `QString` | 追加一行执行日志 |
| `addInfo(stage, message)` | `QString`, `QString` | 记录一条 Info 级别诊断 |
| `addWarning(stage, message, suggestion?)` | `QString`, `QString`, `QString?` | 记录一条 Warning 级别诊断，可选建议 |
| `addError(stage, message, suggestion?)` | `QString`, `QString`, `QString?` | 记录一条 Error 级别诊断 |
| `renderExecutionLog()` | → `QString` | 渲染执行日志文本 |
| `renderText()` | → `QString` | 渲染按 stage 分组的完整诊断报告 |
| `hasErrors()` | → `bool` | 是否有 Error 级别的诊断项 |
| `clear()` | — | 清空所有诊断数据 |

**级别枚举 McpDiagnosticLevel**：`Info` | `Warning` | `Error`

---

## 重连与自愈

```cpp
#include <mcp_core/McpReconnectPolicy.h>

mcp::McpReconnectPolicy policy;
policy.enabled = true;           // 启用自动重连
policy.initialDelayMs = 250;     // 初始重试延迟
policy.maxDelayMs = 5000;        // 最大延迟（毫秒）
policy.multiplier = 2.0;         // 指数退避乘数
policy.maxAttempts = -1;         // -1 = 无限重试

client->setReconnectPolicy(policy);

// 重连信号
QObject::connect(client.get(), &McpQtClient::reconnecting, []{
    qDebug() << "正在重连...";
});
QObject::connect(client.get(), &McpQtClient::reconnected, []{
    qDebug() << "重连成功，状态已恢复";
});
QObject::connect(client.get(), &McpQtClient::recoveryFailed,
    [](const QString& msg) {
        qDebug() << "重连失败:" << msg;
    });
```

**重连恢复内容**：
- 已注册的 notification handler
- 已注册的双向能力（Sampling/Elicitation/Roots）
- Resource subscription 状态
- 可重放的请求（自动回放队列）
- 工具列表缓存

### 生命周期

```cpp
// 优雅关闭（发送 shutdown 请求）
client->close(5000);

// 检查连接状态
if (client->isConnected()) { /* ... */ }
```

---

## LLM 格式导出

```cpp
// 枚举
enum class McpQtClient::LlmFormat {
    OpenAI,     // OpenAI function calling
    Anthropic,  // Claude/Anthropic tools
    Gemini      // Gemini function declarations
};

// 导出全部工具
QJsonArray openaiTools = client->exportAllToolsToLlmFormat(
    McpQtClient::LlmFormat::OpenAI);

// 导出单个工具
QJsonObject tool = client->exportToolToLlmFormat("calculate_add",
    McpQtClient::LlmFormat::Anthropic);

// 静态方法：不依赖 client 实例
QJsonObject tool = McpQtClient::exportToolToLlmFormat(myMcpTool,
    McpQtClient::LlmFormat::OpenAI);

// 导出为标准 MCP Schema（仅 name/description/inputSchema）
QJsonArray mcpSchema = client->exportAllToolsAsMcpSchema();
```

**返回格式示例**：

OpenAI 格式：
```json
[
  {
    "type": "function",
    "function": {
      "name": "calculate_add",
      "description": "Add two numbers",
      "parameters": { "type": "object", "properties": {...} }
    }
  }
]
```

直接赋值给 API 请求的 `tools` 字段，无需额外包裹。

---

## 错误处理

### McpError

```cpp
struct McpError {
    int code{0};              // JSON-RPC 错误码或 HTTP 状态码
    QString message;          // 错误摘要
    QJsonObject data;         // 附加详情

    QString toString() const; // "[401] Unauthorized"
};
```

### 错误信号

```cpp
QObject::connect(client.get(), &McpQtClient::errorOccurred,
    [](const mcp_qt::McpError& error) {
        qDebug() << "错误:" << error.toString();
        // error.code:  JSON-RPC error code 或 HTTP status
        // error.message: 人类可读描述
        // error.data: 结构化错误数据
    });
```

### 同步 API 错误处理

```cpp
// 带 error 输出参数
QString error;
McpResult result = client->callTool(..., 10000);
if (result.isError) {
    qDebug() << "工具调用失败:" << result.errorString;
}

// 带错误回调的异步版本
client->callToolAsync(name, args,
    [](McpResult result) {
        if (result.isError) { /* ... */ }
    });

// 类型化结果的错误
McpQtToolResult typed = client->callToolTyped(...);
if (typed.isError) {
    qDebug() << typed.errorString;
}
```

---

## 完整示例

### 控制台脚本

```cpp
#include <QCoreApplication>
#include <QDebug>
#include <mcp_qt_client/McpQtClient.h>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // 连接到 MCP 服务器
    auto client = mcp_qt::McpQtClient::connectHttpAndWait(
        "http://localhost:8080/mcp");

    if (!client->isConnected()) {
        qCritical() << "连接失败";
        return 1;
    }

    qDebug() << "已连接:" << client->serverInfo();

    // 获取工具列表
    auto tools = client->listTools();
    qDebug() << "发现" << tools.size() << "个工具:";
    for (auto& t : tools) {
        qDebug() << " -" << t.name << ":" << t.description;
    }

    // 调用工具
    auto result = client->callTool("calculate_add",
        {{"a", 5}, {"b", 3}});
    qDebug() << "结果:" << result.data;

    // 关闭
    client->close();
    return 0;
}
```

### GUI 应用最小框架

```cpp
#include <QApplication>
#include <QListView>
#include <QVBoxLayout>
#include <QWidget>
#include <QPushButton>
#include <mcp_qt_client/McpQtClient.h>
#include <mcp_qt_client/McpToolsModel.h>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* listView = new QListView(&window);
    auto* refreshBtn = new QPushButton("刷新", &window);
    layout->addWidget(listView);
    layout->addWidget(refreshBtn);

    // 异步连接
    auto client = mcp_qt::McpQtClient::connectHttpAsync(
        "http://localhost:8080/mcp");

    // 创建工具模型
    auto toolModel = client->createToolsModel(&window);
    listView->setModel(toolModel.get());

    // 连接就绪后拉取
    QObject::connect(client.get(), &mcp_qt::McpQtClient::connected,
        toolModel.get(), &mcp_qt::McpToolsModel::refresh);

    QObject::connect(refreshBtn, &QPushButton::clicked,
        toolModel.get(), &mcp_qt::McpToolsModel::refresh);

    // 选中工具 → 调用
    QObject::connect(listView->selectionModel(),
        &QItemSelectionModel::currentChanged,
        [&](const QModelIndex& idx) {
            QString name = idx.data(mcp_qt::McpToolsModel::NameRole).toString();
            client->callToolAsync(name, QJsonObject{},
                &window,
                [](mcp_qt::McpResult r) {
                    qDebug() << "结果:" << r.data;
                });
        });

    window.show();
    return app.exec();
}
```

### 多服务器 + LLM Agent

```cpp
#include <QApplication>
#include <QDebug>
#include <mcp_qt_client/McpHost.h>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    McpHost host;

    // 加载配置（http:// + stdio:// 混合）
    host.loadConfigFromFile("mcp_servers.json");

    QObject::connect(&host, &McpHost::hostReady,
        [&](bool success, const QString& msg) {
            if (!success) {
                qWarning() << "部分服务器启动失败:" << msg;
            }

            // 导出所有工具为 OpenAI 格式
            QJsonArray tools = host.exportAllToolsToLlm();

            // 发送给 LLM API ...
            // LLM 返回要调用的工具名: "github_search_code"

            // McpHost 自动路由到对应服务器
            host.callToolAsync("github_search_code",
                {{"q", "mcp-qt"}},
                [](McpResult result) {
                    qDebug() << "GitHub 搜索结果:" << result.data;
                });
        });

    host.start(30000);
    return app.exec();
}
```
