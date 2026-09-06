#!/bin/sh
# 【已退役，仅存档】selfclick 模式已随 #52 原生骨架重构移除（L4 根因已修，
# 无复现需要）。UI 驱动改用 out/drive-pages.ps1（WM_COMMAND PostMessage），
# 崩溃复现用 repro-crash.sh（任意命令行模式仍可用）。
echo "repro-click: selfclick 已退役（见文件头注释）" >&2
exit 2
