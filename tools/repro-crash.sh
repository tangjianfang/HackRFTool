#!/bin/sh
# 在 cdb 下长跑指定模式（默认 autosc=扫描+抓包页压测）复现崩溃
MODE=${1:-autosc}
cd /c/tjf/github/HackRFTool/out/build/x64-debug/Debug || exit 1
rm -f cdb-crash.log
hackrf_spiflash -R >/dev/null 2>&1
sleep 2
(sleep 700; echo "kb"; sleep 1; echo "q") | \
  "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe" \
  -g -G -lines -logo cdb-crash.log -c "g" -- ./HackRFTool.exe $MODE
echo "cdb-exit=$?"
