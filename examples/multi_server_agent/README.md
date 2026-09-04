# Multi-Server Agent Example

这是一个基于 `mcp-qt` SDK 构建的高阶桌面级图形界面（GUI）示例应用。它完美展示了如何在一个 C++ 应用程序中**同时管理、挂载并调用多个跨语言的 MCP（Model Context Protocol）服务器**。

## ✨ 核心特性

1. **跨语言生态融合 (Language-Agnostic)**
   得益于基于 `Stdio` 和 `SSE` 的进程间通信，本示例展示了如何同时拉起并调用：
   - **Node.js** 编写的服务器（通过 `npx` 启动，如 `@modelcontextprotocol/server-memory`）
   - **Python** 编写的服务器（通过 `python` 或 `uvx` 启动，如 `mcp-server-fetch`）
   - **原生可执行程序**（如自定义的 C++ / Go / Rust 服务器）

2. **多协议混合支持 (Mixed Transports)**
   - 支持本地进程流式通信（`Stdio`），用于挂载本地的系统级工具（例如文件系统、浏览器操作）。
   - 支持远程 Server-Sent Events（`SSE`），用于无缝接入云端的 MCP 服务（例如 Anysearch API）。

3. **毫秒级热重载 (Hot-Reloading)**
   应用内置了热重载机制。你可以随时在外部编辑器中修改 `examples_config.json`，然后点击界面上的 **“刷新/重载”** 按钮。底层 `McpHost` 将会安全地关闭所有旧进程，并极速拉起全新的服务器集群，全程无死锁、无需重启应用。

4. **工具聚合与 Agent 大脑**
   提供了一个纯 C++ 实现的 ReAct Agent Loop。它可以自动枚举并合并来自 8 个不同服务器的 60+ 个工具，将其喂给 LLM。LLM 可以自主决策并在多个服务器的工具中反复穿梭执行任务（例如：先用 `anysearch` 查新闻，再用 `playwright` 操控浏览器验证，最后用 `filesystem` 保存结果）。

5. **MCP Apps 交互界面渲染**
   工具返回 MCP Apps（`text/html;profile=mcp-app`）内容时，界面右侧分屏内嵌 **WebView2** 渲染交互 UI（AppBridge 双向消息：App 可回调工具、宿主可推送数据）。Agent 调用到 MCP Apps 工具即可看到仪表盘/图表等交互界面，而非纯文本。

6. **多 Agent 注册与对账**
   `AgentRegistry` 记录 agent → 服务器集合；`AgentReconciler` 按并集启用 `McpHost` 上的服务器（重叠自然去重）。设计说明见 [`docs/multi_agent_mcp_设计草案.md`](../../docs/multi_agent_mcp_设计草案.md)。

## 🚀 内置 MCP 服务器列表

目前的 `examples_config.json` 默认集成了以下业界标杆级的 MCP 服务器：

- 🔍 **Anysearch**: 通用互联网搜索与深度垂直搜索 (SSE 模式)
- 📚 **Context7**: 编程框架官方文档查询 (`@upstash/context7-mcp`)
- 🌐 **Fetch**: HTTP 网页智能抓取与转换 (`mcp-server-fetch` Python 原生)
- 🧠 **Memory**: 持久化知识图谱存储 (`@modelcontextprotocol/server-memory`)
- 🤖 **Playwright**: 自动化浏览器控制 (`@playwright/mcp`)
- 📁 **Filesystem**: 宿主机本地文件读写 (`@modelcontextprotocol/server-filesystem`)
- 🧪 **Everything**: 官方协议全特性测试场 (`@modelcontextprotocol/server-everything`)
- 📝 **Mock Search**: 本地 Node.js 模拟搜索数据源

## 🛠️ 如何运行

### 环境准备

因为本示例调用了来自全网生态的各类 MCP 服务器，建议在运行前准备好以下环境：
1. **Node.js**: 确保终端中可用 `npx` 命令。
2. **Python**: 确保终端中可用 `python` 和 `pip` 命令，并执行以下命令安装 Fetch 服务器：
   ```bash
   pip install mcp-server-fetch
   ```

### 启动流程

1. 通过 CMake 编译整个工程，进入 `build/examples/multi_server_agent/` 目录。
2. （可选，体验 MCP Apps 渲染）先启动本地 MCP Apps mock 服务器：
   ```bash
   node test/mcp_apps_server.js   # 默认 http://127.0.0.1:9123/mcp
   ```
3. 运行 `multi_server_agent.exe`。
4. 界面会自动加载项目目录下的 `examples_config.json` 并在后台拉起所有进程（需要等待数秒，取决于网络下载包的速度）。
5. （可选）如果你修改了 JSON 配置文件，随时点击 **“刷新/重载”** 按钮更新状态。
6. 在界面右侧输入框填入你的 OpenAI 兼容 API Key 和 Base URL。
7. 点击 **“⚡ 启动 Agent 任务”**，在弹窗中告诉 AI 你的需求（比如：“调用 show_dashboard 展示数据看板”）。
8. 欣赏全自动的 ReAct 执行流！若工具返回 MCP Apps 界面，右侧分屏会渲染出可交互的 App。

## 📄 配置文件格式 (`examples_config.json`)

你可以参照以下格式轻松地将自己的 MCP 服务加入到系统中：

```json
{
    "mcpServers": {
        "my-python-tool": {
            "command": "python",
            "args": ["-m", "my_module"]
        },
        "my-node-tool": {
            "command": "npx.cmd",
            "args": ["-y", "some-node-mcp-server"]
        },
        "my-sse-service": {
            "type": "sse",
            "url": "https://api.example.com/mcp",
            "headers": {
                "Authorization": "Bearer YOUR_TOKEN"
            }
        }
    }
}
```
