# mcp-qt 开发者接手指南（Handover Guide）

> 面向后来接手者的完整流程文档。
> 覆盖：项目概览、环境搭建、构建测试、架构导航、已完成能力、未完成项清单、接手路线图。
> 更新日期：2026-08-08 ｜ 分支：feat/protocol-2026-07-28

---

## 1. 项目概览

**mcp-qt** 是基于 Qt6 纯净实现的 **Model Context Protocol (MCP) 客户端 SDK**（C++17，零外部网络库）。

当前分支已完成：

| 领域 | 状态 |
|------|------|
| MCP 2026-07-28 核心协议（stateless/MRTR/Header 路由/OAuth 硬化） | ✅ 官方 conformance 32/32 全绿 |
| 旧协议（2025-11-25 等）向后兼容 | ✅ legacy 路径保留，场景按 spec-version 路由 |
| MCP Apps（io.modelcontextprotocol/ui 扩展） | ✅ 协议层 + WebView2 渲染 + postMessage 互通（端到端验证通过） |

## 2. 技术栈与环境

### 2.1 依赖

| 依赖 | 版本/来源 | 用途 |
|------|----------|------|
| Qt6 | 6.11.1（Core/Network/Test；MCP Apps 需 Widgets/Gui） | 基础 |
| 编译器 | MinGW-w64 13.1 / MSVC 2019+ / GCC 8+ | 编译 |
| CMake | ≥ 3.16 | 构建 |
| nlohmann/json | 3.11.3（FetchContent） | JSON |
| WebView2 SDK | 1.0.4129.50（`third_party/webview2`） | MCP Apps 渲染 |
| wil | `third_party/wil` | WebView2 兼容头 |

### 2.2 开发机工具链（Windows）

```powershell
# MinGW 工具链（当前默认）
Qt6_DIR = E:/Qt6/6.11.1/mingw_64
CMAKE_CXX_COMPILER = E:/Qt6/Tools/mingw1310_64/bin/g++.exe
CMAKE_MAKE_PROGRAM = E:/Qt6/Tools/Ninja/ninja.exe

# MSVC 工具链（备选，MCP Apps/WebView2 静态链接时用）
VS 2026 + Qt MSVC 包（E:/Qt6/6.11.1/msvc2022_64，aqt 手动安装）
```

### 2.3 构建

```bash
# MinGW
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=E:/Qt6/6.11.1/mingw_64 \
  -DCMAKE_CXX_COMPILER=E:/Qt6/Tools/mingw1310_64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=E:/Qt6/Tools/Ninja/ninja.exe
cmake --build build
```

### 2.4 测试

```bash
# 单元/集成测试
./build/build_test/tests_qt.exe            # 27/27 通过

# 2026-07-28 官方 conformance（32 场景）
npx -y @modelcontextprotocol/conformance@0.2.0-alpha.10 client \
  --command "build\conformance_runner_qt\mcp_client_conformance_qt.exe" \
  --scenario request-metadata --spec-version 2026-07-28

# MCP Apps 演示
./build/examples/mcp_app_demo/mcp_app_demo
```

## 3. 代码架构导航

```
src/
 ├── core/        # 纯 C++ 核心：McpClientSession（会话）、McpOAuthClient（认证）、
 │                #   McpHeaderEncoding（Header 编码）、传输抽象 IMcpTransport
 ├── transport/   # Qt 传输：QtStatelessHttpTransport（2026-07-28）、
 │                #   QtHttpSseTransport（legacy）、QtProcessStdioTransport
 ├── client/      # Qt 高层：McpQtClient（QObject 封装）、McpHost（多服务器外观）、
 │                #   McpServerManager、路由/Model/配置加载
 └── apps/        # MCP Apps 扩展：McpAppSupport（协议层）、IMcpAppRenderer（抽象）、
                  #   McpAppWebView2Renderer（WebView2 后端）
```

**关键设计决策**（接手前必读）：

1. **新旧双协议路径**：`McpClientSession` 同时支持 stateless（2026-07-28，免握手）和 legacy（initialize 握手）。`setStatelessMode(true)` 切换；`SUPPORTED_PROTOCOL_VERSIONS` 列表含 4 个版本。
2. **conformance 场景路由**：`conformance_runner_qt/ScenarioRegistry.cpp` 按场景名注册；`runToolsCall2026` / `_raQt` 根据 `config.protocolVersion` 自动选择 stateless/legacy。
3. **MCP Apps 渲染可插拔**：`IMcpAppRenderer` 抽象接口，当前实现 WebView2 后端；未来可加 QCefView/QtWebEngine。
4. **WebView2 关键坑**（已踩过）：
   - MinGW 不能链接 MSVC 的 `WebView2LoaderStatic.lib`（CRT 符号不匹配）→ **必须动态加载** `WebView2Loader.dll`（`LoadLibrary + GetProcAddress`）。
   - COM 回调接口在 MinGW 下手写 vtable，**引用计数必须正确**：raw 指针赋值后要显式 `AddRef()`，否则 WebView2 回调返回后对象被释放 → 后续操作静默失败（这是此前"环境问题"误判的根因）。
   - `CreateCoreWebView2EnvironmentWithOptions` 的 `userDataFolder` 建议用绝对路径。
   - **QWidget 渲染器不要用 `std::shared_ptr` 持有**：一旦 `layout->addWidget(renderer)` reparent 到 Qt，Qt 父对象与 shared_ptr 形成双所有权，程序退出时二次 `delete` 崩溃（`0xC0000409`）。裸指针 + Qt 父子管理即可。
   - **并发/多次实例必须指定独立 `userDataFolder`**：共享默认目录会锁冲突，`CreateCoreWebView2Controller` 报 `0x800700AA ERROR_NO_SYSTEM_RESOURCES`。崩溃残留的孤儿 `msedgewebview2` 进程也会累积占资源，需清理。
   - **Chromium 解析陷阱**：`<script>` 块内出现字面量 `<script`/`</script`（即使在 JS 字符串里）会进入 double-escaped 解析状态使整个脚本失效。JS 里表示 `<` 要用 `\x3C` 转义；也不要裸写 `'</script>'` 字符串。

## 4. 已完成能力（2026-07-28）

- **Stateless 核心**：免握手、每请求 `_meta`（protocolVersion/clientCapabilities）、版本协商重试
- **server/discover**：异步/同步 API
- **MRTR**：input_required 拦截、requestState 原样回显、新 JSON-RPC id、InputResponses 顶层回传
- **Header 路由**：Mcp-Method/Mcp-Name 三路 + Base64 sentinel 编码（SEP-2243）
- **x-mcp-header**：Mcp-Param-{Name} 镜像 + 约束校验 + 非法工具剔除
- **CacheableResult**：listTools/listResources 等 WithCache 变体（ttlMs/cacheScope）
- **subscriptions/listen**：长连接通知流 + acknowledged + subscriptionId 派发
- **OAuth 硬化**：RFC 9207 iss 校验、DCR application_type、CIMD URL-based client_id、issuer 凭据绑定、scope 合并
- **错误码分区**：-32900 系列本地码；-32020/-32021/-32022 规范码
- **MCP Apps**：能力声明、ui:// 资源获取、WebView2 渲染、JS⇄C++ postMessage 互通

## 5. 未完成项清单（接手重点）

### 5.1 MCP 2026-07-28 协议未完成

| 编号 | 项目 | 说明 | 建议方向 |
|:---:|------|------|---------|
| C1 | **Tasks 扩展**（io.modelcontextprotocol/tasks，SEP-2663） | tasks/get、tasks/update、subscriptions 通知 | 实现扩展模块，参考 ext-apps 模式 |
| C2 | **EMA 扩展**（Enterprise Managed Authorization） | ID-JAG（RFC 8693 token exchange）+ JWT-bearer 换 MCP token | 需要 RFC 7523 JWT 签名支持 |
| C3 | **DPoP**（RFC 9449） | 访问令牌绑定密钥，请求附 proof JWT | 需要 JWT 签名 + WebView2 无关 |
| C4 | **WIF**（Workload Identity Federation） | 工作负载用平台 JWT 认证 | 依赖 C1 的 JWT 基础设施 |
| C5 | **服务端角色** | 本项目是客户端 SDK，未实现 2026-07-28 服务端 | 全新领域，评估是否有必要 |
| C6 | **MRTR sampling/roots 路径专项验证** | MRTR handler 可处理 sampling/createMessage，但 conformance 只验证了 elicitation | 写测试覆盖 sampling/roots 的 inputRequests |
| C7 | **per-request logLevel** | ✅ 已实现：`_meta.io.modelcontextprotocol/logLevel` 注入（`McpClientSession::setLogLevel` + `McpQtClient::setRequestLogLevel`），`setLoggingLevel` 在 2026-07-28 下自动走新路径 | — |
| C8 | **OpenTelemetry trace context** | ✅ 已实现：W3C trace context（traceparent/tracestate/baggage）经 `McpQtClient::setTraceContext` / Builder `setTraceContext` 注入每个 HTTP 请求 | — |
| C9 | **JWT-Bearer grant** | ✅ 已实现（2026-08-08）：`private_key_pem` + ES256（P-256）生成 RFC 7523 client assertion（`src/client/src/es256jwt.cpp`，Windows 走系统 BCrypt / 非 Windows 走 OpenSSL；自含最小 P-256 点运算从 d 恢复公钥）。**已验证通过 `auth/client-credentials-jwt` conformance（2026-08-11，8/8）**，期间修复 JWT `aud` 应为授权服务器 issuer 而非 token endpoint。**剩余：RS256 及 ECDSA 其它曲线（P-384/P-521）未实现** | 解锁 C2 的 JWT 前提 / C4 / C10；RS256 需引入 RSA 签名后端 |
| C10 | **`--suite all` 全量验证** | 只跑了 2026-07-28 过滤的 32 场景；extension 属性场景（dpop/wif-jwt/enterprise-managed-authorization）未跑 | 跑全量 + 补齐 extension handler |

### 5.2 MCP Apps 未完成

| 编号 | 项目 | 状态 | 建议方向 |
|:---:|------|------|---------|
| A1 | **AppBridge 工具调用代理** | ✅ 已实现：`McpAppBridge`（tools/call、tools/list、resources/read 代理 + 响应回传） | — |
| A2 | **权限策略细化** | ✅ 已实现：`McpAppBridge::setPermissionPolicy`（allowedTools/allowedCapabilities 校验 + `_meta.ui.visibility` 可见性过滤） | — |
| A3 | **更多 ui/ 方言方法** | ✅ 已实现：ui/open-link、ui/message、ui/update-model-context、ui/request-display-mode（可自定义 handler） | — |
| A4 | **ui:// 真实服务器端到端** | ✅ 已验证（2026-08-11）：官方 `@modelcontextprotocol/server-threejs` 联调通过（`tests_qt_apps_e2e` 设 `MCP_APPS_E2E_URL` 连外部服务器，ui:// 资源走 `resources/read`） | — |
| A5 | **QCefView 后端** | ⛔ 暂不实现（2026-08-11 用户决定，不纳入范围） | `IMcpAppRenderer` 抽象已预留 |
| A6 | **跨平台渲染** | ⛔ 暂不实现（2026-08-11 用户决定，不纳入范围） | WebView2 仅 Windows；其它平台需对应后端 |
| A7 | **自动化测试** | ✅ 已补充：`test_qt_mcp_app_bridge.cpp`（7 用例，mock renderer/transport，无需 WebView2） | — |
| A8 | **沙箱/CSP 细化** | ✅ 已实现（2026-08-11）：双 iframe 沙箱代理 + CSP + permissions→allow + initialized 门禁 + size-changed + WebView2 权限授予 + hostContext + displayMode 协商 + appLogMessage + teardown 等待 + CSP 审计 + 沙箱边界 + 外部域检测 + prefersBorder/domain + hashHtml + 加载看门狗 + **资源限制（内存监控）+ 哈希 allowlist + 未声明域警告 UI**，单测 + `mcp_app_demo` 端到端 + **A4 端到端（`tests_qt_apps_e2e`，官方 `server-threejs` 联调通过）** 验证通过 | 剩余：A5/A6 跨平台渲染（受平台限制） |

### 5.3 已知环境性/历史限制（非本分支回归）

- `sse-retry`（重连时序 500ms）在未改动 master 上同样失败
- 部分旧 auth 场景在 master 上同样失败（conformance 版本差异）
- OAuth endpoint fallback（2025-03-26 场景）明确不实现（baseline 忽略）

## 6. 接手路线图建议

### Phase 1：熟悉（1-3 天）
1. 读本文档 + docs/API_REFERENCE.md + docs/MCP_2026-07-28_规范对照审计.md
2. 按 §2 搭环境，跑通 §2.4 全部测试
3. 跑 `mcp_app_demo` 看端到端效果

### Phase 2：补协议缺口（按优先级）
1. ~~C7 + C8（低成本，SHOULD 项）—— 半天量~~ ✅ 已完成（2026-08-08）：per-request logLevel + W3C trace context
2. **C6**（MRTR sampling/roots 测试）—— 半天量
3. **C1 Tasks 扩展** —— 中等量级，独立模块
4. **C9 JWT 签名基础设施** —— 解锁 C2/C4/C10

### Phase 3：完善 MCP Apps（按优先级）
1. ~~A7 自动化测试~~ ✅ 已完成（2026-08-08）：AppBridge 协议层 7 用例
2. ~~A1 工具调用代理 + A2 权限策略~~ ✅ 已完成（2026-08-08）：McpAppBridge
3. ~~A3 ui/ 方言方法~~ ✅ 已完成（2026-08-08）：open-link / message / update-model-context / request-display-mode
4. **A4 真实服务器端到端**
4. **A5/A6 跨平台后端**（按战略需要）

### Phase 4：扩展生态（可选）
- C2 EMA / C3 DPoP / C4 WIF —— 依赖 JWT 基础设施
- C5 服务端角色 —— 全新领域评估

## 7. 提交与协作规范

### 7.1 Git 规范（Conventional Commits）

```
feat:      新功能
fix:       修复
docs:      文档
test:      测试
refactor:  重构
chore:     杂项
```

### 7.2 分支策略

- 当前开发分支：`feat/protocol-2026-07-28`
- 主分支：`master`（待合并，已确认零冲突）
- 新功能建议开子分支，合并前跑通 §2.4 全部测试

### 7.3 合并前检查清单

- [ ] `tests_qt` 27/27 通过
- [ ] 2026-07-28 conformance 关键场景回归（request-metadata / sep-2322 / http-standard-headers / auth/iss-supported）
- [ ] 旧场景 spot check（initialize / tools_call 2025-11-25）
- [ ] `mcp_app_demo` 运行验证（若涉及 MCP Apps）
- [ ] 工作区干净，无杂散文件

### 7.4 团队协作

- 大任务可用团队模式：Leader 拆解 → 子代理并行（按文件域隔离避免冲突）
- 跨模块接口（如 IMcpTransport）由 Leader 预先定义，子代理只消费不修改

---

## 8. 关键参考

| 资源 | 地址 |
|------|------|
| MCP 2026-07-28 规范 | https://modelcontextprotocol.io/specification/2026-07-28 |
| 官方 conformance | https://github.com/modelcontextprotocol/conformance |
| MCP Apps 文档 | https://modelcontextprotocol.io/extensions/apps |
| WebView2 SDK | https://www.nuget.org/packages/Microsoft.Web.WebView2 |
| QCefView（可选后端） | https://github.com/CefView/QCefView |
| 本分支审计报告 | docs/MCP_2026-07-28_规范对照审计.md |
| API 参考 | docs/API_REFERENCE.md |
