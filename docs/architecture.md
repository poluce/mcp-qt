# mcp-qt 终态架构（Target Architecture）

> 本文档定义本 SDK 的**目标架构**。所有改动必须朝终态走：与终态一致的改动直接做，与终态冲突的改动不做，临时绕路必须在此登记并注明拆除条件。
>
> 现状与终态的差距见文末「差距清单」。迁移按「路线图」分步执行，每步有验收标准。

## 一、设计原则

1. **单线程状态机优先**：协议核心是无锁单线程状态机，线程边界显式、可验证，不靠锁推理正确性。
2. **演进核心与冻结代码分离**：会继续演进的路径（stateless 协议族）独立成类；冻结路径（legacy 握手）封进冻结模块，只修 bug 不扩展。
3. **一条规范路径**：对外 API 只有一条推荐用法，新功能只加在规范路径上。
4. **安全默认拒绝**：多 Agent 视图的调用层校验归属，默认拒绝越界调用。
5. **测试是发布门槛**：tests_qt 全绿 + conformance 32/32 是任何合并的前置条件。

## 二、线程模型（终态）

### 2.1 共享 I/O 线程

- 进程内一条共享 I/O 线程（McpIoContext：QObject + QThread，默认共享实例，McpQtClientBuilder 可注入自定义实例）。
- 所有 transport 的 I/O 都在这条线程上：QNAM 请求、SSE 流、QProcess 子进程。QNAM/QProcess 必须在 I/O 线程内创建，禁止构造后 moveToThread。
- 单线程承载多连接是安全的：QNAM/QProcess 都是异步的，I/O 线程只做事件派发。

### 2.2 无锁 session

- McpClientSession 是无锁单线程状态机：所有方法调用与回调触发都发生在 client 所在线程（transport 回调经 queued 连接投递回该线程）。已删除全部 mutex（46 处 lock_guard）。
- 唯一例外：通知去重定时器在后台线程运行，只访问 m_debounceStates（m_debounceMutex 保护）与 transport->send（线程安全）。
- 回调契约：session 回调固定触发于 client 线程；**用户回调不得阻塞该线程**，耗时操作必须自行投递到其它线程。
- 禁止从 I/O 线程回调路径发起 BlockingQueuedConnection（同线程死锁）；transport 的 close()/send() 必须带同线程守卫。

### 2.3 marshal 边界

- transport 是 marshal 边界：网络结果经 queued 连接投递回 transport 所在线程（client 线程）；McpQtClient 对外信号/回调经 QMetaObject::invokeMethod 切到 context 线程。
- 同步 API：2.0 移除。过渡期维持局部事件循环实现（queued 回调由该循环处理），按精确请求 id 取消；文档明确「仅限脚本/测试，禁止 GUI 线程，禁止嵌套」。

## 三、协议层（终态）

- **stateless 路径独立成类**（2026-07-28+）：版本协商、每请求 _meta 帧封装、resultType 处理、MRTR、Tasks 都长在这里。未来协议版本只改这个类。
- **legacy 握手冻结**（2025-03-26 / 2025-06-18 / 2025-11-25）：封进冻结模块，只修 bug，不扩展，不新增协议版本。
- 版本协商表驱动：SUPPORTED_PROTOCOL_VERSIONS 表 + 每版本一个策略入口，不散落 if/else。

## 四、API 面（终态）

- **规范路径**：McpHost → McpServerManager → McpQtClient。新功能只加在这条路径上。
- **同步 API**：deprecated → 2.0 移除。过渡期文档明确「仅限脚本/测试，禁止 GUI 线程，禁止嵌套」。
- **视图硬隔离**：McpServerView 的 callToolAsync/getPromptAsync/readResourceAsync 校验 serverName_ / mcp-{server}- 前缀归属，不在可见列表直接拒绝并返回强类型错误。空列表 = 全部可见语义保留。
- **前缀解析收敛**：serverName_ / mcp-{server}- 前缀的解析/剥离收敛为单一工具函数，view 与三个 router 共用，禁止各自实现。

## 五、基础设施（终态）

- **CI**：GitHub Actions——Windows 构建 + tests_qt + e2e（job 内起 node mock 服务器跑 tests_qt_apps_e2e）。
- **Pimpl 统一**：三个 transport 公开头全部 Pimpl，不暴露 QNAM/QProcess 等实现细节。
- **conformance 32/32** 作为发布门槛，协议层改动必须复测。

## 六、路线图（每步验收）

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | 本文档 | 评审通过 |
| 2 | 便宜修复：视图硬隔离、精确取消、close 同线程守卫 | ✅ 71/71 + 新增隔离/取消测试 |
| 3 | 线程收编：McpIoContext + 按 transport 迁移（stateless → stdio → SSE 统一）+ session 无锁化 | ✅ 72/72 + 线程契约测试 + conformance draft 173/173 + auth 238/238 |
| 4 | stateless 抽取 + legacy 冻结 | ✅ 72/72 + conformance draft 173/173 + auth 238/238 |
| 5 | API 收敛：McpConfigStore 抽取、deprecated 标记、前缀解析收敛 | ✅ 72/72 |
| 6 | 基础设施：CI（.github/workflows/ci.yml）、Pimpl 统一（worker 模式）、e2e 验证 | ✅ 72/72 + e2e 1/1 + conformance 双套件全绿 |

## 七、明确不做

- **不做重入守卫**：线程收编（第 3 步）根治重入问题，守卫是会被拆除的临时代码。
- **不做每 client 一线程**：共享 I/O 线程扩展性更好（10 服务器 = 1 线程，不是 10 线程）。
- **不为 legacy 建策略抽象框架**：冻结代码不配抽象层，只抽会演进的 stateless 路径。

## 八、差距清单（现状 → 终态）

| 项 | 现状 | 终态 |
|---|---|---|
| session 线程模型 | ✅ 无锁单线程状态机（client 线程），去重定时器为唯一例外 | 同左 |
| transport 线程 | ✅ 全部在共享 I/O 线程，回调 queued 回 client 线程 | 同左 |
| 同步 API | ✅ 局部事件循环 + 精确 id 取消 | 2.0 移除 |
| 协议版本 | ✅ McpStatelessSession 独立类（discover/MRTR/Tasks/subscriptions/_meta）+ 基类 legacy 冻结 | 同左 |
| 视图隔离 | ✅ 调用层硬隔离 | 同左 |
| 前缀解析 | ✅ McpNamespace 单一实现，3 router 委托 | 同左 |
| McpHost | ✅ 薄 facade：持久化抽到 McpConfigStore，规范路径写入类文档 | 同左 |
| Pimpl | ✅ 三个 transport 公开头均不暴露实现（worker 模式） | 同左 |
| CI | ✅ GitHub Actions：构建 + tests_qt + e2e + conformance 双套件 | 同左 |
