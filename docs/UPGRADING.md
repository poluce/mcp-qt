# 升级指南（v0.1.x → v0.2.x）

v0.2.0 是架构级重构（线程收编、stateless 抽取、API 收敛）。对 **McpQtClient / McpHost 用户**几乎无感（内部自动适配）；对**直接使用 core 层（McpClientSession）或 transport 层**的用户有编译与行为变化。按影响面分三档。

## 一、编译会挂的（必须改）

### 1. stateless 方法从 McpClientSession 移到 McpStatelessSession

以下方法不再存在于 `McpClientSession`，直接调用会编译失败：

- `discoverServer`
- `getTask` / `updateTask` / `cancelTask`（含 `*Sync` 变体）
- `listenSubscriptions` / `cancelSubscription` / `setSubscriptionListener`
- `setMrtrHandler`
- `setLogLevel` / `getLogLevel`

**改法**：stateless 场景改用 `mcp::McpStatelessSession`（`#include <mcp_core/McpStatelessSession.h>`），构造即无状态模式、免握手：

```cpp
// 旧
auto session = std::make_shared<mcp::McpClientSession>(transport);
session->setStatelessMode(true);
session->discoverServer(cb);

// 新
auto session = std::make_shared<mcp::McpStatelessSession>(transport);
session->discoverServer(cb);
```

经 `McpQtClient` 使用这些能力的用户无需改动（客户端在 stateless 模式下自动创建并使用 `McpStatelessSession`）。

### 2. getLastRequestId() 已删除

`McpClientSession::getLastRequestId()` 随精确取消重构删除。**改法**：27 个请求方法（`listTools`/`callTool`/`getPrompt` 等）现在返回 `int64_t` 请求 id，直接用它：

```cpp
// 旧
session->listTools(cb);
int64_t id = session->getLastRequestId();

// 新
int64_t id = session->listTools(cb);
```

返回值可忽略（源码兼容），`cancelRequest(id)` 按精确 id 取消。

## 二、行为会变的（不挂但要注意）

### 1. McpServerView 调用层硬隔离

`McpServerView` 的 `callToolAsync` / `getPrompt` / `getPromptAsync` / `readResource` / `readResourceAsync` 现在**校验前缀归属**：目标服务器不在 `setVisibleServers` 列表时直接拒绝（返回 error，不发请求）。旧版本只过滤导出、调用层透传。

**影响**：多 Agent 场景下，代码绕过视图或 LLM 幻觉编造工具名会被拒。空列表 = 全部可见的语义不变。依赖旧透传行为的调用方需要把目标服务器加进可见列表。

### 2. session 单线程契约

`McpClientSession` / `McpStatelessSession` 是无锁单线程状态机：**所有方法调用与回调必须发生在同一线程**（client 所在线程）。旧版本有 mutex 部分保护，多线程并发调用"碰巧能用"；新版本违反契约是未定义行为。

**影响**：从多个线程直接调用同一个 session 的代码需要收敛到单线程（或经 `McpQtClient`，它负责线程投递）。

### 3. 同步 API 弃用

全部同步方法（`callTool`/`listTools`/`getPrompt`/`readResource`/`ping`/`complete`/`discoverServer`/`getTask` 等）标记弃用，计划 2.0 移除。行为不变，但新代码应改用对应的 `*Async` 方法。过渡期约束不变：仅限脚本/测试，禁止 GUI 线程，禁止嵌套。

### 4. transport 回调线程统一

三个 transport 的网络 I/O 全部迁到共享 I/O 线程（`McpIoContext`），回调经 queued 连接投递回 transport 所在线程。对 `McpQtClient` 用户无感；直接使用 transport 的用户注意：回调不再在调用线程同步触发，且**不要在回调里做耗时操作**（会阻塞共享 I/O 线程）。

## 三、新增能力（可选迁移）

- **McpStatelessSession**：stateless 协议族独立类，未来协议版本只演进它；
- **McpIoContext**：共享 I/O 线程，`McpQtClientBuilder` 可注入自定义实例（测试隔离）；
- **McpConfigStore**：配置持久化（从 McpHost 抽出）；
- **McpNamespace**：`serverName_` / `mcp-{serverName}-` 前缀解析唯一实现；
- **精确取消**：请求方法返回 id，`cancelRequest(id)` 精确取消。

详见 [API_REFERENCE.md](API_REFERENCE.md)。

## 四、构建环境注意

- CI 用 Qt 6.10.3（6.11.0 官方在线仓库缺 aqt 索引；6.9.x 的 `setRawHeader` 小写化 header 名，测试断言大小写敏感）。本地 6.11.0 与 CI 6.10.3 并存是有意为之；
- 构建/测试在 Windows PowerShell 侧执行（见 AGENTS.md 第 1 节）。
