---
name: qt-ci-troubleshooting
description: mcp-qt 仓库 Qt/MinGW Windows CI 构建与测试的踩坑经验：环境差异、退出码诊断、Qt 版本选择、工具链 ABI、header 大小写等。CI 失败或本地/CI 行为不一致时激活。
version: 1.0.0
authors:
  - mcp-qt maintainers
---

## Overview

本 skill 记录 mcp-qt 仓库（Qt6 + MinGW，Windows 主平台）在 GitHub Actions CI 搭建与调试中踩过的全部坑。CI 是 2026-09 新建的，第一次跑连续暴露 7 个环境差异，每个都有明确的诊断方法和修复。以后再遇到 CI 失败、本地通过但 CI 挂、或 Qt 版本相关行为差异，先对照本 skill 的清单逐项排查，不要重新踩一遍。

## Trigger

以下任一情况激活本 skill：

1. GitHub Actions CI 失败，需要排查 workflow 或环境问题；
2. 本地测试通过但 CI 失败（环境差异）；
3. Windows exe 运行时报 0xC0000139 / 0xC0000135 / 无输出直接退出；
4. 需要选择或更换 CI 上的 Qt 版本；
5. conformance 套件在 CI 上跑挂；
6. 修改 .github/workflows/ci.yml。

## 本地构建约定（基线）

本地构建与测试统一用 PowerShell 在 Windows 侧执行，WSL 只做源码编辑：

    $env:PATH = 'E:\Qt6\6.11.0\mingw_64\bin;' + $env:PATH
    & 'E:\CodeSoftware\CMake\bin\cmake.exe' --build 'F:\B_My_Document\GitHub\mcp-qt\build' --target tests_qt -j 8
    & 'F:\B_My_Document\GitHub\mcp-qt\build\build_test\tests_qt.exe'

本地 Qt 是 6.11.0（官方安装器渠道），CI 用 6.10.3（见下）。本地与 CI 的 Qt 版本不同是**有意为之**，不要试图统一。

## CI 七个坑（按发现顺序，全部已修复）

### 1. Qt 6.11.0 在官方在线仓库装不了

- **现象**：aqtinstall 报 Failed to locate XML data for Qt version '6.11.0'。
- **根因**：官方在线仓库（download.qt.io）的 6.11.0 目录结构变了——只有分架构目录（qt6_6110_mingw/ 等），缺 aqt 需要的统一索引 qt6_6110/qt6_6110/Updates.xml（404）。6.10.3 及更早结构完整（200）。
- **修复**：CI 用 6.10.3。验证方法：curl -s -o /dev/null -w "%{http_code}" https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_XXXX/qt6_XXXX/Updates.xml

### 2. conformance CLI 需要 Node 22

- **现象**：npx conformance 崩溃，日志有 npm warn EBADENGINE content-type@3.0.0 required node >=22。
- **修复**：setup-node 用 node-version: '22'。

### 3. YAML 转义陷阱：\b 变退格字符

- **现象**：workflow 报 "workflow file issue"，本地 yaml.safe_load 报 unacceptable character #x0008。
- **根因**：用 Python 脚本生成 workflow 时，\bin 在 Python 字符串里被解释成 \b（退格）写进文件。
- **教训**：改完 workflow 必须本地 python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))" 验证；生成含反斜杠内容的文件时用 String.raw 或四重转义。

### 4. conformance --command 必须用反斜杠

- **现象**：CI 上 conformance 客户端全部 'build' is not recognized as an internal or external command。
- **根因**：conformance CLI 以 shell:true 经 cmd.exe 拉起客户端，cmd 不认正斜杠路径。
- **修复**：--command build\conformance_runner_qt\mcp_client_conformance_qt（反斜杠）。本地 WSL 侧跑时同样用反斜杠。

### 5. 0xC0000139（入口点未找到）= MinGW 运行时 DLL 版本冲突

- **现象**：exe 启动即退，EXITCODE=-1073741511（0xC0000139），无任何输出。
- **根因**：Qt 包自带的 MinGW 运行时 DLL（libstdc++-6.dll 等）与工具链版本不一致，谁在 PATH 前面谁赢。
- **修复**：Qt bin **追加**到 PATH（set PATH=%PATH%;%QT_ROOT_DIR%\bin），让工具链的运行时 DLL 优先。**不要前置**。
- **诊断技巧**：run 步骤加 echo EXITCODE=%ERRORLEVEL% 拿到真实退出码；0xC0000135=缺 DLL，0xC0000139=ABI 不匹配，exit 1=程序正常返回。

### 6. cmake 会捡 runner 预装的 C:\mingw64

- **现象**：编译通过但运行 0xC0000139；日志显示 Check for working CXX compiler: C:/mingw64/bin/c++.exe。
- **根因**：install-qt-action 默认不装 MinGW 工具链，cmake 就捡了 runner 预装的编译器，与 Qt DLL ABI 不匹配。
- **修复**：install-qt-action 加 tools: 'tools_mingw1310'，Configure 步骤显式指定编译器：
  -DCMAKE_CXX_COMPILER="$env:IQTA_TOOLS\mingw1310_64\bin\g++.exe" -DCMAKE_C_COMPILER="$env:IQTA_TOOLS\mingw1310_64\bin\gcc.exe"
- **注意**：工具链目录在 $env:IQTA_TOOLS（= Qt\Tools），不是 $env:QT_ROOT_DIR\..\Tools（QT_ROOT_DIR 上两级才是 Tools）。

### 7. Qt 6.9.x 的 setRawHeader 小写化 header 名

- **现象**：换 6.9.3 后 4 个 HTTP 测试挂（420/9 断言），全部是 header 断言失败（"Should set content type" 等）。
- **根因**：Qt 6.9.x 的 QNetworkRequest::setRawHeader 会把 header 名小写化（QTBUG-131474），测试断言大小写敏感（contains("Content-Type: application/json")）。6.10+ 无此问题。
- **修复**：CI 用 6.10.3。若未来必须用 6.9.x，把测试断言改成大小写不敏感。

## 诊断流程（CI 失败时按序执行）

1. gh run list --limit 5 看状态；
2. gh run view <id> --log-failed 看失败步骤；grep ##[error]、EXITCODE、Stderr；
3. exe 无输出直接退 → 加 echo EXITCODE=%ERRORLEVEL% 拿真实退出码，对照 0xC0000135/0xC0000139/exit 1 分类；
4. 本地通过 CI 挂 → 逐项对照上面七个坑；
5. 改 workflow 后本地 yaml 验证再 push；
6. gh run watch <id> --exit-status 等结果（注意 watch 输出里 "✓ Complete job" 可能只是单个 job，最终状态以 gh run list 为准）。

## 当前 CI 配置快照（2026-09）

- Qt 6.10.3 + tools_mingw1310（显式安装 + 显式编译器路径）
- Node 22
- 两个 job：build-test（构建 + tests_qt + e2e）、conformance（draft + auth 套件，CLI 固定 @0.2.0-alpha.10）
- 运行 exe 前 Qt bin 追加到 PATH
- 全绿基线：tests_qt 72/72、conformance draft 173/173、auth 238/238、e2e 1/1

## 相关文件

- .github/workflows/ci.yml — CI 定义（含全部修复的注释）
- docs/architecture.md — 终态架构文档
- AGENTS.md — 仓库工作约定（WSL 不跑 Windows exe 等）
