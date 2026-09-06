#!/bin/sh
# 在 cdb 下长跑指定模式（默认 autosc=扫描+抓包页压测）复现崩溃
MODE=${1:-autosc}
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
[ -x "$CDB" ] || { echo "cdb 不存在：$CDB（装 Windows SDK Debugging Tools）" >&2; exit 2; }
cd /c/tjf/github/HackRFTool/out/build/x64-debug/Debug || { echo "Debug 产物目录不存在（先构建 x64-debug）" >&2; exit 2; }
[ -x ./HackRFTool.exe ] || { echo "HackRFTool.exe 不存在（先构建 x64-debug）" >&2; exit 2; }
rm -f cdb-crash.log
hackrf_spiflash -R >/dev/null 2>&1
sleep 2
(sleep 700; echo "kb"; sleep 1; echo "q") | \
  "$CDB" \
  -g -G -lines -logo cdb-crash.log -c "g" -- ./HackRFTool.exe $MODE
