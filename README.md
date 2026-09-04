# mcp-qt

基于 Qt6 纯净实现的 **Model Context Protocol (MCP) 客户端 SDK**。

整个 SDK 基于 **Qt6 (Core/Network/Widgets)** 原生架构进行响应式设计，零外部第三方网络库（如 libcurl、httplib 等）物理依赖，完整支持 **MCP 2026-07-28** 无状态协议核心并通过官方 conformance 验证。

📖 **使用手册请查阅 [API 参考手册 (docs/API_REFERENCE.md)](docs/API_REFERENCE.md)**

---

## 官方合规与最新规范支持

**协议版本支持**：
- `2026-07-28` — 无状态核心（Stateless Core）、MRTR 多轮交互、Header 路由、OAuth 硬化、MCP Apps 扩展
- `2025-11-25` / `2025-06-18` / `2025-03-26` — 向后兼容（legacy initialize 握手保留）

### MCP 2026-07-28 官方 conformance（conformance@0.2.0-alpha.10）

| 场景类别 | 场景数 | 结果 |
|---------|:------:|:----:|
| 核心 stateless（discover / MRTR / header 路由 / x-mcp-header / schema 安全） | 7 | ✅ 全过 |
| OAuth 授权（iss 校验 / scope / token-endpoint / CIMD / 迁移） | 25 | ✅ 全过 |
| **总计** | **32** | **32/32 全绿，0 失败 0 警告** |

**全量 `--suite all`（2026-09-03 实测）**：43 个客户端场景 **450 passed / 14 failed / 0 warnings**，全部失败均在 `conformance_runner_qt/conformance-baseline.yml` 预期基线内（C2 EMA、C3 DPoP、C4 WIF、旧版 OAuth 端点回退——见 `docs/缺失功能清单.md`）。sse-retry 与 auth/authorization-server-migration 已修复全绿。

**新增官方 2026-07-28 能力**：

| 能力 | 说明 |
|------|------|
| `server/discover` | 免握手能力发现（SEP-2575） |
| MRTR 多轮交互 | `input_required` → requestState 回显 → 重试（SEP-2322） |
| `Mcp-Method` / `Mcp-Name` Header 路由 | + Base64 sentinel 编码（SEP-2243） |
| `x-mcp-header` | 工具参数镜像 `Mcp-Param-{Name}`，非法注解工具剔除 |
| `subscriptions/listen` | 长连接变更通知流（取代旧资源订阅） |
| CacheableResult | `ttlMs` / `cacheScope`（SEP-2549） |
| OAuth 硬化 | RFC 9207 `iss` 校验、DCR `application_type`、CIMD、issuer 凭据绑定、scope 合并 |
| **MCP Apps** | `io.modelcontextprotocol/ui` 扩展：`ui://` 资源 + WebView2 渲染 + postMessage 互通 |

### 旧协议（2025-11-25）回归

`initialize` / `tools_call` / `elicitation` 等旧场景通过（按 spec-version 自动选择 stateless/legacy 路径）。
已知环境性失败（`sse-retry` 时序、部分 auth 场景）在未改动的 master 上同样存在，非本分支回归。

---

## 快速开始

SDK 提供了基于 QObject 的高层接口 `McpQtClient`，采用 Qt 信号/槽机制进行异步事件通知，使用极其简便：

```cpp
#include <mcp_qt_client/McpQtClient.h>

// HTTP/SSE 连接（同步等待初始化完成）
auto client = mcp_qt::McpQtClient::connectHttpAndWait("http://localhost:8080/mcp");

// 同步 API（利用局部事件循环阻塞，非 GUI 线程推荐）
auto tools    = client->listTools();
auto result   = client->callTool("calculate_add", {{"a", 5}, {"b", 3}});
auto resource = client->readResource("file:///data/config.json");
auto prompt   = client->getPrompt("greeting", {{"name", "World"}});

// 异步 API（GUI 线程安全，通过回调接收结果）
client->callToolAsync("calculate_add", {{"a", 5}, {"b", 3}}, [](McpResult result) {
    qDebug() << "Tool result:" << result.data;
});

// QFuture 现代异步接口
QFuture<McpResult> future = client->callToolFuture("query", {{"q", "hello"}});

// 并发多工具调用
std::vector<McpBatchCallRequest> requests = {
    {"tool_a", {{"x", 1}}},
    {"tool_b", {{"y", 2}}}
};
auto results = client->callToolsConcurrent(requests);

// OAuth 认证连接
mcp_qt::McpQtClient::OAuthConfig oa;
oa.serverUrl    = "https://secure-server.com/mcp";
oa.clientId     = "my-client-id";
oa.clientSecret = "my-secret";
auto authClient = mcp_qt::McpQtClient::connectWithOAuthAndWait(oa);

// 本地 Stdio 子进程连接
auto stdioClient = mcp_qt::McpQtClient::connectStdioAndWait("python", {"server.py"});

// 🌟 MCP 2026-07-28 无状态 HTTP 节点（云原生免握手 & Header 路由）
auto statelessClient = mcp_qt::McpQtClientBuilder()
    .setTransportStatelessHttp("http://localhost:8080/mcp")
    .setProtocolVersion("2026-07-28")
    .setStatelessMode(true)
    .buildAndConnectAsync();

// 🌟 MCP 2026-07-28 MRTR 多轮次交互信号监听（用于 Qt GUI / QML 弹窗交互）
// inputRequests: 规范 InputRequests map (key -> {method, params})；
// requestState: 服务端 opaque 状态，库会在重发时自动原样回显。
// 回传的 InputResponses 需按 key 组织；单请求 elicitation 也可直接回扁平表单数据。
QObject::connect(statelessClient.get(), &mcp_qt::McpQtClient::inputRequired,
                 [](const QString& reqId, const QJsonObject& inputRequests, const QString& requestState,
                    mcp_qt::MrtrReplyCallback replyCb) {
    qDebug() << "Server requested additional input for request:" << reqId << "InputRequests:" << inputRequests;
    QJsonObject inputResponses;
    const QJsonObject requests = inputRequests;
    for (auto it = requests.begin(); it != requests.end(); ++it) {
        const QJsonObject req = it.value().toObject();
        if (req.value("method").toString() == "elicitation/create") {
            QJsonObject content;
            content["password"] = "secret123";
            inputResponses[it.key()] = QJsonObject{{"action", "accept"}, {"content", content}};
        }
    }
    Q_UNUSED(requestState);
    replyCb(inputResponses);
});

// 🌟 MCP Apps（io.modelcontextprotocol/ui）：WebView2 渲染 + JS⇄C++ 互通
#include <mcp_qt_apps/McpAppWebView2Renderer.h>
#include <mcp_qt_apps/McpAppSupport.h>
// 1. 声明客户端能力（capabilities.extensions 支持 text/html;profile=mcp-app）
client->registerMcpAppCapabilities();
// 2. 创建渲染器并嵌入布局
auto renderer = std::make_shared<mcp_qt::McpAppWebView2Renderer>();
layout->addWidget(renderer->hostWidget());
// 3. 收到 App 消息（ui/initialize 等 postMessage JSON-RPC）
renderer->setAppMessageHandler([renderer](const QJsonObject& msg) {
    qDebug() << "App message:" << msg;
});
// 4. 加载服务器 ui:// 资源返回的 HTML
renderer->loadHtml(htmlFromServer, QUrl());

// 双向能力（处理来自服务端的请求）
client->setElicitationHandler([](const QJsonObject& params, auto callback) { ...; callback(result, error); });
client->setSamplingHandler([](const QJsonObject& params, auto callback) { ...; callback(result, error); });
client->setRootsProvider([](auto callback) { ...; callback(roots, error); });

// 响应式信号槽
QObject::connect(client.get(), &McpQtClient::connected,    []{ qDebug() << "connected"; });
QObject::connect(client.get(), &McpQtClient::disconnected, []{ qDebug() << "disconnected"; });
```

---

## 构建

项目依赖 Qt6（Core/Network；MCP Apps 需 Widgets）和 C++17 编译器（MinGW / MSVC / GCC 均可），支持 Out-Of-Source 构建：

```bash
# 配置项目
cmake -B build

# 执行编译
cmake --build build

# 运行测试
cd build && ctest
# 主测试产物在 build/build_test/（见 test/CMakeLists.txt）
./build/build_test/tests_qt

# 主示例：多服务器 Agent + MCP Apps 渲染（WebView2）
./build/examples/multi_server_agent/multi_server_agent
```

> **WebView2 依赖**：Windows 10/11 系统预装 Edge WebView2 Runtime；SDK 头文件与 `WebView2Loader.dll` 已纳入 `third_party/webview2`（构建时自动复制到可执行文件旁）。

---

## 项目结构

```text
mcp-qt/
 ├── src/
 │    ├── core/                       # SDK 核心（纯 C++17，零 Qt 依赖）
 │    │    ├── IMcpTransport.h        #   传输层抽象接口
 │    │    ├── McpClientSession.h     #   会话管理（stateless/legacy 双模式）
 │    │    ├── McpOAuthClient.h       #   OAuth 2.0 + PKCE + iss 校验 + issuer 绑定
 │    │    ├── McpHeaderEncoding.h    #   SEP-2243 Header Base64 sentinel 编码
 │    │    ├── JsonRpcDispatcher.h    #   JSON-RPC 消息分发
 │    │    ├── McpMessage.h           #   消息类型定义
 │    │    ├── McpTool.h              #   工具定义 + annotations + x-mcp-header
 │    │    ├── McpResource.h          #   资源/资源模板定义
 │    │    └── McpPrompt.h            #   提示词定义
 │    ├── transport/                  # Qt 原生传输层（QNAM + QProcess，零 curl）
 │    │    ├── QtHttpSseTransport.h   #   HTTP/SSE 长连接（legacy）
 │    │    ├── QtStatelessHttpTransport.h # 无状态 HTTP（2026-07-28 Header 路由）
 │    │    └── QtProcessStdioTransport.h  # 子进程 Stdio 传输
 │    ├── client/                     # Qt 高层客户端（QObject 封装）
 │    │    ├── McpQtClient.h          #   主客户端（5 套 API 风格 + 2026 能力）
 │    │    ├── McpHost.h              #   外观模式一站式入口
 │    │    ├── McpServerManager.h     #   多服务器生命周期管理
 │    │    └── ...                    #   路由 / MVC 模型 / 配置加载
 │    └── apps/                       # MCP Apps 扩展（io.modelcontextprotocol/ui）
 │         ├── McpAppSupport.h        #   协议层：能力声明 / ui:// 资源 / AppBridge 消息
 │         ├── IMcpAppRenderer.h      #   渲染抽象（可插拔）
 │         └── McpAppWebView2Renderer.h # WebView2 后端（QWidget + JS postMessage 桥）
 ├── conformance_runner_qt/          # 官方协议合规测试（2026-07-28 32/32 全绿）
 ├── test/                           # Qt Test 框架单元/集成测试
 ├── third_party/                    # WebView2 SDK + wil
 └── examples/
      ├── multi_server_agent/        # 完整 GUI 应用：多服务器聚合 + ReAct Agent + MCP Apps 渲染（WebView2）
      └── anysearch_qt/              # 轻量搜索客户端示例
```

> **💡 关于实战验证**：
> 如果想了解如何在真实的高并发 GUI 场景中挂载多语言 MCP 服务器，请直接查看 [multi_server_agent 示例](./examples/multi_server_agent/README.md)。

---

## 🧪 测试与验证体系 (Testing & Validation)

为了将 `mcp-qt` 打造为一个企业级的稳健基座，本项目构建了一个从底层代码逻辑到顶层应用实践的 **“三维立体测试矩阵”**。

这三个维度分别对应根目录下的三个文件夹：`test`、`conformance_runner_qt` 以及 `examples`。如果你是第一次接手或参与贡献本项目，请务必了解它们各自的职责。

### 1. `test`：组件级单元测试 (Unit & Integration Tests)
**“内部零件是不是好的？”**
- **定位**：面向 SDK **开发者** 的白盒/灰盒测试。
- **框架**：基于 `QtTest` 框架构建。
- **核心职责**：验证 SDK 内部齿轮运转是否正常（JSON-RPC 报文、配置解析、stateless `_meta` 注入、MRTR 循环、Header 路由等）。
- **触发时机**：每次提交代码或修改底层逻辑时，必须跑通此测试，防止回归 Bug。

### 2. `conformance_runner_qt`：协议一致性黑盒测试 (Protocol Conformance)
**“说的话别人听得懂吗？”**
- **定位**：面向 **MCP 官方协议标准** 的黑盒测试。
- **框架**：官方 Conformance Test Suite（`@modelcontextprotocol/conformance`）。
- **核心职责**：验证 SDK 发出去的报文与收到的报文 100% 遵守 MCP 官方协议标准。
  - 2026-07-28：`request-metadata`、`sep-2322-client-request-state`、`http-standard-headers`、`http-custom-headers`、`http-invalid-tool-headers`、`json-schema-ref-no-deref`、auth/iss-* 等 **32/32 全绿**
  - legacy：`initialize`、`tools_call`、`elicitation` 按 spec-version 自动路由

### 3. `examples`：端到端业务与集成实战 (End-to-End Examples)
**“造出来的车究竟好不好开？”**
- **代表作**：`examples/multi_server_agent`（多服务器聚合 + ReAct Agent + MCP Apps 渲染）
- **核心职责**：将最真实、最极限的环境融合在一起进行测试（真实网络、多语言服务、多协议并发、UI 多线程压测、Agent 调度闭环、MCP Apps 渲染互通）。

**💡 测试体系总结**：
- 修改了底层逻辑，先看 `test` 能不能通过。
- 升级了 MCP 协议版本，去 `conformance_runner_qt` 跑一遍对齐标准。
- 想要评估新功能好不好用，直接在 `examples` 里狂飙测试。

---

## SDK 高级特性

### 多 API 风格

每个 MCP 操作提供 **5 套调用方式** 以适应不同场景：

| 风格 | 示例 | 适用场景 |
|------|------|---------|
| 同步阻塞 | `client->callTool(...)` | 非 GUI 线程，脚本/CLI 工具 |
| 异步回调 | `client->callToolAsync(..., callback)` | GUI 线程，不阻塞事件循环 |
| QFuture | `client->callToolFuture(...)` | 现代 C++ 异步链式调用 |
| 批量并发 | `client->callToolsConcurrent({...})` | 一次发起多个工具调用 |
| 类型化结果 | `client->callToolTyped(...)` | 自动解析 Base64 图片/嵌入资源 |

### MCP 2026-07-28 高级能力

- **无状态 HTTP**：免握手、每请求 `_meta` 自包含、Header 路由（`Mcp-Method`/`Mcp-Name`）
- **MRTR 多轮交互**：`inputRequired` 信号 + requestState 原样回显 + 自动重试
- **`server/discover`**：异步/同步能力发现
- **CacheableResult**：Qt 层 `listTools(..., McpCacheHint*)`（session 层为 `listToolsWithCache`）暴露 `ttlMs`/`cacheScope`
- **`subscriptions/listen`**：长连接变更通知（`listenSubscriptions` / `setSubscriptionListener`）
- **OAuth 硬化**：RFC 9207 `iss` 校验、DCR `application_type`、CIMD URL-based client_id、issuer 凭据绑定、scope 合并

### MCP Apps（交互式 UI 渲染）

服务器工具可返回交互式 HTML（图表/表单/仪表盘），在客户端内嵌 WebView2 渲染，双向 postMessage 通信：

```cpp
client->registerMcpAppCapabilities();   // 声明扩展能力
auto renderer = std::make_shared<mcp_qt::McpAppWebView2Renderer>();
layout->addWidget(renderer->hostWidget());
renderer->setAppMessageHandler([renderer](const QJsonObject& msg) {
    // 处理 ui/initialize、tools/call 代理等 AppBridge 消息
});
renderer->loadHtml(htmlFromServer, QUrl());
```

### LLM 格式导出

一键将 MCP 工具定义转换为大模型 API 原生格式：

```cpp
QJsonArray openaiTools  = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::OpenAI);
QJsonArray anthropicTools = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::Anthropic);
QJsonArray geminiTools  = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::Gemini);
```

### 多服务器路由与聚合

通过 `McpHost` 外观模式统一管理多个 MCP 服务器，自动为工具名加 `serverName_` 前缀实现跨服务器路由：

```cpp
McpHost host;
host.loadConfigFromFile("mcp_servers.json");
host.start();
QJsonArray allTools = host.exportAllToolsToLlm();
host.callToolAsync("github_search_code", {{"q", "mcp-qt"}}, [](McpResult r) {
    qDebug() << r.data;
});
```

**按会话/Agent 过滤视图（`McpServerView`）**：底层连接全局共享（同一服务器只连一次），
每个会话用轻量视图裁剪可见服务器，实现"每个 agent 一套独立 MCP 环境"：

```cpp
// 每个 agent 一个视图，只声明它要的服务器
McpServerView view(&host);
view.setVisibleServers({"github", "filesystem"});   // 空 = 全部可见
QJsonArray agentTools = view.exportAllToolsToLlmFormat();  // 只含可见服务器工具
QJsonArray agentPrompts = view.exportAllPrompts();         // 只含可见服务器提示词
QJsonArray agentResources = view.exportAllResources();     // 只含可见服务器资源
// 切换 agent = 换可见列表，连接不重建
view.setVisibleServers({"search"});
```

### 统一日志（McpLogger）

全局日志级别控制 + 文件落盘，解决"各模块各自 qDebug 无开关、日志不落盘"两个可观测性缺口：

```cpp
#include <mcp_qt_client/McpLogger.h>

// 全局级别开关（Debug/Info/Warning/Error，低于该级别的日志被丢弃）
mcp_qt::McpLogger::setGlobalLevel(mcp_qt::McpLogLevel::Info);

// 文件落盘（追加模式，线程安全）
mcp_qt::McpLogger::setLogFile("mcp-qt.log");

// 统一入口（带模块名，输出含时间戳 + 级别 + 模块）
mcp_qt::McpLogger::warning("MCP client error for github: connection refused", "McpServerManager");
// 或便捷宏
MCP_LOG_INFO_MOD("McpHost", "Starting MCP Host...");
```

日志行格式：`2026-09-03 14:30:00.123 [Warning] [McpServerManager] MCP client error for ...`。
`McpServerManager` 的连接错误/心跳失败/状态变化已接入；控制台输出可用 `setConsoleEnabled(false)` 关闭（仅写文件）。

**日志配置建议**：

| 环境 | 推荐配置 |
|---|---|
| 开发调试 | `setGlobalLevel(Debug)` + 控制台默认开（不落盘，避免文件噪音） |
| 生产 | `setGlobalLevel(Warning)` + `setLogFile(日志目录/mcp-qt.log)` + `setConsoleEnabled(false)`（只落盘，避免刷屏） |
| 问题排查 | 临时 `setGlobalLevel(Debug)` + 保留文件输出，配合流量追踪（`setTrafficLogger`）定位协议层问题 |

注意：文件输出是**显式开启**的——不调用 `setLogFile` 不会产生任何日志文件；路径完全由调用方指定。

### 类型化工具结果

无缝解析复合型工具返回，自动解码 Base64 图片，**始终保留原始 JSON**：

```cpp
McpQtToolResult result = client->callToolTyped("generate_image", {{"prompt", "sunset"}});
if (!result.isError) {
    for (const auto& content : result.content) {
        if (content.kind == McpQtContentKind::Image) {
            processImage(content.binary, content.mimeType); // 已自动解码
        }
    }
    qDebug() << "Raw:" << result.raw;
}
```

### MVC 模型绑定

4 个 `QAbstractListModel` 子类，可直接绑定 `QListView` 或 QML ListView，支持分页懒加载：

```cpp
auto toolModel = client->createToolsModel(parent);
listView->setModel(toolModel.get());
toolModel->refresh();
```

### 指数退避重连与状态自愈

网络抖动或子进程崩溃时自动重连，并恢复所有已注册的 handler、subscription 和双向能力：

```cpp
mcp::McpReconnectPolicy policy;
policy.initialDelayMs = 250;
policy.maxDelayMs = 5000;
policy.multiplier = 2.0;
policy.maxAttempts = -1;
client->setReconnectPolicy(policy);
QObject::connect(client.get(), &McpQtClient::reconnected, []{ qDebug() << "通道自愈成功"; });
```

### 更多

- **全流量 Tracing** — `setTrafficLogger()` 拦截所有出站/入站报文
- **OAuth 2.0** — PKCE + DCR + iss 校验 + Token 自动刷新
- **双向能力** — Sampling / Elicitation / Roots（旧双向通道 + MRTR 双路径）
- **资源订阅路由** — `subscribeResource()` + `subscriptions/listen` 双机制
- **HTTP 高级配置** — 自定义 Headers、代理、Bearer token 自动注入
- **Stdio 进程管理** — Windows 上用 `CreateJobObject` 确保子进程随父进程退出

---

## 依赖

*   **编译器**：支持 C++17 或以上标准（MSVC 2019+ / GCC 8+ / MinGW-w64）
*   **构建系统**：CMake ≥ 3.16
*   **Qt 组件**：Qt6::Core, Qt6::Network, Qt6::Test（MCP Apps 额外需要 Qt6::Widgets/Gui）
*   **第三方库**：
    *   nlohmann/json 3.11.3（FetchContent 自动下载）
    *   WebView2 SDK 1.0.4129.50（`third_party/webview2`，MCP Apps 渲染）
    *   wil（`third_party/wil`，WebView2 兼容头）
*   **Windows 额外依赖**：`bcrypt`（mcp_core OAuth PKCE + mcp_qt_client ES256 JWT 签名）；MCP Apps 需系统 Edge WebView2 Runtime（Win10/11 预装）

---

## 已知限制

*   **JWT-Bearer Grant Type**：✅ 已支持 **ES256（P-256）**（`private_key_pem` → RFC 7523 client assertion，Windows BCrypt / 非 Windows OpenSSL；`auth/client-credentials-jwt` conformance 已验证通过 8/8）；RS256 与 ECDSA P-384/P-521 未实现
*   **OAuth 端点回退**：旧场景 `auth/2025-03-26-oauth-endpoint-fallback` 的早期 PRM 猜解回退未实现（已加入 `conformance-baseline.yml`）
*   **MCP Apps 渲染**：默认后端为 WebView2（Windows）；当前 `ui://` 资源获取支持 HTTP/相对地址解析，复杂 AppBridge 能力（工具调用代理、权限策略细化）为可扩展预留
*   **扩展生态**：Tasks（`io.modelcontextprotocol/tasks`）、EMA、DPoP、WIF 等扩展未实现（非核心协议 MUST）
*   **环境性旧场景失败**：`sse-retry`（重连时序）、部分旧 auth 场景在未改动的 master 上同样存在，非本分支回归
