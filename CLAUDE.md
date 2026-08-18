# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

纯 C++17 / Qt6 实现的 Model Context Protocol (MCP) 客户端 SDK。通过 Qt Network Access Manager 进行 HTTP/SSE 通信，零 libcurl 依赖。

当前分支支持 **MCP 2026-07-28** 无状态核心（stateless / MRTR / Header 路由 / OAuth 硬化），并保留 2025-11-25 / 2025-06-18 / 2025-03-26 的 legacy 握手路径。官方 conformance 2026-07-28 客户端套件的已记录结果为 **32/32**（见 `README.md` 与 `docs/mcp_2026-07-28_规范对照审计.md` 第五节）。

## 构建命令

```bash
# 标准构建
cmake -B build
cmake --build build

# 运行全部测试
cd build && ctest

# 主单元/集成目标（CMake 将 test/ 编到 build_test/）
./build/build_test/tests_qt

# HTTPS 运行时测试（独立目标，防止 TLS 动态装载冲突）
cmake --build build --target tests_qt_https_runtime
./build/build_test/tests_qt_https_runtime

# MCP Apps 端到端（独立目标，需另起 node 测试服务器）
cmake --build build --target tests_qt_apps_e2e
./build/build_test/tests_qt_apps_e2e

# 官方合规 runner（2026-07-28 场景由 conformance CLI 拉起 runner）
npx -y @modelcontextprotocol/conformance client \
  --command "build/conformance_runner_qt/mcp_client_conformance_qt" \
  --spec-version 2026-07-28
```

根构建脚本（`CMakeLists.txt`）没有按层裁剪的可选开关：`src/core`、`src/transport`、`src/client`、`src/apps` 始终加入构建。

## 项目架构

### 分层依赖

```
mcp_core (纯 C++17 + nlohmann/json, 零 Qt 依赖)
    ↑
mcp_qt_transport (Qt6::Core + Qt6::Network)
    ↑
mcp_qt_client (整合层, QObject 信号/槽)
    ↑
mcp_qt_apps (MCP Apps 扩展, Widgets + WebView2)
```

四层源码目录：`src/core`、`src/transport`、`src/client`、`src/apps`。

### 各层职责与关键类

#### core/ — 协议核心（`mcp::` 命名空间，纯 C++17 零 Qt 依赖）

**数据结构（纯值类型，自带 `toJson`/`fromJson` 序列化）：**

| 头文件 | 定义 |
|---|---|
| `McpMessage.h` | `RequestId`（`variant<monostate,int64_t,string>`）、`McpRequest`、`McpResponse`、`McpNotification` |
| `McpTool.h` | `McpTool`、`ToolAnnotations`、`isValidToolName()` 正则校验（1-128字符，`[a-zA-Z0-9_.-]+`） |
| `McpResource.h` | `McpResource`、`McpResourceTemplate`、`McpResourceContent` |
| `McpPrompt.h` | `McpPrompt`、`McpPromptArgument`、`McpPromptMessage`、`McpPromptResult` |
| `McpReconnectPolicy.h` | 重连策略结构体，`getDelayMs(attempt)` 计算指数退避延迟 |
| `McpTrafficEvent.h` | 流量追踪事件（方向 Outbound/Inbound × 类型 Request/Response/Notification） |
| `McpHeaderEncoding.h` | SEP-2243 Header Base64 sentinel 编码 |

**核心类：**

| 类 | 职责 |
|---|---|
| `IMcpTransport` | 传输层抽象接口：纯 C++ 回调（send/onMessage/onClose/onError/start/close），不依赖 Qt |
| `McpClientSession` | 协议生命周期：JSON-RPC 分发、请求超时、双向能力、通知去重；stateless（2026-07-28，免握手 + 每请求 `_meta`）与 legacy initialize 双路径。异步回调 + 同步阻塞 + Raw String API |
| `JsonRpcDispatcher` | JSON-RPC 2.0 消息解析：注册 handler → dispatch 原始消息 → 返回序列化响应 |
| `McpOAuthClient` | OAuth 2.0：发现(RFC 8414) → DCR(RFC 7591) → PKCE(RFC 7636) → token 交换与刷新；含 RFC 9207 `iss` 校验、issuer 凭据绑定 |

传输实现不在 core：HTTP/SSE 与 Stdio 在 `src/transport/`。

#### transport/ — Qt 传输层（代码使用 `mcp_qt::` 命名空间，头文件路径为 `mcp_qt_transport/`）

`IMcpTransport` 的三个 Qt 实现：

| 类 | 传输方式 | 设计 | 关键细节 |
|---|---|---|---|
| `QtHttpSseTransport` | HTTP/SSE 长连接 | **Pimpl**（公开头短，Impl 隐藏 `QtHttpSseWorker`） | POST 发请求 → GET SSE 收响应/通知。`TokenProvider` + `AuthRetryHandler`（401 重试）。endpoint 就绪前缓冲待发消息 |
| `QtStatelessHttpTransport` | 无状态 HTTP | 直接继承 `QObject` + `IMcpTransport` | POST 后按 `Content-Type` 适配 JSON/SSE。2026-07-28 走 `Mcp-Method` / `Mcp-Name` Header 路由，不发送协议级 `Mcp-Session-Id`。OAuth 401 重试（最多 3 次） |
| `QtProcessStdioTransport` | 子进程 stdin/stdout | 继承 `QObject` + `IMcpTransport` | `QProcess` 管理子进程。**Windows 上用 `CreateJobObject`** 确保子进程随父进程退出。`serverLog` 信号分离 stderr |

**内部组件（`src/transport/src/`，不对外暴露）：**

| 类 | 职责 |
|---|---|
| `QtHttpSseWorker` | SSE 工作对象：POST 发送、GET SSE 监听、断线重连、健康检查、pending 缓冲重放 |
| `QtSseParser` | SSE 增量解析：`pushChunk()` → `event:`/`data:`/`id:`/`retry:` → `QtSseEvent` |

#### client/ — Qt 高层封装（`mcp_qt::` 命名空间）

**配置与错误：**
| 类 | 职责 |
|---|---|
| `McpServerConfig` | 纯值类型：serverName/command/args/url/type/nameSpace/env/headers |
| `IMcpConfigLoader` | 配置加载器抽象接口：`virtual QList<McpServerConfig> load() = 0` |
| `McpJsonConfigLoader` | JSON 文件 → `McpServerConfig` 列表，支持 `$ENV_VAR` 环境变量插值 |
| `McpError` | 强类型错误（code + message + data），`Q_DECLARE_METATYPE` 支持跨线程传递 |

**核心客户端：**
| 类 | 职责 |
|---|---|
| `McpQtClient` | QObject 封装 `McpClientSession`。同步 / 异步回调 / QFuture / 批量 / 类型化多套 API；LLM 格式导出；重连自愈；`QObject* context` 生命周期保护；2026 能力（discover / MRTR `inputRequired` / `listenSubscriptions` / CacheableResult / per-request logLevel / W3C trace） |
| `McpQtClientBuilder` | 链式配置 transport → headers → proxy → `setProtocolVersion` / `setStatelessMode` → reconnect → build |

**类型化结果与解析：**
| 类 | 职责 |
|---|---|
| `McpQtContent` | 单块内容（Text/Image/EmbeddedResource/Unknown），Base64 自动解码为 `QByteArray binary`，始终保留 `QJsonObject raw` |
| `McpQtToolResult` | 工具返回值（content 列表 + `structuredContent` + `raw` + `isError`），**永不丢弃原始 JSON** |
| `McpParser` | 静态工具类：从 content 数组提取并解码 Base64 图片，兼容 Qt5/Qt6 API 差异 |

**MVC 模型适配器（全部继承 `QAbstractListModel`，支持 `canFetchMore`/`fetchMore` 分页，QML 可直接绑定）：**
| 类 | roleNames |
|---|---|
| `McpToolsModel` | Name, Description, InputSchema |
| `McpPromptsModel` | Name, Description, Arguments |
| `McpResourcesModel` | Uri, Name, Description, MimeType |
| `McpResourceTemplatesModel` | UriTemplate, Name, Description, MimeType |

**多服务器管理：**
| 类 | 职责 |
|---|---|
| `McpServerManager` | 多 client 生命周期管理、心跳保活（`QTimer` 定期 ping）、工具预热（`fetchAllToolsAsync`）、状态聚合发射 |
| `McpToolRouter` | 工具名加 `serverName_` 前缀 → 路由分发 → 剥离前缀 → 调用对应 client。支持导出到 LLM 格式（OpenAI/Anthropic/Gemini） |
| `McpPromptRouter` | 同前缀路由模式，提示词跨服务器分发 |
| `McpResourceRouter` | URI 重写为 `mcp-{serverName}-{uri}` 避免冲突，跨服务器读资源 |
| `McpHost` | **外观模式**：`loadConfigFromFile → start → exportAllToolsToLlm → callToolAsync` 一站式入口，内嵌启动看门狗超时 |

**诊断与订阅路由：**
| 类 | 职责 |
|---|---|
| `McpDiagnosticReporter` | Info/Warning/Error 三级，按 stage 分组，生成执行日志 + 文本报告 |
| `McpResourceSubscriptionRouter` | URI → callback 列表映射。**线程安全**：mutex + 快照模式（锁内拷贝回调列表，锁外执行），token 粒度退订 |

#### apps/ — MCP Apps 扩展（`mcp_qt::`，头文件路径 `mcp_qt_apps/`）

| 类 | 职责 |
|---|---|
| `McpAppSupport` | 能力声明、`ui://` 资源获取、CSP / permissions 辅助 |
| `IMcpAppRenderer` | 渲染抽象（可插拔） |
| `McpAppWebView2Renderer` | Windows WebView2 后端（动态加载 `WebView2Loader.dll`） |
| `McpAppBridge` | App ↔ Host ↔ MCP 服务器代理（tools/call、权限、ui/ 方言） |

多 Agent 注册与对账（`AgentRegistry` + `AgentReconciler`）在 `examples/multi_server_agent/`，不是未实现草案。

### 关键设计模式

- **双 API 设计**：每个 MCP 操作同时提供同步版（`callTool()` 内部跑局部事件循环阻塞等待）和异步版（`callToolAsync()` 回调 / `callToolFuture()` 返回 `QFuture`）。同步 API **仅限非 GUI 线程**；GUI 线程必须用异步 API
- **Builder 模式**：`McpQtClientBuilder` 链式配置后调用 `buildAndConnectAndWait()`（同步）或 `buildAndConnectAsync()`（异步）
- **Pimpl 模式**：`QtHttpSseTransport` 公开头短，Impl 隐藏 `QtHttpSseWorker`、`QNetworkAccessManager` 和 SSE 细节。`QtStatelessHttpTransport` 和 `QtProcessStdioTransport` 未使用 Pimpl
- **重连自愈**：`McpReconnectPolicy` 指数退避 + `McpQtClient` 恢复 handler / subscription / 双向能力。重连期间可重放请求排队，恢复后批量回放
- **QObject 生命周期保护**：异步 API 接受 `QObject* context`，回调前检查 `context.isNull()`；经 `QMetaObject::invokeMethod` 切回 context 线程
- **通知去重**：`McpClientSession::enableNotificationDebounce()` 合并窗口内重复通知；`sendNotificationDebounced()` 只发最后一条
- **外观模式**：`McpHost` 组合 `McpServerManager` + Router 三部曲 + `McpDiagnosticReporter`
- **线程安全订阅路由**：`McpResourceSubscriptionRouter` mutex + 快照模式
- **LLM 格式导出**：`McpQtClient::exportAllToolsToLlmFormat()` 支持 OpenAI/Anthropic/Gemini；跨服务器时 `McpToolRouter` 自动加 `serverName_` 前缀

## 测试体系

### 三维测试矩阵

| 维度 | 目录 | 定位 | 框架 |
|------|------|------|------|
| 组件单元测试 | `test/` | SDK 内部白盒/灰盒测试 | `QCoreApplication` + `test/common.h` 宏（`TM_RUN_TEST` / `TM_ASSERT_*`），链接 Qt6::Test |
| 协议合规测试 | `conformance_runner_qt/` | 官方 MCP 协议黑盒验证 | 官方 conformance CLI + 本仓库 runner |
| 端到端实战 | `examples/` | 真实多语言混编、GUI、MCP Apps 渲染 | 手动/独立 e2e 目标 |

### 测试目标（与 `test/CMakeLists.txt` 一致）

- `tests_qt`：主测试可执行文件。源文件列在 `test/CMakeLists.txt`；入口 `test/main_qt.cpp`；链接 `mcp_qt_transport` + `mcp_qt_client` + `mcp_qt_apps`。产物路径：`build/build_test/tests_qt`
- `tests_qt_https_runtime`：独立 HTTPS 运行时测试（`EXCLUDE_FROM_ALL`），防止 TLS 动态装载冲突
- `tests_qt_apps_e2e`：MCP Apps 端到端（`EXCLUDE_FROM_ALL`），需 node 起 mock / 外部服务器

不要在未实际跑通 `tests_qt` 时写入新的 `N/N` 通过数。较早文档里的 `27/27` 是 2026-08-07 的历史快照。

写法说明见 `test/TEST_WORKFLOW.md`。

### 合规测试状态

- **2026-07-28 官方客户端套件**（已记录）：**32/32**。明细见 `README.md` 与 `docs/mcp_2026-07-28_规范对照审计.md` 第五节。
- **legacy `--suite all`**（2026-07-07 快照，非今日结论）：见 `conformance_runner_qt/调试笔记.md`。当时剩余真实缺口包括 EMA 完整流程（C2）与 `auth/2025-03-26-oauth-endpoint-fallback`（baseline 忽略）。`auth/client-credentials-jwt` 已于 2026-08-11 复测通过（ES256，8/8）。

仍未实现 / 未跑的项以 `docs/缺失功能清单.md` 为准：C1 Tasks、C2 EMA、C3 DPoP、C4 WIF、C6 MRTR sampling/roots 专项测试、C10 `--suite all`；A5/A6 跨平台渲染明确暂缓。

## 依赖项

- CMake ≥ 3.16
- Qt 6.x（Core, Network, Test；MCP Apps / `multi_server_agent` 另需 Widgets/Gui）
- C++17 编译器（MSVC 2019+ / GCC 8+ / Clang 7+ / MinGW-w64）
- nlohmann/json 3.11.3（通过 `FetchContent` 自动下载）
- Windows 上 `mcp_core` 额外链接 `bcrypt`（OAuth PKCE + client 层 ES256 JWT）
- MCP Apps：`third_party/webview2` + `third_party/wil`；运行时需系统 Edge WebView2 Runtime

## 示例项目

| 示例 | 演示内容 |
|------|---------|
| `examples/multi_server_agent` | 完整 GUI：多服务器聚合、ReAct Agent、Stdio+HTTP 混合、热重载、MCP Apps（WebView2）。内含 `AgentRegistry` / `AgentReconciler`。静态库 `multi_server_agent_core` + 可执行文件；`tests_qt` 直接编译 Registry/Reconciler 源文件做单测 |
| `examples/anysearch_qt` | 轻量搜索客户端示例 |

## 编码约定

- **命名空间**：核心协议用 `mcp::`，transport / client / apps 代码用 `mcp_qt::`。transport 头路径是 `mcp_qt_transport/`，**命名空间不带 `_transport` 后缀**
- **头文件包含**：优先前向声明。`McpQtClient.h` 用 `QPointer<T>` 安全引用 QObject 子对象
- **中文注释**：UTF-8 直接写汉字，不使用 `\uXXXX` 转义
- **段落注释**：超过 10 行的函数体按逻辑阶段用 `// 中文描述` 拆分，段间留空行
- **MSVC 编译**：需要 `/utf-8`（各 CMakeLists.txt）和 `/Zc:__cplusplus`（根 CMakeLists.txt）
- **CMAKE_AUTOMOC**：transport / client / apps / 测试 / 示例需要 `set(CMAKE_AUTOMOC ON)`
- **线程安全**：`McpClientSession` 用 `std::mutex` 保护请求表 + `std::atomic` 保护状态。`McpResourceSubscriptionRouter` mutex + 快照。`McpOAuthClient` mutex 保护 token。`McpQtClient` 的 `m_replayMutex` / `m_pendingFetchMutex` 分别保护回放队列和挂起 fetch
- **值类型序列化**：core 值类型用成员 `toJson()`/`fromJson()`，不用全局 `to_json`/`from_json`（后者仅用于 `RequestId` variant）

## 平台支持

- Windows（主要开发平台；MCP Apps 默认 WebView2）
- Linux
- macOS

## 测试服务器

```bash
# Stdio 模式（默认，legacy mock）
node test_mcp_server.js

# HTTP/SSE 模式
node test_mcp_server.js --http

# 2026-07-28 无状态 mock（discover / MRTR / subscriptions/listen 等）
node test_mcp_server_2026.js
```

## 注意事项

- Windows 路径使用双反斜杠 `\\`
- 中文注释使用 UTF-8 编码，不使用 `\uXXXX` 转义
- 合规测试由官方 CLI 拉起 `mcp_client_conformance_qt`；本地 mock 见上节

## Git 规则

- **不擅自删除暂存区内容**：不执行 `git reset HEAD`、`git rm --cached` 等清除暂存区的操作，除非用户明确要求
- **不擅自添加到暂存区**：不执行 `git add` 将工作区修改加入暂存区，除非用户明确要求提交
