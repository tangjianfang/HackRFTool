// nRF24 ESB（Enhanced ShockBurst）帧搜索器：在解调比特流中找
// 前导码 + 地址 + PCF + 载荷 + CRC16 全对的帧（CRC 不过不产出）。
// 空口约定：比特 LSB-first；前导 {0xAA,0x55}；地址 3-5 字节；
// CRC16-CCITT（LSB 进位，poly 反转 0x8408），初值尝试 {0xFFFF, 0x3D18}。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hackrftool::dsp {

struct EsbFrame {
    std::vector<std::uint8_t> address;   // 3-5 字节，发送端自然字节序
    std::vector<std::uint8_t> payload;   // 0-32 字节
    std::size_t bit_offset = 0;          // 帧起始在输入比特流中的下标
    std::uint8_t pid = 0;                // PCF 2bit 包序号（+1/包，重传同值）
    bool no_ack = false;                 // PCF 1bit NO_ACK 标志
};

[[nodiscard]] std::vector<EsbFrame> esb_scan(const std::vector<std::uint8_t>& bits);

// 地址过滤（#102）：filter 空=全匹配；否则全字节精确匹配（不做前缀匹配——
// 窄带误配会混入干扰源）
[[nodiscard]] bool addr_match(const std::vector<std::uint8_t>& address,
                              const std::vector<std::uint8_t>& filter) noexcept;

// 载荷解读启发式（#103）：给"看不懂的 hex"加人话标注。结论全部是"疑似"
// 级——确定性的只有 CRC/PCF 字段；厂商表是已知地址/前缀的公开常识。
struct EsbInterp {
    std::string vendor;                // 已知地址/前缀备注（空=未识别）
    float entropy_bits = 0.f;          // 载荷香农熵 0..8 bit/字节
    bool looks_encrypted = false;      // 高熵互异 → 疑似加密/随机化载荷
    bool all_zero = false;             // 全零 → 空保持包（在线无数据）
    int seq_byte = -1;                 // 跨帧 +1 的疑似序号字节下标
    std::vector<unsigned> diff_bytes;  // 与上一帧差异字节下标（空=无历史或全同）
};

// history = 同地址的历史载荷（旧→新，调用方给最近最多 4 条即可）
[[nodiscard]] EsbInterp esb_interpret(
    const std::vector<std::uint8_t>& address,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::vector<std::uint8_t>>& history);

// 字节序列 → 大写 hex 文本，空格分隔（"FB 50 00"）；空输入 → 空串
[[nodiscard]] std::string hex_dump(const std::vector<std::uint8_t>& bytes);

} // namespace hackrftool::dsp
