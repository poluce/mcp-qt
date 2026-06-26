# mcp-cpp-agent

纯 C++17 实现的 **Model Context Protocol (MCP) 客户端 SDK**。

零外部框架依赖，通过 CMake FetchContent 一行集成，即可连接任意 MCP 服务端完成 initialize 握手、Tool/Resource/Prompt 调用等全部协议操作。

---

## 快速集成

在你的 `CMakeLists.txt` 中：

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp_sdk
    GIT_REPOSITORY https://github.com/yourname/mcp-cpp-agent.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(mcp_sdk)

target_link_libraries(your_app PRIVATE mcp::mcp_core)
```

---

## 极简示例

```cpp
#include <mcp_core/mcp_core.h>
#include <iostream>

int main() {
    // 以 Stdio 方式启动 MCP 服务端子进程
    auto transport = std::make_shared<mcp::SubprocessStdioTransport>(
        "node", {"/path/to/mcp-server.js"});

    auto session = std::make_shared<mcp::McpClientSession>(transport);
    session->init();
    session->start();

    // 同步阻塞 API：初始化握手
    bool ok = session->initializeSync("my-app", "1.0.0");
    if (!ok) { std::cerr << "握手失败\n"; return 1; }

    // 同步阻塞 API：列举工具
    auto tools = session->listToolsSync();
    for (const auto& t : tools) {
        std::cout << "Tool: " << t.name << " — " << t.description << "\n";
    }

    // 同步阻塞 API：调用工具
    mcp::json args = {{"path", "/tmp/test.txt"}};
    auto result = session->callToolSync("read_file", args);
    std::cout << "Result: " << result.dump(2) << "\n";

    session->shutdownSync();
    return 0;
}
```

---

## API 概览

### 核心类

| 类 | 说明 |
|---|---|
| `McpClientSession` | MCP 客户端会话管理器，跟踪请求、分发消息 |
| `IMcpTransport` | 传输层纯虚接口，可自行扩展 |
| `SubprocessStdioTransport` | 纯 C++ 子进程 Stdio 传输（内置） |
| `ConsoleStdioTransport` | 从当前进程 stdin/stdout 读写（用于 conformance 测试） |
| `McpTool` | 工具元数据结构体（name, description, inputSchema） |

### API 风格

`McpClientSession` 提供三套 API 风格，覆盖不同场景：

**1. 异步回调** — 适合事件循环驱动的应用
```cpp
session->listTools([](const std::vector<mcp::McpTool>& tools, const mcp::json& err) {
    // 回调处理
});
```

**2. 同步阻塞** — 适合脚本或简单场景
```cpp
auto tools = session->listToolsSync(timeout);
auto result = session->callToolSync("tool_name", args);
```

**3. Raw 字符串** — 解耦 nlohmann/json，适合跨语言绑定
```cpp
session->callToolRaw("tool_name", R"({"key":"value"})", 
    [](const std::string& result, const std::string& error) { ... });
```

### 协议操作

| 操作 | 异步 | 同步 |
|------|------|------|
| 初始化握手 | `initialize()` | `initializeSync()` |
| 安全关闭 | `shutdown()` | `shutdownSync()` |
| 列举工具 | `listTools()` | `listToolsSync()` |
| 调用工具 | `callTool()` | `callToolSync()` |
| 列举资源 | `listResources()` | `listResourcesSync()` |
| 读取资源 | `readResource()` | `readResourceSync()` |
| 列举提示词 | `listPrompts()` | `listPromptsSync()` |
| 获取提示词 | `getPrompt()` | `getPromptSync()` |

---

## 自定义传输层

实现 `IMcpTransport` 接口即可接入任意传输协议（WebSocket、gRPC、共享内存等）：

```cpp
class MyTransport : public mcp::IMcpTransport {
public:
    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    bool start() override;
    void close() override;
};
```

---

## 构建选项

| CMake 选项 | 默认值 | 说明 |
|------------|--------|------|
| `MCP_ENABLE_QT` | `OFF` | 启用 Qt6 传输扩展和调试器示例 |

```bash
# 仅构建 SDK（零外部依赖）
cmake -B build

# 启用 Qt 扩展 + 调试器示例
cmake -B build -DMCP_ENABLE_QT=ON -DCMAKE_PREFIX_PATH="/path/to/qt6"
```

---

## 项目结构

```
mcp-cpp-agent/
 ├── mcp_core/                 # SDK 核心库（纯 C++17）
 │    ├── include/mcp_core/
 │    │    ├── mcp_core.h               # 一站式头文件
 │    │    ├── McpClientSession.h        # 会话管理
 │    │    ├── IMcpTransport.h           # 传输层接口
 │    │    ├── SubprocessStdioTransport.h# 子进程 Stdio 传输
 │    │    ├── McpTool.h                 # 工具数据结构
 │    │    ├── McpMessage.h              # JSON-RPC 消息结构
 │    │    └── JsonRpcDispatcher.h       # 协议分发器
 │    └── src/
 │
 ├── extensions/qt/            # Qt6 可选扩展
 │    ├── include/mcp_qt/               # QtStdioTransport, QtHttpTransport, QtMcpClient
 │    └── src/
 │
 ├── examples/
 │    └── debugger/            # Qt GUI 调试面板示例
 │
 ├── tests/                    # 测试用例
 ├── conformance_runner/       # 协议一致性测试
 └── CMakeLists.txt
```

---

## 依赖

- **SDK 核心**：C++17 编译器 + nlohmann/json（自动 FetchContent）
- **Qt 扩展**（可选）：Qt6 Core & Network
- **调试器示例**（可选）：Qt6 Widgets
