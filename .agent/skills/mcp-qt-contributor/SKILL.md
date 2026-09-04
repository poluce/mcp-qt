---
name: mcp-qt-contributor
description: 专门用于指导 AI 代理或新开发者为基于 Qt6 的 MCP C++ SDK (mcp-qt) 贡献代码、修复 Bug 或提交 PR 的标准操作程序 (SOP)。
---

# MCP C++ Agent Contributor Skill

## 1. 触发条件 (When to use this skill)
当用户要求“给 mcp-qt 加个功能”、“修复 SDK 的 Bug”、“重构底层传输协议”或“准备提交 PR”时，**必须强制激活本技能**并严格遵守以下所有规约。

## 2. 核心架构认知 (Architecture Mindset)
在修改任何代码前，你必须理解以下架构边界：
- `src/core/`：**纯粹的协议层**。JSON-RPC 序列化、会话、OAuth、Header 编码。严禁引入 Qt / GUI / 特定网络传输库。
- `src/transport/`：**原生传输层**。基于 `QProcess` 和 `QNetworkAccessManager`，严禁引入 `libcurl` 等第三方网络库。
- `src/client/`：**Qt 高级封装层**。信号槽、重连自愈、多服务器 Host/Router、同步+异步双 API。
- `src/apps/`：**MCP Apps 扩展**。`ui://` 协议层、AppBridge、WebView2 渲染。不要把渲染细节塞回 core。

根构建没有按层裁剪的可选开关；上述四层始终编译。

## 3. 编码铁律 (Coding Golden Rules)
- **事件循环不得在 GUI / transport 热路径上卡死**：禁止 `while(true)` 轮询，禁止在传输层或 GUI 线程随意 `waitForFinished()`。网络和进程通信必须通过 **Qt 信号与槽** 异步处理。
- **双 API 是既有设计**：`callTool()` 等同步方法用局部 `QEventLoop` 阻塞，**只允许在非 GUI 线程**使用。GUI 线程必须走 `*Async` / `QFuture` / 信号。不要把同步 API 删掉或写成“违规”。
- **锁的安全域 (Mutex Safety)**：在 Transport 层持有 `std::mutex` / `QMutex` 时，必须在调用任何可能触发 Qt 事件循环或发射信号的代码之前**释放锁**。
- **指针与闭包捕获**：Lambda 里处理网络响应时宿主可能已销毁。强制使用 `QPointer<T>` 或 `std::weak_ptr`，并在第一行做存活检查。

## 4. 测试与验证强制流程 (Mandatory Validation Workflow)
在生成代码并建议用户提交 PR 之前，引导完成以下验证（与仓库三维测试矩阵一致）：
1. **单元/集成**：构建并运行 `build/build_test/tests_qt`（源列表与目标见 `test/CMakeLists.txt`；另有 `tests_qt_https_runtime`、`tests_qt_apps_e2e`）。写法见 `test/TEST_WORKFLOW.md`。
2. **协议合规**：若改了 `src/core` 或 2026-07-28 传输/OAuth 路径，用 `conformance_runner_qt` 经官方 CLI 回归；2026-07-28 客户端套件的已记录成绩是 **32/32**（见 `README.md`）。不要引用过期的 legacy 套件通过率。
3. **端到端**：运行 `examples/multi_server_agent`，至少做一次界面「刷新/重载」，确认进程树与热插拔仍稳定。涉及 MCP Apps 时再看 WebView2 分屏。

## 5. PR 提交规范 (PR Submission Checklist)
帮用户生成 Commit Message 和 PR 描述时，必须：
- 采用 Conventional Commits（如 `feat(transport): ...`, `fix(client): ...`）。
- 在 PR 描述中勾选上述三步验证的完成状态。
- 未亲自跑通 `tests_qt` 时，不要编造新的 `N/N` 通过数。
