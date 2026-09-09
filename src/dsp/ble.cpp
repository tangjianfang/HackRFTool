#include "dsp/ble.hpp"

#include <cstring>

namespace hackrftool::dsp {

namespace {

// LSB-first 读 n 比特（与 esb.cpp read_lsb 同约定）
unsigned read_lsb(const std::vector<std::uint8_t>& bits, std::size_t off,
                  unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n && off + i < bits.size(); ++i)
        v |= unsigned(bits[off + i]) << i;
    return v;
}

// 白化 LFSR 抽头：x^7(bit6) ^ x^4(bit3)；输出取移位前的 bit6，字节内 MSB-first
struct Whiten {
    unsigned char reg;
    explicit Whiten(unsigned channel) : reg(unsigned char(channel & 0x7F)) {}
    unsigned char next() {
        const unsigned out = (reg >> 6) & 1u;
        const unsigned fb = ((reg >> 6) & 1u) ^ ((reg >> 3) & 1u);
        reg = unsigned char(((reg << 1) | fb) & 0x7F);
        return unsigned char(out);
    }
};

// 广播接入码字节（空口序）
constexpr unsigned char kAaAir[4] = {0xD6, 0xBE, 0x89, 0x8E};

// AD 结构解析：载荷（AdvA 之后）→ 名称/厂商/flags/发射功率
void parse_ad(BleAdvPdu& p, const std::uint8_t* d, std::size_t n) {
    std::size_t i = 0;
    while (i + 1 < n) {
        const std::size_t len = d[i];
        if (len == 0 || i + 1 + len > n) break;
        const std::uint8_t type = d[i + 1];
        const std::uint8_t* val = d + i + 2;
        const std::size_t vlen = len - 1;
        if (type == 0x09 || type == 0x08)
            p.name.assign(reinterpret_cast<const char*>(val), vlen);
        else if (type == 0xFF)
            p.manufacturer.assign(val, val + vlen);
        else if (type == 0x01 && vlen >= 1)
            p.flags = int(val[0]);
        else if (type == 0x0A && vlen >= 1) {
            p.tx_power_dbm = int(static_cast<signed char>(val[0]));
            p.has_tx_power = true;
        }
        i += 1 + len;
    }
}

} // namespace

void ble_whiten(std::uint8_t* data, std::size_t n, unsigned channel) noexcept {
    Whiten w(channel);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t out = 0;
        for (int bit = 7; bit >= 0; --bit)
            out = std::uint8_t(out | (w.next() << bit));
        data[i] = std::uint8_t(data[i] ^ out);
    }
}

std::uint32_t ble_crc24(const std::uint8_t* data, std::size_t n,
                        std::uint32_t init) noexcept {
    std::uint32_t crc = init & 0xFFFFFF;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0x00065Bu : crc >> 1;
    }
    return crc & 0xFFFFFF;
}

const char* ble_pdu_name(unsigned type) noexcept {
    switch (type) {
    case 0: return "ADV_IND 可连接广播";
    case 1: return "ADV_DIRECT_IND 定向广播";
    case 2: return "ADV_NONCONN_IND 不可连接广播";
    case 3: return "SCAN_REQ 扫描请求";
    case 4: return "SCAN_RSP 扫描响应";
    case 5: return "CONNECT_IND 连接请求";
    default: return "未知类型";
    }
}

std::vector<BleAdvFrame> ble_scan(const std::vector<std::uint8_t>& bits,
                                  unsigned channel) {
    std::vector<BleAdvFrame> out;
    // 最短：前导 8 + AA 32 + 头 16 + AdvA 48 + CRC 24
    constexpr std::size_t kMinBits = 8 + 32 + 16 + 48 + 24;
    const std::size_t n = bits.size();
    if (n < kMinBits) return out;

    std::size_t i = 0;
    while (i + kMinBits <= n) {
        const unsigned pre = read_lsb(bits, i, 8);
        if (pre != 0xAA && pre != 0x55) {
            ++i;
            continue;
        }
        bool aa = true;
        for (int b = 0; b < 4; ++b)
            if (read_lsb(bits, i + 8 + std::size_t(b) * 8, 8) != kAaAir[b]) {
                aa = false;
                break;
            }
        if (!aa) {
            ++i;
            continue;
        }
        const std::size_t pdu_off = i + 8 + 32;
        // 读头 2B 需先去白化——先取 2 字节解出长度，再定全帧
        std::uint8_t head[2];
        {
            Whiten w(channel);
            std::uint8_t mask[2];
            for (int b = 0; b < 2; ++b) {
                std::uint8_t key = 0;
                for (int bit = 7; bit >= 0; --bit)
                    key = std::uint8_t(key | (w.next() << bit));
                mask[b] = key;
            }
            for (int b = 0; b < 2; ++b)
                head[b] = std::uint8_t(read_lsb(bits, pdu_off + std::size_t(b) * 8, 8) ^
                                       mask[b]);
        }
        const unsigned len = head[1] & 0x3F;
        if (len > 31) {   // 广播信道 PDU 载荷上限 31
            ++i;
            continue;
        }
        const std::size_t frame_bytes = 2 + std::size_t(len) + 3;
        const std::size_t end_bit = pdu_off + frame_bytes * 8;
        if (end_bit > n) {
            ++i;
            continue;
        }
        // 整帧字节提取 + 去白化
        std::vector<std::uint8_t> frame(frame_bytes);
        for (std::size_t b = 0; b < frame_bytes; ++b)
            frame[b] = std::uint8_t(read_lsb(bits, pdu_off + b * 8, 8));
        ble_whiten(frame.data(), frame.size(), channel);
        // CRC24 校验（头+载荷）
        const std::uint32_t calc =
            ble_crc24(frame.data(), 2 + std::size_t(len), 0x555555);
        const std::uint32_t rx = unsigned(frame[2 + len]) |
                                 (unsigned(frame[2 + len + 1]) << 8) |
                                 (unsigned(frame[2 + len + 2]) << 16);
        if (calc != rx) {
            ++i;
            continue;
        }
        BleAdvFrame f;
        f.pdu.type = head[0] & 0x0F;
        f.pdu.tx_add = (head[0] & 0x40) != 0;
        f.pdu.rx_add = (head[0] & 0x80) != 0;
        f.pdu.payload_len = len;
        const std::uint8_t* payload = frame.data() + 2;
        if (len >= 6) {
            const std::size_t adv_off =
                (f.pdu.type == 3 || f.pdu.type == 5) ? 6 : 0;   // ScanA/InitA 在前
            if (adv_off + 6 <= len)
                f.pdu.adv_a.assign(payload + adv_off, payload + adv_off + 6);
            if (f.pdu.type != 3 && f.pdu.type != 5)
                parse_ad(f.pdu, payload + 6, len - 6);
        }
        f.bit_offset = i;
        out.push_back(std::move(f));
        i = end_bit;   // 跳过整帧
    }
    return out;
}

} // namespace hackrftool::dsp
