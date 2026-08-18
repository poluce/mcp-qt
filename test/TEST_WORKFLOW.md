# MCP Qt 测试流程指南

本文描述 `test/` 目录的真实测试架构、编译运行方式，以及如何新增用例。以 `test/CMakeLists.txt` 与 `test/main_qt.cpp` 为准。

## 1. 测试目标

根 `CMakeLists.txt` 用 `add_subdirectory(test build_test)`，因此产物在 `build/build_test/`，不是 `build/test/`。

| CMake 目标 | 入口 | 何时构建 | 说明 |
|---|---|---|---|
| `tests_qt` | `test/main_qt.cpp` | 默认 | 主单元/集成套件，链接 `mcp_qt_transport` + `mcp_qt_client` + `mcp_qt_apps` |
| `tests_qt_https_runtime` | `test/test_qt_https_runtime.cpp` | `EXCLUDE_FROM_ALL` | 独立 HTTPS 运行时，避免 TLS 动态装载拖垮主套件 |
| `tests_qt_apps_e2e` | `test/e2e_main.cpp` | `EXCLUDE_FROM_ALL` | MCP Apps 端到端，需另起 node mock 或设 `MCP_APPS_E2E_URL` |

`tests_qt` 的源文件清单以 `test/CMakeLists.txt` 的 `add_executable(tests_qt ...)` 为准（含直接编入的 `examples/multi_server_agent/AgentRegistry.cpp` 与 `AgentReconciler.cpp`）。

不要把 2026-08-07 审计里的 `27/27` 当成今日用例数；未实际跑通本目标前不要写新的 `N/N`。

## 2. 运行器形态

链接了 `Qt6::Test`，但主套件**不是** `QTEST_MAIN` / 按 `Q_OBJECT` 类自动注册。

- 调度与断言宏在 **`test/common.h`**：`TM_RUN_TEST`、`TM_ASSERT_TRUE`、`TM_ASSERT_FALSE`、`TM_ASSERT_EQ`、`TM_ASSERT_STR_CONTAINS`
- `test/tests/common.h` 只是 `#include "../common.h"` 的转发
- `test/main_qt.cpp` 创建 `QCoreApplication`，再对每个自由函数调用 `TM_RUN_TEST(...)`，最后打印汇总并按失败数返回退出码

用例是普通 C++ 函数，例如 `void test_qt_sse_parser_reads_retry();`，必须在 `main_qt.cpp` 里声明并 `TM_RUN_TEST`，同时把 `.cpp` 加进 `test/CMakeLists.txt`。

## 3. 编译与运行

```bash
# 仓库根目录
cmake -B build
cmake --build build --target tests_qt

# Windows
.\build\build_test\tests_qt.exe

# Linux / macOS
./build/build_test/tests_qt
```

独立目标：

```bash
cmake --build build --target tests_qt_https_runtime
cmake --build build --target tests_qt_apps_e2e
```

失败时宏会打印文件、行号、说明，以及 `TM_ASSERT_EQ` 的 Actual / Expected。

## 4. 编写新用例

在 `test/` 下新增 `test_qt_new_feature.cpp`（不必单独写头文件）。

```cpp
#include "tests/common.h"
#include "mcp_qt_client/McpQtClient.h"

void test_something_works() {
    int a = 1;
    int b = 2;
    TM_ASSERT_EQ(a + b, 3, "Addition should work correctly");
    TM_ASSERT_TRUE(a < b, "a must be less than b");
}
```

然后：

1. 把该 `.cpp` 加入 `test/CMakeLists.txt` 的 `tests_qt` 源列表
2. 在 `test/main_qt.cpp` 增加函数声明，并在 `main` 里加一行 `TM_RUN_TEST(test_something_works);`
3. 保持 `CMAKE_AUTOMOC ON`（`test/CMakeLists.txt` 已开启）。只有源文件里出现 `Q_OBJECT` 时才需要 MOC

没有单独的宏头文件，也没有按 `Q_OBJECT` 测试类自动注册的入口。

## 5. 断言宏

定义在 `test/common.h`：

| 宏 | 作用 |
|---|---|
| `TM_ASSERT_TRUE(cond, msg)` | `cond` 为假则记录失败 |
| `TM_ASSERT_FALSE(cond, msg)` | `cond` 为真则记录失败 |
| `TM_ASSERT_EQ(actual, expected, msg)` | 不相等则打印实际/期望值 |
| `TM_ASSERT_STR_CONTAINS(str, pattern, msg)` | `str` 不含 `pattern` 则失败 |
| `TM_RUN_TEST(func)` | 跑一个自由函数用例并计入汇总 |

失败不会立刻 `abort` 整个进程；当前用例继续跑完，由 `main` 根据失败计数返回非零。

## 6. 异步与事件循环

传输与会话是异步的。同步风格用例里用局部 `QEventLoop` + `QTimer` 超时，避免死等。

```cpp
void test_async_response() {
    bool gotResponse = false;
    // 发起异步调用，在回调里置位 gotResponse …

    QEventLoop loop;
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    // connect(..., &loop, &QEventLoop::quit);
    loop.exec();

    TM_ASSERT_TRUE(gotResponse, "Should have received response within 2000ms");
}
```

注意：

- 卡死多半是 Mock socket / Timer 没拆干净；断开时对 `QTcpSocket` 做 `close()` + `deleteLater()`
- 排竞态时打带毫秒时间戳的日志，核对真实触发顺序
- 同步 SDK API（`callTool` 等）内部也会跑 `QEventLoop`，测试线程可以调用；不要在真实 GUI 主线程测这些同步方法
