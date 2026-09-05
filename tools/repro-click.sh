#!/bin/sh
# cdb 下复现"点击全频段"崩溃（selfclick 模式）
cd /c/tjf/github/HackRFTool/out/build/x64-debug/Debug || exit 1
rm -f cdb-crash.log selfclick.log
hackrf_spiflash -R >/dev/null 2>&1
sleep 2
(sleep 300; echo "kb"; sleep 1; echo "q") | \
  "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe" \
  -g -G -lines -logo cdb-crash.log -c "g" -- ./HackRFTool.exe selfclick
echo "cdb-exit=$?"
