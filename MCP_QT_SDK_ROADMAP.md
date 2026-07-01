# MCP Qt Client SDK 补齐计划与优先级清单 (Roadmap)

本文档旨在罗列当前 `mcp-cpp-agent` (Qt 客户端后端 SDK) 距离“完全合规、对标官方核心能力”还缺失的功能，并根据协议规范的强制性以及工程需要，进行了优先级的划分。

## 一、必须添加 (Must Have) / 重要且紧急 (Important & Urgent)

这些功能直接关系到 MCP 协议的核心交互闭环。如果不实现，将导致客户端与服务端状态脱节，或者服务端拒绝调用客户端提供的能力。

### 1. 列表变更通知 (`list_changed`) 的原生支持
- **原因**：MCP 协议要求服务端在工具、资源、提示词发生变动时，下发特定的通知。如果不处理，客户端的数据将是过期的。
- **具体实现**：
  - 拦截 `notifications/tools/list_changed`，转换为 `toolsChanged()` 信号。
  - 拦截 `notifications/resources/list_changed`，转换为 `resourcesChanged()` 信号。
  - 拦截 `notifications/prompts/list_changed`，转换为 `promptsChanged()` 信号。
- **紧急度**：⭐⭐⭐⭐⭐ (极高)

### 2. Client Capabilities 自动协商补齐
- **原因**：官方 TypeScript SDK 中，只要挂载了处理函数（如 `setSamplingHandler`），客户端会在握手阶段 (`initialize`) 自动声明对应的 capabilities。目前我们的代码依赖开发者手动 `registerCapability`，这不符合 SDK 的易用性标准，且容易导致握手失败。
- **具体实现**：
  - 修改 `doInitialize()`，在发送 `initialize` 请求前，检查当前对象是否设置了 `m_samplingHandler`、`m_elicitationHandler` 或 `m_rootsProvider`。
  - 如果已设置，自动在 `params.capabilities` 中打上对应的标记。
- **紧急度**：⭐⭐⭐⭐⭐ (极高)

---

## 二、重要但不紧急 (Important but not Urgent)

这些功能是评价一个 **“Qt 版本 SDK”** 是否专业和易用的重要指标。缺少它们不违反底层协议，但会大幅增加使用该 SDK 的前端（UI）开发者的工作量。

### 1. 补齐三大支柱数据模型 (Qt Models)
- **原因**：SDK 目前只提供了 `McpToolsModel`，缺少另外两个核心数据类型的 Qt Model 封装。
- **具体实现**：
  - 增加 `McpPromptsModel`：继承自 `QAbstractListModel`，将 `listPrompts` 接口的数据映射为表格/列表。
  - 增加 `McpResourcesModel`：继承自 `QAbstractListModel`，将 `listResources` 接口的数据映射为表格/列表。
- **紧急度**：⭐⭐⭐⭐ (较高)

### 2. 游标分页 (Pagination) 与 Qt 模型的深度结合
- **原因**：当服务端有数万个资源时，MCP 协议要求使用 `cursor` 进行分页。目前 C++ 层提供了带 `cursor` 的重载方法，但对 UI 层不够友好。
- **具体实现**：
  - 在 `McpToolsModel`、`McpResourcesModel` 中重写 `canFetchMore()` 和 `fetchMore()`。
  - 模型内部维护 `nextCursor` 状态，当 ListView 滚动到底部时自动发起网络请求加载下一页。
- **紧急度**：⭐⭐⭐ (中等)

### 3. QML 异步调用支持 (Promise Wrapper)
- **原因**：`McpQtClient` 大量使用 C++11 `std::function` 作为异步回调，这导致无法直接在 QML (JavaScript) 中调用 `callTool` 并 `await` 等待结果。
- **具体实现**：
  - 提供一层 QML 包装层（如 `QmlMcpClient`），将异步方法通过 `QJSValue` 返回 JavaScript Promise，允许前端使用原生的 async/await 语法进行调用。
- **紧急度**：⭐⭐⭐ (中等)

---

## 三、长期演进 / 不紧急 (Long-term / Not Urgent)

属于官方正在推行的最新草案（Draft），或者是特定场景下的高级能力，现有环境的兼容性完全不受影响。

### 1. 新的无状态 HTTP 传输层 (Stateless HTTP Transport - SEP-2575)
- **原因**：MCP 官方在 2026 版本的协议草案中，引入了完全无状态的 POST 请求模式，以替代传统 SSE 连接，主要用于适配 Serverless 和云端架构。
- **具体实现**：
  - 在 `mcp_qt_transport` 目录下新增 `QtStatelessHttpTransport`，继承 `IMcpTransport`。
  - 不再维护长连接线程，而是将每一个收发的 JSON-RPC 包装成独立的 POST 请求。
- **紧急度**：⭐⭐ (低，按需排期)
