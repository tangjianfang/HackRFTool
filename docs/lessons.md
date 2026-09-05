# Lessons — HackRFTool

Entry template: `| id | one-sentence lesson | how to apply (an executable action, not a slogan) | source | verified |`

- Update an existing entry on the same topic instead of adding a duplicate.
- Every retrospective re-checks entries: confirmed in action → `verified +1`; proved wrong or stale → rewrite or delete on the spot.
- ≥ 3 same-theme lessons with ≥ 5 total verifications → crystallize into a named SKILL.md mechanism or a standalone skill; mark the entries `crystallized → <where>`.

## 测试与验证

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L1 | 构建失败后 ctest 跑的是陈旧二进制，"全绿"是假象 | 每次先确认 `cmake --build` 输出零 error（`grep -cE 'warning\|error'` 为 0）再跑 ctest；宁可多跑一次空构建；grep 到 LNK1104（exe 被占用）先 Stop-Process 清残留 GUI 实例再编 | evolve#3/#4 连续两次踩中（字段名笔误后 ctest 仍绿）；evolve#51 LNK1104 被前置检查拦住 | 3 |
| L2 | 手算期望值易错（7.5 截断写成 6）——断言值用脚本算，不要心算 | 写数值断言前用一行 python/echo 核算；测试失败先验期望值再查实现 | evolve#4 waterfall_level 中点 | 1 |
| L5 | PowerShell `Start-Process -ArgumentList` 传参会吞/改坏 GUI 程序的模式参数（三次截图全同、自动模式从未生效，状态栏仍显示"未开始"） | 自动化驱动 GUI 用直接 exec（bash 后台 / CTest）而非 Start-Process 包装；截图前先核对页内状态（页签高亮/状态栏文本）与预期模式一致再采 | evolve#50 复截图三次全同 | 1 |

## 进程与协作

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L3 | 并行会话在同一工作树会互相踩（增量构建竞速、文件被改、Edit 锚点失配） | 开工前 ListAgents 查邻居；发现文件被外部修改立即重读再改；构建"成功"但 mtime 未变即 touch 强制重链；会话压缩/中断恢复先 `git diff` + todo 对账在制工作，勿盲目重做或回滚 | 2026-09-05 与 evolve-07 会话碰撞全程；evolve#50 压缩恢复对账 | 2 |

## WinFlux UI

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L4 | 点击处理器内 State::set() 之后访问 app = UAF（set 同步重建树释放正在执行的 lambda） | 副作用全部前置，set 永远最后一句；多步逻辑包自由函数（App& 走参数）——详见 memory/winflux-set-terminal-rule | 2026-09-05 用户崩溃根因（crash-9472/5652 双实证） | 1 |
