# Agent 工作教训（agents.md）

本文件记录在本仓库开发中踩过的坑与工作约定，供后续 agent 会话参考。

## 1. 测试必须用 Windows 环境（PowerShell），不要用 WSL 跑 Windows exe

**教训**：本仓库是 Windows 主平台（Qt6 + MinGW）。在 WSL 里跑 Windows exe（`tests_qt.exe` 等）会遇到大量互操作假象：

- **输出缓冲**：Windows exe 的 stdout 重定向到文件是全缓冲，进程被 `timeout` 杀掉时缓冲丢失 → 表现为"0 输出 + 卡住"，实际代码正常；
- **进程管理**：WSL 的 `ps`/`kill` 看到的是 `/init` 包装进程，Windows 侧真实进程要用 `taskkill /F /IM xxx.exe` 清理（exe 文件被占用会导致链接 `Permission denied`）；
- **环境变量**：WSL 环境变量不传给 Windows 进程（即使 WSLENV），需要包装脚本转 argv（见 `conformance_runner_qt/run_conformance_wsl.sh`）；
- **代理**：Windows 系统代理（如 127.0.0.1:7890）会被 Qt 自动检测，本地探测请求必须显式 `QNetworkAccessManager::setProxy(QNetworkProxy::NoProxy)`，否则代理对 localhost 挂起导致请求永不完成。

**约定**：构建与测试统一用 PowerShell 在 Windows 侧执行：

```powershell
$env:PATH = 'E:\Qt6\6.11.0\mingw_64\bin;' + $env:PATH
& 'E:\CodeSoftware\CMake\bin\cmake.exe' --build 'F:\B_My_Document\GitHub\mcp-qt\build' --target tests_qt -j 8
& 'F:\B_My_Document\GitHub\mcp-qt\build\build_test\tests_qt.exe'
```

WSL 侧只做源码编辑/查看/git 操作。

## 2. `git add -A` 会带入运行时产物

**教训**：`git add -A` 会把 untracked 的运行时产物（如示例运行生成的 `multi_server_agent.log`）带进提交。提交前先 `git status --short` 检查，运行时产物应加入 `.gitignore`。

## 3. 协议自动探测（2026-07-28 vs 旧协议）

- `http`/`sse` 类型且未显式 `protocolVersion` 时，`McpServerManager` 会 POST `server/discover`（2026-07-28）试探服务器：支持则自动切 `stateless_http`，否则回退旧协议（2025-11-25）；
- 探测是异步的——依赖探测结果的测试要轮询等待（如 `test_qt_server_manager_router` 的 clientB）；
- 显式指定 `protocolVersion` 跳过探测；`type` 与 `protocolVersion` 矛盾时启动警告。

## 4. nlohmann::json 花括号初始化陷阱

**教训**：`nlohmann::json x{nlohmann::json::object()};` 走的是 initializer_list 构造路径，实际得到 `[{}]` 数组而不是空对象（json 可迭代，花括号初始化把它当容器）。后果：`x.is_null()` 恒为 false，依赖 null 判断的代码（如 conformance runner 的 context 环境变量解析）永远不执行。这是 nlohmann 已知问题（issue #1594）。

**约定**：json 成员默认值用 `nlohmann::json x;`（null）或 `= nlohmann::json::object()`（赋值而非花括号）。验证方法：`x.type_name()` 应为 "object" 而不是 "array"。

## 5. CI 踩坑经验

Qt/MinGW Windows CI 的七个环境差异（Qt 版本选择、工具链 ABI、header 大小写等）已沉淀为 skill：`.agents/skills/qt-ci-troubleshooting/SKILL.md`。CI 失败或本地/CI 行为不一致时先加载该 skill 对照排查。

## 6. 其他

- 单元测试通过数：`tests_qt` 当前 72/72（2026-09-08 快照），更新前必须实际跑通；
- 中文注释用 UTF-8 直接写汉字，不用 `\uXXXX` 转义；
- 不擅自 `git add`/`git reset`/`git rm --cached`，除非用户明确要求。
