// Meteor LRPT 帧层（EP-1.2）：维特比比特流 → 1024bit（128 字节）帧。
// CCSDS 转移去随机化（h(x)=x⁸+x⁷+x⁵+x³+1，种子 0xFF）+ 帧头解析
// （帧计数）+ 载荷输出。字节去交错的真机参数（深度/置换表）留待
// EP-1.4 过境对照校准——本层提供可配置钩子。
// 纯 C++；合成回环测试（随机化↔去随机化、装帧↔拆帧）直测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hackrftool::dsp {

// CCSDS 伪随机序列（转移密度）：生成器 h(x)=x^8+x^7+x^5+x^3+1、种子 0xFF
class Derandomizer {
public:
    // 就地 XOR 序列（重置种子；同一实例重复调用=序列续跑，除非 reset）
    void apply(std::uint8_t* bytes, std::size_t n);
    void reset() { sr_ = 0xFFu; }

private:
    unsigned sr_ = 0xFFu;
};

// 比特流 → 字节（MSB 先）辅助
[[nodiscard]] std::vector<std::uint8_t> bits_to_bytes(
    const std::vector<std::uint8_t>& bits);

// LRPT 帧装配：1024bit（128B）/帧 = 头 8B（帧计数在首 2B 低 10bit）+
// 载荷 120B。feed 比特（0/1），凑满一帧输出载荷；帧计数从头部提取。
class MeteorFrameAssembler {
public:
    struct Frame {
        std::uint16_t counter = 0;
        std::vector<std::uint8_t> payload;   // 120B（已去随机化）
    };

    // 每比特喂入；帧完成时返回该帧（含去随机化），否则 nullopt
    [[nodiscard]] std::optional<Frame> push(std::uint8_t bit);

    [[nodiscard]] std::uint64_t frames_out() const noexcept { return frames_; }

private:
    std::vector<std::uint8_t> buf_;   // 128 字节累积（bit 顺序入）
    std::size_t bit_count_ = 0;
    std::uint64_t frames_ = 0;
    Derandomizer derand_;
};

} // namespace hackrftool::dsp
