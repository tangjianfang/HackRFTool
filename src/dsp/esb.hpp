// nRF24 ESB（Enhanced ShockBurst）帧搜索器：在解调比特流中找
// 前导码 + 地址 + PCF + 载荷 + CRC16 全对的帧（CRC 不过不产出）。
// 空口约定：比特 LSB-first；前导 {0xAA,0x55}；地址 3-5 字节；
// CRC16-CCITT（LSB 进位，poly 反转 0x8408），初值尝试 {0xFFFF, 0x3D18}。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hackrftool::dsp {

struct EsbFrame {
    std::vector<std::uint8_t> address;   // 3-5 字节，发送端自然字节序
    std::vector<std::uint8_t> payload;   // 0-32 字节
    std::size_t bit_offset = 0;          // 帧起始在输入比特流中的下标
};

[[nodiscard]] std::vector<EsbFrame> esb_scan(const std::vector<std::uint8_t>& bits);

} // namespace hackrftool::dsp
