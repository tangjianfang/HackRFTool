#include "dsp/esb.hpp"

namespace hackrftool::dsp {

namespace {

unsigned read_lsb(const std::vector<std::uint8_t>& bits, std::size_t off, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n && off + i < bits.size(); ++i)
        v |= unsigned(bits[off + i]) << i;
    return v;
}

unsigned short crc16_lsb(const std::vector<std::uint8_t>& bits, std::size_t from,
                         std::size_t count, unsigned short init) {
    unsigned short crc = init;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned fb = (crc & 1u) ^ bits[from + i];
        crc >>= 1;
        if (fb != 0u) crc ^= 0x8408u;
    }
    return crc;
}

} // namespace

std::vector<EsbFrame> esb_scan(const std::vector<std::uint8_t>& bits) {
    std::vector<EsbFrame> out;
    constexpr unsigned kPreambles[2] = {0xAA, 0x55};
    constexpr unsigned short kInits[2] = {0xFFFF, 0x3D18};
    const std::size_t n = bits.size();
    // 最短帧：前导 8 + 地址 3×8 + PCF 9 + CRC 16，外加前导前的 1 bit 余量
    constexpr std::size_t kMinFrame = 8 + 3 * 8 + 9 + 16;
    if (n < kMinFrame) return out;

    std::size_t i = 0;
    while (i + kMinFrame <= n) {
        bool matched = false;
        const unsigned pre = read_lsb(bits, i, 8);
        for (const unsigned pre_ok : kPreambles) {
            if (pre != pre_ok) continue;
            for (const unsigned addr_len : {3u, 4u, 5u}) {
                const std::size_t p = i + 8;
                const std::size_t pcf_off = p + addr_len * 8;
                if (pcf_off + 9 + 16 > n) continue;
                std::vector<std::uint8_t> addr(addr_len);
                for (unsigned b = 0; b < addr_len; ++b)
                    addr[b] = static_cast<std::uint8_t>(read_lsb(bits, p + b * 8, 8));
                const unsigned payload_len = read_lsb(bits, pcf_off, 6);
                if (payload_len > 32u) continue;
                const std::size_t payload_off = pcf_off + 9;
                const std::size_t crc_off = payload_off + payload_len * 8;
                if (crc_off + 16 > n) continue;
                const unsigned short rx_crc =
                    static_cast<unsigned short>(read_lsb(bits, crc_off, 16));
                for (const unsigned short init : kInits) {
                    if (crc16_lsb(bits, p, crc_off - p, init) != rx_crc) continue;
                    EsbFrame f;
                    f.address = addr;
                    f.payload.resize(payload_len);
                    for (unsigned b = 0; b < payload_len; ++b)
                        f.payload[b] = static_cast<std::uint8_t>(
                            read_lsb(bits, payload_off + b * 8, 8));
                    f.bit_offset = i;
                    out.push_back(std::move(f));
                    i = crc_off + 16;   // 跳过整帧继续
                    matched = true;
                    break;
                }
                if (matched) break;
            }
            if (matched) break;
        }
        if (!matched) ++i;
    }
    return out;
}

std::string hex_dump(const std::vector<std::uint8_t>& bytes) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace hackrftool::dsp
