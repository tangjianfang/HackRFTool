# Evolve 运行报告 — #59–#78（2026-09-06，N=20）

主题（用户指令）：全量日志系统（UI/点击/事件/数据详细记录），据日志推算分析问题；**减少视觉识别**（用户明确指出视觉识别不准）。

## 指标趋势

| 轮 | 目标 | findings | fixes | result | 断言 |
|---|---|---|---|---|---|
| #59 | 遥测核心+点击埋点 | 2 | 1 | green+progress | 217→228 |
| #60 | UI 状态/DSP 1Hz/断言工具 | 0 | 1 | green+progress | 228 |
| #61 | APT 诊断+scan/record/sweep | 1 | 1 | green+progress | 228 |
| #62 | 信号库非模态弹窗 | 2 | 1 | green+progress | 228 |
| #63 | Y 轴动态档 | 0 | 1 | green+progress | 228 |
| #64 | 应用内日志查看器 | 0 | 1 | green+progress | 228 |
| #65 | 云图状态卡 | 0 | 1 | green+progress | 228 |
| #66 | 数据面覆盖缺口 | 0 | 1 | green+progress | 228 |
| #67 | 制度化（L14/AGENTS/--order） | 0 | 1 | green+no-progress* | 228 |
| #68 | selftest 事件链断言 | 0 | 1 | green+progress | 228 |
| #69 | 文件轮转回归 | 0 | 1 | green+progress | 230 |
| #70 | Meteor QPSK 纯函数 | 1 | 1 | green+progress | 235 |
| #71 | Meteor 接线 | 1 | 1 | green+progress | 235 |
| #72 | 眼图假阳性治理 | 1 | 1 | green+no-progress | 235 |
| #73 | 池刷新 | 0 | 0 | 绿（池维护） | 235 |
| #74 | README 功能补全 | 0 | 1 | green+no-progress（文档） | 235 |
| #75 | 体积测量（117B/行→4.4h） | 0 | 0 | green+no-progress（measured） | 235 |
| #76 | 验收报告终态 | 0 | 1 | green+no-progress（文档） | 235 |
| #77 | EP-1 epic 提案 | 0 | 1 | 绿（提案） | 235 |
| #78 | 回顾（重放审计✓/lessons/本文） | — | — | — | 235 |

*#67 顺序断言实测 PASS（measured 项）。合计：findings 8 | fixes 16 | regressions 0；断言 217→235（+18）；每轮 ctest 5/5。

## 交付清单

1. **遥测系统**：JSONL 结构化事件（LIFE/UI/RADIO/AUDIO/DSP/APT/METEOR/SCAN/SIGDB/SETTINGS/ESB），文件 1MB×3 轮转，环形缓冲+count_event 自测断言，UI/点击/事件/数据四类全覆盖
2. **日志验收新范式**：tools/log-assert.py（--order 顺序断言）替代截图+视觉识别；selftest 报告自带事件链证据；应用内日志查看器（工具栏「日志」）
3. **云图排查就绪**（B6）：APT diag（子载波/同步率/行数）+ METEOR diag（眼图/ASM）+ 云图页状态卡三态灯——待过境出图
4. **Meteor 三部曲 #3**：QPSK Costas+Gardner+ASM 帧同步（合成 >95% 判决）+ 云图页 Meteor 模式（EP-1 解压缩提案待决策）
5. **信号库弹窗/Y 轴档/README/验收报告终态**

## 经验

- L14（新）：视觉幻觉两次实证 → 日志断言为准；埋点三坑（通知刷屏/kv 撞名/声明前插入）
- L1/L13 verified+1（grep 中文错误行跑陈旧 exe；bash 反引号命令替换第九变体）

## 重放审计（回顾轮）

#59/#70 两轮 checkout 父提交确认新测试缺失、本提交齐备——**通过，无 gamed**。

## 遗留

- EP-1（Meteor 解压缩）proposed 待用户决策
- T1.5 导频 PLL 定量、T1.6 过境出图、T4.2 --csv、T4.4 README 路径统一（池内）
