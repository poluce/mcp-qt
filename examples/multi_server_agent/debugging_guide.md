# MCP C++ Agent 调试与排错技术指南

本指南记录了在多服务器 MCP 智能体（`multi_server_agent`）开发及实机联调过程中，针对**连接卡死、主线程无响应、文件锁死锁、内存崩溃及网络代理**等深层技术问题的排错诊断过程与终极解决方案。

---

## 目录
1. [远程 Streamable HTTP 协议握手与 mcp-remote 代理](#1-远程-streamable-http-协议握手与-mcp-remote-代理)
2. [主线程 QEventLoop 同步卡死与 Tools 缓存异步防阻塞设计](#2-主线程-qeventloop-同步卡死与-tools-缓存异步防阻塞设计)
3. [Windows NTFS 高频文件 open/close 引起的文件锁自我死锁](#3-windows-ntfs-高频文件-openclose-引起的文件锁自我死锁)
4. [QString::arg 二次占位符格式化引发的内存崩溃缺陷](#4-qstringarg-二次占位符格式化引发的内存崩溃缺陷)
5. [常见环境变量与联调快捷键](#5-常见环境变量与联调快捷键)

---

## 1. 远程 Streamable HTTP 协议握手与 mcp-remote 代理

### 🕵️ 诊断发现
在对接第三方远程 MCP 搜索引擎（如 `https://api.anysearch.com/mcp`）时，直接以传统的 SSE 方式连接会报 `404 Not Found`。这是因为该服务使用的是 **Streamable HTTP 传输协议**（MCP 最新标准），必须在本地通过官方 Node.js 桥接代理（`mcp-remote`）作为本地 Stdio 桥梁。

### 🚨 匿名网络限流与本地 TUN 代理挂起
即使配置了 Stdio 桥接代理，在没有配置 API 凭证的**匿名模式**下：
*   远程云端（通常分发在 AWS CloudFront）会对匿名连接进行极严苛的速率限制，**直接挂起（Queueing / Drop）initialize 握手包**，导致本地代理一直卡在 initialize 请求阶段。
*   在国内网络环境下，即使本地开启了代理和 **TUN 模式**，如果代理的 Rules 规则没有将该特定 AI 域名收录（被判定为 `DIRECT` 直连），也会因为直连 AWS 东京/北美节点丢包而导致长连接握手死锁。

### 🛠️ 解决方案
1.  在本仓库 [`examples_config.json`](examples_config.json) 中为 `anysearch` 注入合法的 Authorization 头：
    ```json
    "anysearch": {
        "command": "npx.cmd",
        "args": [
            "-y",
            "mcp-remote",
            "https://api.anysearch.com/mcp",
            "--header",
            "Authorization: Bearer as_sk_*********************************"
        ]
    }
    ```
2.  确保本地代理客户端的规则集中，该域名已配置走代理路由。

---

## 2. 主线程 QEventLoop 同步卡死与 Tools 缓存异步防阻塞设计

### 🕵️ 诊断发现
在 Stdio 代理冷启动期间，一旦进程建立，底层的管道连通会立刻触发 `Connected = true`。这会导致主线程提前触发起跑任务进入 `beginRunAgainstCurrentClients`。
此时由于 anysearch 云端尚未完全就绪，其工具列表缓存 `cachedTools()` 必然为空，进而触发了**同步拉取 `client->fetchAllTools(m_timeoutMs)`**。

由于 `fetchAllTools` 底层通过 `QEventLoop` 挂起主线程以等待网络响应，如果 anysearch 一直因为网络原因不作答，**主线程就会被同步锁死整整 15 秒**！期间 GUI 界面完全丧失视窗消息泵送能力，表现为**界面卡死、右上角关闭按钮变红**。

### 🛠️ 解决方案
在 `AgentSession.cpp` 的归集流程中引入**针对远程服务的同步拉取豁免**：
*   **本地 Stdio 顺发保留**：本地 Stdio 服务器（如 `mock-search` 和 `playwright`）由于管道读取全在 $1\text{ ms}$ 内完成，继续保留同步 fetch 兼容。
*   **远程 AnySearch 彻底豁免**：
    ```cpp
    if (serverName.contains(QStringLiteral("anysearch"), Qt::CaseInsensitive)) {
        qInfo().noquote() << "[AgentSession] anysearch 缓存工具为空，强行跳过同步拉取以释放 Qt 主线程！";
        continue;
    }
    ```
    一旦检测到 anysearch 缓存尚未就绪，主线程物理跳过 `fetchAllTools`，直接带上就绪的本地服务器秒级起跑，主线程彻底释放。

---

## 3. Windows NTFS 高频文件 open/close 引起的文件锁自我死锁

### 🕵️ 诊断发现
Stdio 代理在 stderr 中输出大量 initialize 的 JSON 长报文时，微秒内会触发好几十次 `clientErrorOccurred` 信号，使得主线程高频重入执行日志落盘：
```cpp
// 🌟 原高频写盘逻辑
if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
    stream << logLine;
    file.close();
}
```
在 Windows NTFS 文件系统下，微秒级高频重入地对同一个文件描述符执行 `open` 和 `close` 操作，会导致前一次的 OS 文件句柄锁释放尚未完成，后一次的 `open` 挂起等待。由于这一切发生在同一个主线程中，导致**主线程与 NTFS 文件锁陷入自我死锁（Self-Deadlock）**，主线程直接无响应。

### 🛠️ 解决方案
将日志落盘重构为**全局单句柄长连接 + `std::mutex` 线程保护模型**：
*   程序启动时由 `updateGlobalLogFile` 一次性打开日志文件长连接；
*   `myMessageOutput` 日志拦截器中直接高效写入，免去任何 `open/close` 重入，并加锁保障多线程安全：
    ```cpp
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile && g_logFile->isOpen()) {
            QTextStream stream(g_logFile);
            stream << logLine;
        }
    }
    ```

---

## 4. QString::arg 二次占位符格式化引发的内存崩溃缺陷

### 🕵️ 诊断发现
在收集 Stdio 进程 stderr 报错时，我们执行了：
```cpp
QStringLiteral("Server '%1' reported error: %2").arg(serverName, error)
```
由于 `error` 是子进程吐出的**原始大 JSON 文本**，当里面包含特殊转义子串（如 `"%1"`）时，Qt 的 `arg()` 函数在处理完前一个参数的占位符后，会顺次对替换后的字符串进行二次检索。如果在替换的大文本中重新匹配到了占位符格式，在某些 Qt6 版本的边界拷贝计算中，会引发**内存段错误（Segmentation Fault）物理崩溃**，使 C++ 程序瞬间夭折退出（Exit Code 1）。

### 🛠️ 解决方案
摒弃包含二次匹配逻辑的 `.arg()`，使用 **`+` 号物理字符串直连拼接**，实现绝对的安全：
```cpp
QStringLiteral("Server '") + serverName + QStringLiteral("' reported error: ") + error
```

---

## 5. Stdio 进程 stderr 日志输出触发 auto-reconnect 重连恶性循环

### 🕵️ 诊断发现
当 `mcp-remote` 代理启动时，它在 stderr 输出了大量的正常运行和初始化 Debug 日志。
然而，在 `McpQtClient.cpp` 的底层 SDK 连接管理中，原代码将 Stdio 的所有 stderr 触发的 `setOnError` 直接与 `handleTransportFailure()`（重连崩溃恢复）绑定了：
```cpp
    m_session->setOnError([this](const std::string& err) {
        // ... 
        handleTransportFailure(); // 🌟 只要有 stderr 输出就判定为连接断开崩溃！
    });
```
这导致 Stdio 子进程输出任何正常运行日志时，客户端都会误以为其崩溃了，从而强行杀死子进程并重新拉起。新进程拉起后再次输出日志，再次被强杀拉起，从而陷入**“无限重启 $\to$ CPU 占满 $\to$ Exit Code 1 崩溃”的恶性死循环**。

### 🛠️ 解决方案
在 `McpQtClient.cpp` 中取消日常 stderr 报错对重连的直接触发。只有在 QProcess finished/errorOccurred 等底层的物理崩溃信号发生时，才去触发重连保护。日常的 stderr 仅作为日志分发。

---

## 6. 常见环境变量与联调快捷键

在调试时，大模型密钥的加载链条遵循以下优先级：
$$\text{最终API Key} = \text{输入框手动填入} \to \text{环境变量 DEEPSEEK\_API\_KEY} \to \text{环境变量 OPENAI\_API\_KEY} \to \text{报错拦截并要求用户输入}$$

*   **GUI 默认在线**：最新版的图形界面一打开即自动切入“在线 API”模式，开启全部配置框输入，并回显检测到的密钥。
*   **清理残留进程**：如果编译时报 `Permission denied` 无法覆盖可执行文件，代表后台有残留卡死的 GUI 任务在占用文件，在 PowerShell 中执行以下命令可一键清理：
    ```powershell
    Get-Process multi_server_agent -ErrorAction SilentlyContinue | Stop-Process -Force
    ```
