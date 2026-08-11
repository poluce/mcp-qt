# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

纯 C++17 / Qt6 实现的 Model Context Protocol (MCP) 客户端 SDK。通过 Qt Network Access Manager 进行 HTTP/SSE 通信，零 libcurl 依赖。

## 构建命令

```bash
# 标准构建
cmake -B build
cmake --build build

# 运行全部测试
cd build && ctest

# 运行单个测试
./build/test/tests_qt testToolsModel

# 列出所有测试用例
./build/test/tests_qt -help

# HTTPS 运行时测试（独立目标，防止 TLS 动态装载冲突）
cmake --build build --target tests_qt_https_runtime
./build/test/tests_qt_https_runtime

# 合规测试（需先在另一个终端启动测试服务器）
node test_mcp_server.js --http &
./build/conformance_runner_qt/mcp_client_conformance_qt --suite core
./build/conformance_runner_qt/mcp_client_conformance_qt --suite all
```

## 项目架构

### 分层依赖

```
mcp_core (纯 C++17 + nlohmann/json, 零 Qt 依赖)
    ↑
mcp_qt_transport (Qt6::Core + Qt6::Network)
    ↑
mcp_qt_client (整合层, QObject 信号/槽)
```

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

**核心类：**

| 类 | 职责 | 行数 |
|---|---|---|
| `IMcpTransport` | 传输层抽象接口：纯 C++ 回调（send/onMessage/onClose/onError/start/close），不依赖 Qt | 58 |
| `McpClientSession` | **最大核心类**。MCP 协议生命周期管理：JSON-RPC 分发、请求超时清理、双向能力（Sampling/Elicitation/Roots）、通知去重。所有操作提供异步回调 + 同步阻塞两套 API，外加 Raw String API（不依赖 nlohmann/json 的字符串版本） | 444 |
| `JsonRpcDispatcher` | JSON-RPC 2.0 消息解析：注册 handler → dispatch 原始消息 → 返回序列化响应 | 56 |
| `McpOAuthClient` | OAuth 2.0 完整流程：发现元数据(RFC 8414) → 动态注册(RFC 7591) → PKCE 授权码(RFC 7636) → token 交换与刷新。纯 C++ 实现 HTTP GET/POST，thread-safe | 185 |

> ⚠️ `mcp_core.h` 聚合头引用了 `SubprocessStdioTransport.h` 和 `HttpSseTransport.h`，这两个文件已迁移到 transport 层，include 是**遗留的无效引用**，不应使用。

#### transport/ — Qt 传输层（代码使用 `mcp_qt::` 命名空间，但头文件路径为 `mcp_qt_transport/`）

`IMcpTransport` 的三个 Qt 实现：

| 类 | 传输方式 | 设计 | 关键细节 |
|---|---|---|---|
| `QtHttpSseTransport` | HTTP/SSE 长连接 | **Pimpl 模式**（公开头仅 48 行，Impl 隐藏 `QtHttpSseWorker`） | 双向通道：POST 发请求 → GET SSE 收响应/通知。支持 `TokenProvider`（Bearer token 自动注入）和 `AuthRetryHandler`（401 重试）。内部有 endpoint 竞态条件修复 |
| `QtStatelessHttpTransport` | 无状态 HTTP | 直接继承 `QObject` + `IMcpTransport` | POST 请求后根据 `Content-Type` 被动适配 JSON/SSE。收到 `initialized` 通知后启动 GET SSE 监听流。支持 session ID、OAuth 401 重试（最多 3 次） |
| `QtProcessStdioTransport` | 子进程 stdin/stdout | 继承 `QObject` + `IMcpTransport` | 通过 `QProcess` 管理子进程。**Windows 上用 `CreateJobObject`** 确保子进程随父进程退出。`serverLog` 信号分离 stderr（与传输错误分开） |

**内部组件（`src/transport/src/`，不对外暴露）：**

| 类 | 职责 |
|---|---|
| `QtHttpSseWorker` | SSE 连接的工作线程（QObject）：管理 POST 消息发送、GET SSE Stream 监听、断线自动重连、健康检查（基于 `QElapsedTimer`）、pending 消息缓冲重放 |
| `QtSseParser` | SSE 协议增量解析器：`pushChunk()` 增量喂入，`flushEventBlock()` 解析 `event:`/`data:`/`id:`/`retry:` 字段，callback 通知 `QtSseEvent` |

#### client/ — Qt 高层封装（`mcp_qt::` 命名空间）

**配置与错误：**
| 类 | 职责 |
|---|---|
| `McpServerConfig` | 纯值类型：serverName/command/args/url/type/nameSpace/env/headers |
| `IMcpConfigLoader` | 配置加载器抽象接口：`virtual QList<McpServerConfig> load() = 0` |
| `McpJsonConfigLoader` | JSON 文件 → `McpServerConfig` 列表，支持 `$ENV_VAR` 环境变量插值 |
| `McpError` | 强类型错误（code + message + data），`Q_DECLARE_METATYPE` 支持跨线程传递 |

**核心客户端（最大类，头文件 572 行）：**
| 类 | 职责 |
|---|---|
| `McpQtClient` | QObject 封装 `McpClientSession`，信号/槽驱动。涵盖：同步/异步/批量/并发/Future 多套 API、LLM 格式导出（OpenAI/Anthropic/Gemini）、重连自愈（恢复 handler + 回放请求队列）、`QObject* context` 生命周期保护、工具列表缓存、本地参数校验 |
| `McpQtClientBuilder` | Builder 模式：链式配置 transport → headers → proxy → reconnect policy → build（同步 `AndWait` / 异步 `Async`） |

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

### 关键设计模式

- **双 API 设计**：每个 MCP 操作同时提供同步版（`callTool()` 内部跑局部事件循环阻塞等待）和异步版（`callToolAsync()` 回调 / `callToolFuture()` 返回 `QFuture`）。同步 API 仅限非 GUI 线程使用
- **Builder 模式**：`McpQtClientBuilder` 链式配置后调用 `buildAndConnectAndWait()`（同步）或 `buildAndConnectAsync()`（异步）
- **Pimpl 模式**：`QtHttpSseTransport` 公开头仅 48 行，Impl 隐藏 `QtHttpSseWorker`、`QNetworkAccessManager` 和所有 SSE 细节。注意 `QtStatelessHttpTransport` 和 `QtProcessStdioTransport` 未使用 Pimpl，直接暴露内部成员
- **重连自愈**：`McpReconnectPolicy` 指数退避重连 + `McpQtClient` 自动恢复已注册的 notification handler、resource subscription、双向能力处理器。重连期间收到的"可重放"请求排队（`ReplayableRequest` 队列），连接恢复后批量回放
- **QObject 生命周期保护**：异步 API 接受 `QObject* context` 参数，回调执行前检查 `context.isNull()`，防止野指针；回调通过 `QMetaObject::invokeMethod` 切回 context 所在线程
- **通知去重**：`McpClientSession::enableNotificationDebounce()` 在指定时间窗口内合并重复通知，避免高频震荡；`sendNotificationDebounced()` 在 debounce 窗口内只发最后一条
- **外观模式**：`McpHost` 组合 `McpServerManager` + Router 三部曲 + `McpDiagnosticReporter`，对外暴露统一接口。内嵌启动看门狗定时器，超时后 `finishStartup` 报告失败
- **线程安全订阅路由**：`McpResourceSubscriptionRouter` 使用 mutex + 快照模式（锁内拷贝回调列表到栈上，锁外遍历执行），防止回调内退订造成死锁
- **LLM 格式导出**：`McpQtClient::exportAllToolsToLlmFormat()` 支持 OpenAI/Anthropic/Gemini 三种格式，`McpToolRouter` 在跨服务器场景下自动加 `serverName_` 前缀后统一导出

## 测试体系

### 三维测试矩阵

| 维度 | 目录 | 定位 | 框架 |
|------|------|------|------|
| 组件单元测试 | `test/` | SDK 内部白盒/灰盒测试 | Qt Test |
| 协议合规测试 | `conformance_runner_qt/` | 官方 MCP 协议黑盒验证 | 外部标准服务器 |
| 端到端实战 | `examples/` | 真实多语言混编、GUI 并发压测 | 手动/自动化 |

### 测试文件说明

`test/CMakeLists.txt` 定义了两个目标：
- `tests_qt`：主测试可执行文件（14 个测试文件），链接 `mcp_qt_transport` + `mcp_qt_client`
- `tests_qt_https_runtime`：独立的 HTTPS 运行时测试（`EXCLUDE_FROM_ALL`），防止 TLS 动态装载冲突挂掉主测试

### 合规测试状态

测试日期：2026-07-07 | 协议版本：2025-11-25 | 通过率：85%（22/26 场景通过，2 警告，2 真实失败）

已修复：`sse-retry`（Race Condition 缓冲队列 + 冷却型探活机制）
警告：`auth/basic-cimd`（1 个非致命警告）、`elicitation-sep1034-client-defaults`（并发假报错）
真实失败：`auth/cross-app-access-complete-flow`（EMA 完整流程：token exchange + jwt-bearer grant，归 C2 扩展，见 docs/缺失功能清单.md）、`auth/2025-03-26-oauth-endpoint-fallback`（已加入 baseline 忽略清单）

> 注：`auth/client-credentials-jwt` 已于 2026-08-11 复测通过（ES256 实现，8/8），从真实失败列表中移除。

## 依赖项

- CMake ≥ 3.16
- Qt 6.x（Core, Network, Test；multi_server_agent 额外需要 Widgets）
- C++17 编译器（MSVC 2019+ / GCC 8+ / Clang 7+）
- nlohmann/json 3.11.3（通过 `FetchContent` 自动下载）
- Windows 上 `mcp_core` 额外链接 `bcrypt`（用于 OAuth PKCE）

## 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `MCP_ENABLE_HTTP` | ON | 启用 HTTP/SSE 传输和 OAuth 客户端 |
| `MCP_ENABLE_QT_TRANSPORT` | OFF | 启用 Qt 传输层和客户端 |

## 示例项目

| 示例 | 演示内容 |
|------|---------|
| `examples/multi_server_agent` | 完整 GUI 应用：多 MCP 服务器聚合、LLM Agent 调度（ReAct 模式）、Stdio+SSE 混合挂载、热插拔重载、Python/Node 混编服务器 |
| `examples/anysearch_qt` | 轻量搜索客户端示例 |

`multi_server_agent` 编译为静态库 `multi_server_agent_core` + 可执行文件，测试目录会链接该库以复用 Agent 组件。

## 编码约定

- **命名空间**：核心协议用 `mcp::`，transport 和 client 层代码中用 `mcp_qt::`。⚠️ transport 层目录名为 `mcp_qt_transport`，头文件路径为 `mcp_qt_transport/`，但**实际 C++ 命名空间是 `mcp_qt::`**（不带 `_transport` 后缀）
- **头文件包含**：优先使用前向声明，避免在 .h 中包含大型头文件。`McpQtClient.h` 使用 `QPointer<T>` 而非 `T*` 来安全引用 QObject 子对象
- **中文注释**：UTF-8 编码直接写汉字，不使用 `\uXXXX` 转义
- **段落注释**：超过 10 行的函数体按逻辑阶段用 `// 中文描述` 拆分，段间留空行
- **MSVC 编译**：需要 `/utf-8` 选项（已在各 CMakeLists.txt 中配置）和 `/Zc:__cplusplus`（根 CMakeLists.txt）
- **CMAKE_AUTOMOC**：Qt 传输层和客户端需要启用 `set(CMAKE_AUTOMOC ON)`
- **线程安全**：`McpClientSession` 使用 `std::mutex` 保护请求表 + `std::atomic` 保护状态标志。`McpResourceSubscriptionRouter` 使用 mutex + 快照模式。`McpOAuthClient` 使用 mutex 保护 token 状态。`McpQtClient` 的 `m_replayMutex` 和 `m_pendingFetchMutex` 分别保护请求回放队列和挂起的 fetch 回调
- **值类型序列化**：core 层的值类型（`McpTool`、`McpResource`、`McpPrompt` 等）使用 `toJson()`/`fromJson()` 成员函数而非 `to_json`/`from_json` 全局函数（后者仅用于 `RequestId` variant）

## 平台支持

- Windows（主要开发平台）
- Linux
- macOS

## 测试服务器

```bash
# Stdio 模式（默认）
node test_mcp_server.js

# HTTP/SSE 模式（合规测试需要）
node test_mcp_server.js --http
```

## 注意事项

- Windows 路径使用双反斜杠 `\\`
- 中文注释使用 UTF-8 编码，不使用 `\uXXXX` 转义
- 合规测试需要先启动测试服务器（见 `test_mcp_server.js`）

## Git 规则

- **不擅自删除暂存区内容**：不执行 `git reset HEAD`、`git rm --cached` 等清除暂存区的操作，除非用户明确要求
- **不擅自添加到暂存区**：不执行 `git add` 将工作区修改加入暂存区，除非用户明确要求提交
