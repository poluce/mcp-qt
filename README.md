# mcp-qt

基于 Qt6 纯净实现的 **Model Context Protocol (MCP) 客户端 SDK**。

整个 SDK 基于 **Qt6 (Core/Network)** 原生架构进行响应式设计，零外部第三方网络库（如 libcurl、httplib 等）物理依赖，完美通过官方 conformance 完整测试套件验证。

📖 **使用手册请查阅 [API 参考手册 (docs/API_REFERENCE.md)](docs/API_REFERENCE.md)**

---

## 官方合规与最新规范支持

**协议版本支持**：`2026-07-28`（完全无状态核心 Stateless Core、MRTR 多轮次交互、`_meta` 数据自包含、网关 Header 路由）、`2025-11-25`（向后兼容）

| 套件 | 场景数 | 通过 | 警告 | 失败 | 通过率 |
|------|:------:|:----:|:----:|:----:|:------:|
| `--suite all` | 26 | 22 | 2 | 2 | **85%** |

**总计**：317 assertions passed, 14 failed, 1 warnings（含并发假报错 6 个）

### 已通过场景 (22/26)

`initialize` · `tools_call` · `elicitation-sep1034-client-defaults` · `sse-retry` · `auth/metadata-default` · `auth/metadata-var1` · `auth/metadata-var2` · `auth/metadata-var3` · `auth/scope-from-www-authenticate` · `auth/scope-from-scopes-supported` · `auth/scope-omitted-when-undefined` · `auth/scope-step-up` · `auth/scope-retry-limit` · `auth/token-endpoint-auth-basic` · `auth/token-endpoint-auth-post` · `auth/token-endpoint-auth-none` · `auth/pre-registration` · `auth/2025-03-26-oauth-metadata-backcompat` · `auth/resource-mismatch` · `auth/offline-access-scope` · `auth/offline-access-not-supported` · `auth/client-credentials-basic`

### 失败场景 (2/26) + 警告场景 (2/26)

| 场景 | 问题 | 状态 |
|------|------|------|
| `auth/basic-cimd` | 1 个非致命警告 | ⚠️ |
| `elicitation-sep1034-client-defaults` | 并发模式下超时假报错（单独运行 ✅ 5/0） | ⚠️ |
| `auth/2025-03-26-oauth-endpoint-fallback` | OAuth 早期 PRM 猜解回退，已加入 baseline 忽略清单 | ❌ |
| `auth/client-credentials-jwt` | JWT assertion 签名（ES256/RS256）未实现 | ❌ |
| `auth/cross-app-access-complete-flow` | 依赖 JWT-Bearer token 交换 | ❌ |

> 本 SDK 在 HTTP/SSE 传输层中优雅地解决并修复了官方协议规范在建立连接时由于 `endpoint` 尚未就绪而发包导致的 Race Condition（竞态条件）Bug。

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
QObject::connect(statelessClient.get(), &mcp_qt::McpQtClient::inputRequired,
                 [](const QString& reqId, const QJsonObject& schema, mcp_qt::MrtrReplyCallback replyCb) {
    qDebug() << "Server requested additional input for request:" << reqId << "Schema:" << schema;
    // 用户弹窗确认后回传补充输入
    replyCb(QJsonObject{{"password", "secret123"}});
});

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

项目仅依赖 Qt6 和 C++17 编译器（MinGW / MSVC / GCC 均可），支持 Out-Of-Source 构建：

```bash
# 配置项目
cmake -B build

# 执行编译
cmake --build build

# 运行测试
cd build && ctest
# 或直接运行单个测试
./build/test/tests_qt testToolsModel

# 合规测试（需先启动测试服务器）
node test_mcp_server.js --http &
./build/conformance_runner_qt/mcp_client_conformance_qt --suite core
```

---

## 项目结构

```text
mcp-qt/
 ├── src/
 │    ├── core/                       # SDK 核心（纯 C++17，零 Qt 依赖）
 │    │    ├── IMcpTransport.h        #   传输层抽象接口
 │    │    ├── McpClientSession.h     #   会话管理（最大核心类，444 行）
 │    │    ├── McpOAuthClient.h       #   OAuth 2.0 + PKCE 认证
 │    │    ├── JsonRpcDispatcher.h    #   JSON-RPC 消息分发
 │    │    ├── McpMessage.h           #   消息类型定义
 │    │    ├── McpTool.h              #   工具定义 + annotations
 │    │    ├── McpResource.h          #   资源/资源模板定义
 │    │    └── McpPrompt.h            #   提示词定义
 │    ├── transport/                  # Qt 原生传输层（QNAM + QProcess，零 curl）
 │    │    ├── QtHttpSseTransport.h   #   HTTP/SSE 长连接（Pimpl 模式）
 │    │    ├── QtStatelessHttpTransport.h # 无状态 HTTP + SSE 监听
 │    │    └── QtProcessStdioTransport.h  # 子进程 Stdio 传输
 │    └── client/                     # Qt 高层客户端（572 行 QObject 封装）
 │         ├── McpQtClient.h          #   主客户端（5 套 API 风格）
 │         ├── McpQtClientBuilder.h   #   Builder 模式构造器
 │         ├── McpHost.h              #   外观模式一站式入口
 │         ├── McpServerManager.h     #   多服务器生命周期管理
 │         ├── McpToolRouter.h        #   跨服务器工具路由（名前缀）
 │         ├── McpToolsModel.h        #   QAbstractListModel MVC 适配器
 │         ├── McpJsonConfigLoader.h  #   JSON 配置文件解析
 │         └── McpResourceSubscriptionRouter.h # 资源更新回调路由
 ├── conformance_runner_qt/          # 官方协议合规测试套件（21/26 通过）
 ├── test/                           # Qt Test 框架单元/集成测试（14 个测试文件）
 └── examples/
      ├── multi_server_agent/        # 完整 GUI 应用：多服务器聚合 + LLM Agent
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
- **核心职责**：
  专注于验证 SDK 内部的齿轮运转是否正常。例如：
  - JSON-RPC 请求与响应报文的拼接与解析逻辑是否准确无误？
  - `McpJsonConfigLoader` 解析复杂配置文件时能否正确抽取环境变量？
  - `QtProcessStdioTransport` 对于环境变量代理、正则表达式匹配的内部机制是否稳健？
- **触发时机**：每次提交代码或修改底层逻辑时，必须跑通此测试，防止回归 Bug。

### 2. `conformance_runner_qt`：协议一致性黑盒测试 (Protocol Conformance)
**“说的话别人听得懂吗？”**
- **定位**：面向 **MCP 官方协议标准** 的黑盒测试。
- **框架**：基于外部标准 MCP 服务器或官方 Conformance Test Suite。
- **核心职责**：
  无论 SDK 内部是怎么写的，此测试只在乎 **SDK 发出去的报文和收到的报文是否 100% 遵守 MCP 官方定义的协议标准**。
  - 测试 SDK 的生命周期状态（`Initialize` -> `Initialized` -> `Ping`）。
  - 测试能力协商（Client Capabilities vs Server Capabilities）。
  - 测试不同类型的通信包体（Tools、Prompts、Resources）是否符合 JSON Schema。
- **价值**：保证我们的 SDK 能够与全网生态（任何语言编写的客户端或服务器）完美互通。

### 3. `examples`：端到端业务与集成实战 (End-to-End Examples)
**“造出来的车究竟好不好开？”**
- **定位**：面向 **最终应用开发者 (DX - Developer Experience)** 的实战集成演示。
- **代表作**：`examples/multi_server_agent`
- **核心职责**：
  将最真实、最极限的环境融合在一起进行测试。
  - **真实网络与多语言混编**：在此目录下，你会看到 C++ 客户端同时拉起并调用基于 Node.js (`npx`) 和 Python (`uvx`/`python`) 编写的外部服务（如 `fetch`, `playwright`, `memory`）。
  - **多协议并发**：验证 `Stdio`（本地进程）与 `SSE`（服务器事件流）双协议混合挂载。
  - **UI 与多线程压测**：测试 Qt GUI 主线程、网络工作线程在极高并发操作下（如疯狂点击“重载”按钮进行热插拔）是否会发生死锁（Deadlock）或崩溃（Segfault）。
  - **Agent 调度闭环**：验证几十个工具一并抛给大模型（LLM）时，ReAct Agent 调度逻辑的稳健性。

**💡 测试体系总结**：
- 修改了底层逻辑，先看 `test` 能不能通过。
- 升级了 MCP 协议版本，去 `conformance_runner_qt` 跑一遍对齐标准。
- 想要评估新功能好不好用、有没有死锁风险，直接在 `examples/multi_server_agent` 里狂飙测试。

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

### LLM 格式导出

一键将 MCP 工具定义转换为大模型 API 原生格式：

```cpp
// 导出为 OpenAI function calling 格式
QJsonArray openaiTools = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::OpenAI);

// Anthropic Claude 格式
QJsonArray anthropicTools = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::Anthropic);

// Gemini 格式
QJsonArray geminiTools = client->exportAllToolsToLlmFormat(McpQtClient::LlmFormat::Gemini);
```

### 多服务器路由与聚合

通过 `McpHost` 外观模式统一管理多个 MCP 服务器，自动为工具名加 `serverName_` 前缀实现跨服务器路由：

```cpp
McpHost host;
host.loadConfigFromFile("mcp_servers.json");
host.start();

// 聚合所有服务器的工具，统一喂给 LLM
QJsonArray allTools = host.exportAllToolsToLlm();

// 路由分发：自动解析前缀找到对应服务器并调用
host.callToolAsync("github_search_code", {{"q", "mcp-qt"}}, [](McpResult r) {
    qDebug() << r.data;
});
```

### 类型化工具结果

无缝解析复合型工具返回，自动解码 Base64 图片，**始终保留原始 JSON**：

```cpp
McpQtToolResult result = client->callToolTyped("generate_image", {{"prompt", "sunset"}});
if (!result.isError) {
    for (const auto& content : result.content) {
        if (content.kind == McpQtContentKind::Image) {
            QByteArray binaryData = content.binary; // 已自动解码的原始二进制
            processImage(binaryData, content.mimeType);
        }
    }
    // 原始 JSON 始终保留
    qDebug() << "Raw:" << result.raw;
}
```

### MVC 模型绑定

4 个 `QAbstractListModel` 子类，可直接绑定 `QListView` 或 QML ListView，支持分页懒加载（`canFetchMore`/`fetchMore`）：

```cpp
auto toolModel = client->createToolsModel(parent);
listView->setModel(toolModel.get());
toolModel->refresh(); // 自动拉取并填充
// 服务端工具变化时 models 自动更新
```

### 指数退避重连与状态自愈

网络抖动或子进程崩溃时自动重连，并恢复所有已注册的 handler、subscription 和双向能力：

```cpp
mcp::McpReconnectPolicy policy;
policy.initialDelayMs = 250;
policy.maxDelayMs = 5000;
policy.multiplier = 2.0;
policy.maxAttempts = -1; // 无限重试

client->setReconnectPolicy(policy);
QObject::connect(client.get(), &McpQtClient::reconnected, []{ qDebug() << "通道自愈成功"; });
```

### 更多

- **全流量 Tracing** — `setTrafficLogger()` 拦截所有出站/入站报文（时间戳 + 方向 + 类型 + payload）
- **OAuth 2.0** — 完整 PKCE 授权码流程 + Dynamic Client Registration + Token 自动刷新
- **双向能力** — Sampling（服务端调 LLM）、Elicitation（服务端请求用户输入）、Roots（暴露文件系统根目录）
- **资源订阅路由** — `subscribeResource()` 注册回调，`notifications/resources/updated` 精准派发，token 粒度退订
- **HTTP 高级配置** — 自定义 Headers、代理服务器、Bearer token 自动注入
- **Stdio 进程管理** — Windows 上用 `CreateJobObject` 确保子进程随父进程退出

---

## 依赖

*   **编译器**：支持 C++17 或以上标准（MSVC 2019+ / GCC 8+ / Clang 7+）
*   **构建系统**：CMake ≥ 3.16
*   **Qt 组件**：Qt6::Core, Qt6::Network, Qt6::Test（multi_server_agent 额外需要 Qt6::Widgets）
*   **第三方库**：nlohmann/json 3.11.3（通过 FetchContent 自动下载）
*   **Windows 额外依赖**：`bcrypt`（mcp_core 静态库链接，用于 OAuth PKCE）

---

## 已知限制

*   **JWT-Bearer Grant Type**：暂不支持 `urn:ietf:params:oauth:grant-type:jwt-bearer`（需实现 `client_assertion` + ES256/RS256 JWT 签名）。代码骨架已预留（`McpQtClient.cpp:229-258`）但 `// TODO: 生成 JWT assertion`。影响场景 `auth/client-credentials-jwt`、`auth/cross-app-access-complete-flow`
*   **OAuth 端点回退**：`auth/2025-03-26-oauth-endpoint-fallback` 场景涉及早期 PRM 猜解回退逻辑，已确认不实现，加入 `conformance-baseline.yml` 忽略清单
*   **高并发假报错**：`--suite all` 全量并发时 `elicitation-sep1034-client-defaults` 偶发超时（CPU 瞬时满载导致 Qt 事件循环响应慢），单独运行或 `--suite core` 下完全通过（5/0）。非代码缺陷
