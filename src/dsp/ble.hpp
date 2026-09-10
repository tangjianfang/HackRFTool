// BLE（Bluetooth Low Energy）广播包链路层解析（#108）：
//   前导(0xAA/0x55) + 广播接入码 0x8E89BED6（空口 LSB 字节序 D6 BE 89 8E，
//   不白化）+ 白化(PDU + CRC24)。
//   PDU：头 2B（type 4b / RFU / TxAdd / RxAdd ‖ 长度 6b）+ 载荷
//   （广播类 PDU 前 6B 为 AdvA，其后为 AD 结构：len/type/data）。
//   CRC24：poly 0x00065B（反射式右移），广播初值 0x555555，空口 LSB-first 3B。
//   白化：x^7+x^4+1 七位 LFSR，种子=信道号；字节内 MSB-first 取抽头。
// 纯函数、无 UI/硬件依赖；与 esb.hpp 同一比特约定（每字节 LSB-first）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hackrftool::dsp {

struct BleAdvPdu {
    unsigned type = 0;      // PDU 类型：0=ADV_IND 1=ADV_DIRECT_IND 2=ADV_NONCONN_IND
                            // 3=SCAN_REQ 4=SCAN_RSP 5=CONNECT_IND
    bool tx_add = false;    // TxAdd：1=AdvA 为随机地址
    bool rx_add = false;
    std::vector<std::uint8_t> adv_a;        // 广播者地址（6B；SCAN_REQ/CONNECT_IND 在偏移 6）
    std::string name;                       // AD 0x09/0x08 本地名称（无则空）
    std::vector<std::uint8_t> manufacturer; // AD 0xFF 厂商数据（含 2B company id 小端）
    int flags = -1;                         // AD 0x01 发现模式
    int tx_power_dbm = 0;                   // AD 0x0A
    bool has_tx_power = false;
    unsigned payload_len = 0;               // PDU 载荷长（头 2B 不含）
};

struct BleAdvFrame {
    BleAdvPdu pdu;
    std::size_t bit_offset = 0;   // 帧起始（前导）在比特流中的下标
};

// 在解调比特流（LSB-first）中扫描广播信道 packet；channel 用于白化种子
//（37/38/39 广播信道）。CRC 不过不产出。
[[nodiscard]] std::vector<BleAdvFrame> ble_scan(const std::vector<std::uint8_t>& bits,
                                                unsigned channel);

// 统计前导+广播接入码出现次数（不校验 CRC）——排障用：AA>0 而 ble_scan=0
// 说明有包但白化/CRC 约定或解调质量有问题
[[nodiscard]] std::size_t ble_count_aa(const std::vector<std::uint8_t>& bits) noexcept;

// 变体探测（排障专用，#109）：白化/CRC 空口约定 16 变体（位序 LSB/MSB ×
// 种子 ch/ch|0x40 × 抽头 b6/fb × CRC 空口字节序）逐一试解，counts[16]
// 累计各变体 CRC 通过帧数——真机日志定位正确约定后固化进 ble_scan
void ble_debug_variants(const std::vector<std::uint8_t>& bits, unsigned channel,
                        unsigned counts[16]) noexcept;

// 白化/去白化（同一 LFSR 自逆）：data = PDU+CRC 字节（空口序）
void ble_whiten(std::uint8_t* data, std::size_t n, unsigned channel) noexcept;

// CRC-24（BLE 反射式，init 广播 0x555555）
[[nodiscard]] std::uint32_t ble_crc24(const std::uint8_t* data, std::size_t n,
                                      std::uint32_t init = 0x555555) noexcept;

// PDU 类型中文名（UI 显示）
[[nodiscard]] const char* ble_pdu_name(unsigned type) noexcept;

} // namespace hackrftool::dsp
