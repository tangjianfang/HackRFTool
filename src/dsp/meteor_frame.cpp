#include "dsp/meteor_frame.hpp"

#include <optional>

namespace hackrftool::dsp {

void Derandomizer::apply(std::uint8_t* bytes, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t pn = 0;
        for (int b = 7; b >= 0; --b) {
            // h(x)=x^8+x^7+x^5+x^3+1：抽头 8,7,5,3
            const unsigned fb = ((sr_ >> 7) ^ (sr_ >> 6) ^ (sr_ >> 4) ^
                                 (sr_ >> 2)) & 1u;
            sr_ = ((sr_ << 1) | fb) & 0xFFu;
            pn = std::uint8_t((pn << 1) | (sr_ & 1u));
        }
        bytes[i] ^= pn;
    }
}

std::vector<std::uint8_t> bits_to_bytes(const std::vector<std::uint8_t>& bits) {
    std::vector<std::uint8_t> out(bits.size() / 8);
    for (std::size_t i = 0; i + 7 < bits.size(); i += 8) {
        std::uint8_t v = 0;
        for (int b = 0; b < 8; ++b) v = std::uint8_t((v << 1) | (bits[i + b] & 1u));
        out[i / 8] = v;
    }
    return out;
}

std::optional<MeteorFrameAssembler::Frame> MeteorFrameAssembler::push(
    std::uint8_t bit) {
    buf_.push_back(std::uint8_t(bit & 1u));
    ++bit_count_;
    if (bit_count_ < 1024) return std::nullopt;

    // 凑满 1024 bit → 打包 128 字节 → 去随机化 → 头解析 → 载荷
    const std::vector<std::uint8_t> bytes = bits_to_bytes(buf_);
    buf_.clear();
    bit_count_ = 0;
    derand_.reset();
    std::vector<std::uint8_t> de = bytes;
    derand_.apply(de.data(), de.size());

    Frame f;
    f.counter = std::uint16_t((std::uint16_t(de[0]) << 8) | de[1]);
    f.payload.assign(de.begin() + 8, de.end());   // 120B 载荷
    ++frames_;
    return f;
}

} // namespace hackrftool::dsp
