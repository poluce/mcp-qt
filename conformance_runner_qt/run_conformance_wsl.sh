#!/bin/bash
# run_conformance_wsl.sh
# WSL 互操作包装：WSL 侧环境变量不会传递给 Windows 进程（含 WSLENV 场景下实测不通），
# 而官方 conformance CLI 通过 MCP_CONFORMANCE_SCENARIO / MCP_CONFORMANCE_CONTEXT /
# MCP_CONFORMANCE_PROTOCOL_VERSION 环境变量驱动被测客户端。
# 本脚本把这些 env 转为 argv（--scenario / --context / --protocol）传给 Windows runner，
# 并补上 Qt DLL 搜索路径。
#
# 用法（conformance CLI 的 --command 指向本脚本）：
#   npx -y @modelcontextprotocol/conformance@0.2.0-alpha.10 client \
#     --command "$(pwd)/conformance_runner_qt/run_conformance_wsl.sh" --suite all
#
# 可选环境变量：
#   QT_MINGW_BIN   Qt MinGW bin 目录（缺省自动探测 /mnt/e/Qt6/*/mingw_64/bin 等常见位置）

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXE="$SCRIPT_DIR/../build/conformance_runner_qt/mcp_client_conformance_qt.exe"

# Qt DLL 搜索路径（Windows 进程从 Windows PATH 解析 DLL）
if [ -n "${QT_MINGW_BIN:-}" ]; then
    export PATH="$QT_MINGW_BIN:$PATH"
else
    for cand in /mnt/e/Qt6/*/mingw_64/bin /mnt/c/Qt/*/mingw_64/bin /c/Qt/*/mingw_64/bin; do
        if [ -d "$cand" ] && ls "$cand"/Qt6Core.dll >/dev/null 2>&1; then
            export PATH="$cand:$PATH"
            break
        fi
    done
fi

echo "[WRAPPER] SCENARIO=$MCP_CONFORMANCE_SCENARIO PROTO=$MCP_CONFORMANCE_PROTOCOL_VERSION" >&2
ARGS=()
if [ -n "${MCP_CONFORMANCE_SCENARIO:-}" ]; then
    ARGS+=(--scenario "$MCP_CONFORMANCE_SCENARIO")
fi
if [ -n "${MCP_CONFORMANCE_CONTEXT:-}" ]; then
    ARGS+=(--context "$MCP_CONFORMANCE_CONTEXT")
fi
if [ -n "${MCP_CONFORMANCE_PROTOCOL_VERSION:-}" ]; then
    ARGS+=(--protocol "$MCP_CONFORMANCE_PROTOCOL_VERSION")
fi

exec "$EXE" "${ARGS[@]}" "$@"
