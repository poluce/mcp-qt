# 多 Agent × 多 MCP：Agent-Server 注册与生命周期管理 — 设计草案

> **状态**：设计说明。MVP 已在 `examples/multi_server_agent` 落地（`AgentRegistry` + `AgentReconciler`），并由 `test/test_multi_agent_registry.cpp` 覆盖并集 / 反向索引 / 对账启用。本文保留决策与边界，不构成协议/契约变更。
> 日期：2026-08-12（v3：核心从「连接复用」纠正为「Agent-Server 注册管理」）；2026-08-18 文档对齐：去掉「未实现」表述。

## 1. 核心问题（纠正后的定位）

**不是「怎么复用连接」，而是「每个 agent 注册了自己的 MCP 服务器集合，注册之间可能重叠也可能不同，我们怎么把它们管理起来」。**

- 每个 agent 声明自己需要的服务器集合（可能有重叠、可能有差异）。
- 需要一个**管理抽象**来持有这份注册表、驱动其生命周期、并对每个 agent 投影出正确的工具面。
- **连接复用是管理顺带的结果**（同一服务器被多个 agent 引用时自然只建一条连接），不是独立目标。

## 2. 管理模型

核心是一个 **Agent-Server 注册表 + 期望态对账**：

```
agentA → {serverX, serverY}
agentB → {serverY, serverZ}
```

### 2.1 注册表（Registry）—— 数据模型

持有 agent → 服务器集合 的映射，并提供：

- 增删改：`registerAgent(agent, servers)` / `unregisterAgent` / `addServerToAgent` / `removeServerFromAgent`
- 正查反查：`serversForAgent(agent)`、`agentsForServer(server)`（反向索引）、`allServers()`、`allAgents()`
- 变更通知：注册表变化时发出事件（供工具面/生命周期联动）

### 2.2 期望态对账（Reconciliation）—— 生命周期引擎

- **期望态**：所有 agent 的服务器集合的并集（每服务器带配置）。
- **实际态**：当前运行的连接。
- **对账**：注册表任何变更 → 启动「期望有而实际无」的服务器，停止「实际有而期望无」的服务器。
- **重叠自然去重**：某服务器被 N 个 agent 引用 → 只要 ≥1 个 agent 还在用就保持一条连接，最后引用移除才断开。**去重是对账的结果，不是单独的池机制**。

### 2.3 按 Agent 工具面 —— 投影

- `toolsForAgent(agent)` = 该 agent 服务器集合上工具的全集（前缀路由 `serverName_` 已支持隔离）。
- 注册表变更 → 受影响 agent 的工具面联动刷新。

## 3. 现状盘点（对管理模型的差距）

| 管理能力 | 现状 |
|---|---|
| 单 host 多服务器生命周期 | ✅ `McpServerManager` |
| 前缀路由隔离 | ✅ 三 Router |
| **agent → 服务器 的注册表** | ✅ 示例层 `AgentRegistry`（非 SDK `McpHost` 内置；单 host 列表语义未改） |
| **期望态对账（自动起停/去重）** | ✅ 示例层 `AgentReconciler` 监听注册表，驱动 `McpHost::setServerEnabled` |
| **按 agent 投影工具面** | ⚠️ 仍用现有 `serverName_` 前缀路由；按 agent 过滤由调用方按注册表自行筛 |
| **变更联动（agent 增删服务器 → 工具面刷新）** | ✅ 注册表 `changed()` → 对账刷新启用集合；工具面随 Host 既有 `globalToolsChanged` |

## 4. 组件落点（示例层，非协议）

### 4.1 Agent-Server 注册表（Registry）
已实现：`examples/multi_server_agent/AgentRegistry.h`。数据模型 + 增删改 + 反向索引 + `changed()`。纯数据层，不含连接。

### 4.2 对账管理器（Reconciler）
已实现：`examples/multi_server_agent/AgentReconciler.h`。监听注册表，按并集调用 `McpHost::setServerEnabled`。重叠服务器只保留一条期望启用。

### 4.3 按 Agent 视图（Tool View）
未做成独立 SDK 类型：用注册表的 `serversFor(agent)` 加上现有前缀路由自行投影。

### 4.4 配置来源
复用 `McpJsonConfigLoader` 的 `$ENV_VAR` 插值；注册表由示例在加载配置后写入。

## 5. 协议前提

以 **2026-07-28 无状态**为主目标：stateless 下每请求独立、双向能力已废弃，重叠服务器的共享连接无状态冲突；仅剩共享 SSE 通知流需按 agent 分发的处理（次级关注，见 §7）。

## 6. 明确不做（本轮）

- 协议层改动。
- 共享连接内的权限隔离（安全层）。
- 连接复用作为独立抽象（它是对账的结果）。

## 7. 已定决策（2026-08-12）

| # | 决策点 | 结论 |
|---|---|---|
| 1 | 管理模型 | **声明式期望态对账**：agent 声明服务器集合，管理器自动起停连接、重叠自然去重 |
| 2 | 落点 | **新组件**（`McpAgentRegistry` + Reconciler），组合现有 `McpServerManager`/router，不动现有单 host 语义 |
| 3 | 配置格式 | **单文件按 agent 分组**（agent 键下各自服务器列表，复用 `McpJsonConfigLoader` 的 `$ENV_VAR` 插值） |
| 4 | 变更联动 | **自动对账刷新**：注册表任何变更 → 自动起停连接 + 刷新受影响 agent 工具面 |

## 8. 后续待细化问题（实现 MVP 时再定）

- 同身份判定细节（key 语义）：command+args+env+headers 哪些参与判定。
- 对账的并发/幂等：多 agent 同时增删服务器时的竞态与重复对账。
- 通知分发的次级处理：共享 SSE 流如何按 agent 分发（如需要）。
- 与 `McpResourceSubscriptionRouter` 快照模式的复用关系。
