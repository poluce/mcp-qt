# mcp-cpp-agent

纯 C++17 实现的 **Model Context Protocol (MCP) 客户端 SDK**。

提供两套完整的客户端实现，均通过官方 conformance 测试套件验证。

---

## 官方合规

| 套件 | C++ 版 (libcurl) | Qt 版 (QNAM) |
|------|:---:|:---:|
| `--suite core` (18 场景) | **235/235** ✅ | 233/235 |
| `--suite all` (26 场景) | **287/294** ✅ | — |
| 新协议 (22 场景) | **287/287 100%** ✅ | — |

> C++ 版全部通过，Qt 版仅 `sse-retry` 有时间相关的 1 项差距（QTimer 与 `sleep_for` 的精度差异）。

---

## 快速开始

### C++ 版（libcurl + httplib）

```cpp
#include <mcp_core/mcp_core.h>

// HTTP/SSE 连接
auto transport = std::make_shared<mcp::HttpSseTransport>("http://localhost:8080/mcp");
auto session   = mcp::McpClientSession::connect(transport);
session->initializeSync("my-app", "1.0.0");
auto tools = session->listToolsSync();
auto result = session->callToolSync("add", {{"a", 5}, {"b", 3}});
```

### Qt 版（纯 QNAM，零 libcurl）

```cpp
#include <mcp_qt_client/McpQtClient.h>

// 一行创建，自动完成 transport + init + start + initialize
auto client = mcp_qt::McpQtClient::connectHttp("http://localhost:8080/mcp");

// 同步 API
auto tools    = client->listTools();
auto result   = client->callTool("add", {{"a", 5}, {"b", 3}});
auto resource = client->readResource("file:///data/config.json");
auto prompt   = client->getPrompt("greeting", {{"name", "World"}});

// OAuth 认证
mcp_qt::McpQtClient::OAuthConfig oa;
oa.serverUrl    = "https://secure-server.com/mcp";
oa.clientId     = "my-client-id";
oa.clientSecret = "my-secret";
auto authClient = mcp_qt::McpQtClient::connectWithOAuth(oa);

// Stdio 子进程
auto stdioClient = mcp_qt::McpQtClient::connectStdio("python", {"server.py"});

// 双向能力
client->setElicitationHandler([](const QJsonObject& params) { ... });
client->setSamplingHandler([](const QJsonObject& params) { ... });
client->setRootsProvider([]() -> QJsonArray { ... });

// 信号
QObject::connect(client.get(), &McpQtClient::connected,    []{ qDebug() << "connected"; });
QObject::connect(client.get(), &McpQtClient::disconnected, []{ qDebug() << "disconnected"; });
```

---

## API 概览

### McpQtClient 完整 API

| 分类 | 方法 |
|------|------|
| 创建 | `connectHttp(url)` `connectStdio(cmd, args)` `connectWithOAuth(config)` |
| 工具 | `listTools()` `listTools(cursor, &next)` `callTool(name, args)` `callTool(name, args, onProgress)` |
| 资源 | `listResources()` `readResource(uri)` `subscribeResource(uri)` `unsubscribeResource(uri)` |
| 资源模板 | `listResourceTemplates()` |
| 提示词 | `listPrompts()` `getPrompt(name, args)` |
| 其他 | `ping()` `complete(ref, arg)` `setLoggingLevel(level)` |
| 双向 | `setElicitationHandler()` `setSamplingHandler()` `setRootsProvider()` `notifyRootsListChanged()` |
| 通知 | `registerNotificationHandler()` `enableNotificationDebounce()` `sendNotification()` |
| 异步 | `sendRequest(method, params, callback)` `cancelRequest(id)` |
| 生命周期 | `isConnected()` `close()` |

### McpClientSession 底层 API

三套 API 风格：**异步回调** / **同步阻塞** / **Raw 字符串**

| 操作 | 异步 | 同步 |
|------|------|------|
| 初始化 | `initialize()` | `initializeSync()` |
| 关闭 | `shutdown()` | `shutdownSync()` |
| 工具 | `listTools()` `callTool()` | `listToolsSync()` `callToolSync()` |
| 资源 | `listResources()` `readResource()` `subscribeResource()` `unsubscribeResource()` | `*Sync()` |
| 提示词 | `listPrompts()` `getPrompt()` | `*Sync()` |
| Ping | `ping()` | `pingSync()` |

---

## 构建

```bash
# C++ 版（含 HTTP/SSE 传输 + OAuth）
cmake -B build -DMCP_ENABLE_HTTP=ON
cmake --build build

# Qt 版（含 QtHttpSseTransport + McpQtClient）
cmake -B build -DMCP_ENABLE_HTTP=ON -DMCP_ENABLE_QT_TRANSPORT=ON
cmake --build build

# 纯 Stdio 模式（跳过 HTTP 依赖，编译仅需 10 秒）
cmake -B build -DMCP_ENABLE_HTTP=OFF
cmake --build build
```

---

## 项目结构

```
mcp-cpp-agent/
 ├── mcp_core/                       # SDK 核心（纯 C++17）
 │    ├── include/mcp_core/
 │    │    ├── mcp_core.h                     # 一站式头文件
 │    │    ├── McpClientSession.h              # 客户端会话
 │    │    ├── IMcpTransport.h                 # 传输层接口
 │    │    ├── HttpSseTransport.h              # libcurl SSE 传输
 │    │    ├── ConsoleStdioTransport.h         # 控制台 Stdio
 │    │    ├── SubprocessStdioTransport.h      # 子进程 Stdio
 │    │    ├── McpOAuthClient.h                # OAuth 客户端
 │    │    └── JsonRpcDispatcher.h             # JSON-RPC 分发器
 │    └── src/
 │
 ├── mcp_qt_transport/               # Qt 传输层（QNAM，零 libcurl）
 │    ├── include/mcp_qt_transport/
 │    │    └── QtHttpSseTransport.h
 │    └── src/
 │
 ├── mcp_qt_client/                  # Qt 高层客户端（QObject，信号/槽）
 │    ├── include/mcp_qt_client/
 │    │    └── McpQtClient.h
 │    └── src/
 │
 ├── conformance_runner/             # 官方合规测试客户端（C++ 版）
 ├── conformance_runner_qt/          # 官方合规测试客户端（Qt 版）
 ├── tests/                          # 单元测试
 └── tests_qt/                       # Qt 传输层测试
```

---

## 依赖

| 组件 | 依赖 |
|------|------|
| mcp_core | C++17, nlohmann/json, libcurl |
| mcp_qt_transport | Qt6::Core, Qt6::Network |
| mcp_qt_client | mcp_qt_transport, mcp_core |

---

## 传输层选择

| 场景 | 推荐 |
|------|------|
| 本地 MCP 子进程 | `SubprocessStdioTransport` |
| Qt 应用远程 HTTP/HTTPS | `QtHttpSseTransport` + `McpQtClient` |
| 非 Qt 环境远程 HTTP/HTTPS | `HttpSseTransport` |
| 自定义协议（WebSocket 等） | 实现 `IMcpTransport` 接口 |
