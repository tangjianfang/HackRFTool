# Epics — HackRFTool

升级提案登记（rounds 内不可执行；状态仅由用户翻转为 approved/rejected）。

## EP-1: Meteor M2 LRPT 图像解压缩（提案自 evolve #77）

- **类型**: innovate
- **触发**: 标准件 (d) 无法切片——维特比译码（r=1/2 卷积，约束长度 7）+ 同步反转检测 + 帧去交错 + MCW/Huffman 压缩图像重组 + 灰度成像是紧耦合管线，估 ~1200 行，无法分解为独立绿的 ≤300 行轮（#70/#71 已交付物理层：Costas/Gardner/ASM 均已绿）
- **假设**: 符号流与帧同步已可靠（ASM 命中=帧头），剩余全部为数字域处理，无硬件/时序风险；工程量是唯一障碍
- **实施草案**:
  - 范围: `src/dsp/meteor_vitab.{hpp,cpp}`（维特比+去咬尾）、`src/dsp/meteor_decomp.{hpp,cpp}`（去交错+MCW 解压+行重组）、云图页 Meteor 成像视图复用 apt_view
  - 验证: 合成卷积编码→加噪→译码 BER 断言；合成 MCW 压缩块→解压往返断言；真机过境出图（与 satdump 对照）
  - 回滚: 独立新模块+云图页模式分支，不影响既有 APT 路径
  - 预估: 批准后 4-6 个独立绿轮（先维特比→再解压→后成像接线）
- **状态**: approved（2026-09-06 用户批准）
- **分解切片**（入 Tier 4，顺序执行）:
  - EP-1.1 维特比译码器纯函数（r=1/2 K=7 CCSDS G1=0x79/G2=0x5B 相位反转；合成 BER 断言）
  - EP-1.2 去随机化+帧去交错（1024bit 包，64bit 头 + 960bit 载荷）
  - EP-1.3 MCW 解析+反 DCT 压缩解压（气象信道分离）
  - EP-1.4 云图页 Meteor 成像接线（apt_view 复用灰度行缓冲）
