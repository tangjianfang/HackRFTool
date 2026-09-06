#include "dsp/meteor_vitab.hpp"

#include <algorithm>
#include <cmath>

namespace hackrftool::dsp {

namespace {

// CCSDS K=7 生成多项式：G1=0x79（171o）、G2=0x5B（133o）。
// 7bit 抽头窗 = 6bit 历史（网格状态）+ 当前输入（bit0）——多项式 bit6
// 抽头使两条入边的输出不同（这是网格区分度来源，首版实现丢过它）
constexpr unsigned kG1 = 0x79;
constexpr unsigned kG2 = 0x5B;

inline int parity(unsigned v) {
    int p = 0;
    while (v != 0) {
        p ^= int(v & 1u);
        v >>= 1;
    }
    return p;
}

// 7bit 窗（含当前输入）→ 编码输出对（G2 反相惯例）
inline std::pair<int, int> enc_out(unsigned win7) {
    const int g1 = parity(win7 & kG1) ? 1 : -1;
    const int g2 = parity(win7 & kG2) ? 1 : -1;
    return {g1, -g2};
}

} // namespace

std::pair<int, int> ConvolutionalEncoder::push(int bit) {
    const unsigned win7 = unsigned(((reg_ << 1) | unsigned(bit)) & 0x7Fu);
    reg_ = win7 & 0x3Fu;   // 网格状态 = 窗的低 6 位（历史）
    ++n_;
    return enc_out(win7);
}

ViterbiDecoder::ViterbiDecoder() {
    for (float& v : pm_) v = 0.0f;
    for (float& v : pm_next_) v = 0.0f;
    for (auto& layer : hist_) {
        for (auto& h : layer) h = 0;
    }
}

int ViterbiDecoder::push(float soft_g1, float soft_g2) {
    // 状态 ns = 6bit 历史；输入 b = ns&1（编码器布局 bit0=最新）。
    // 两条入边的前驱 prev∈{ns>>1, (ns>>1)|0x20}；7bit 窗 =
    // (prev<<1|b)&0x7F —— prev 的 bit5 落入窗 bit6（多项式抽头），
    // 故两入边输出不同、分支度量独立。
    for (unsigned ns = 0; ns < 64; ++ns) {
        const unsigned b = ns & 1u;
        const unsigned base = ns >> 1;
        const unsigned w0 = ((base << 1) | b) & 0x7Fu;
        const unsigned w1 = (w0 | 0x40u);   // 前驱 bit5=1
        const auto o0 = enc_out(w0);
        const auto o1 = enc_out(w1);
        const float c0 = pm_[base] + (soft_g1 * float(o0.first) +
                                      soft_g2 * float(o0.second));
        const float c1 = pm_[base | 0x20u] +
                         (soft_g1 * float(o1.first) + soft_g2 * float(o1.second));
        pm_next_[ns] = std::max(c0, c1);
        hist_[hist_pos_][ns] = uint8_t(c0 >= c1 ? 0u : 1u);
    }
    std::swap(pm_, pm_next_);

    float mn = pm_[0];
    for (float v : pm_) mn = std::min(mn, v);
    for (float& v : pm_) v -= mn;

    unsigned best_s = 0;
    float best_m = pm_[0];
    for (unsigned s = 1; s < 64; ++s) {
        if (pm_[s] > best_m) {
            best_m = pm_[s];
            best_s = s;
        }
    }
    unsigned s = best_s;
    std::size_t p = hist_pos_;
    for (int back = 0; back < 72; ++back) {
        const unsigned choice = hist_[p][s];
        s = (s >> 1) | (choice ? 0x20u : 0x00u);
        p = (p + 72 - 1) % 72;
    }
    hist_pos_ = (hist_pos_ + 1) % 72;
    ++depth_;
    if (depth_ <= 72) return 0;   // 填充期：调用方对齐后丢弃
    return int(s & 1u);
}

float ViterbiDecoder::metric_spread() const noexcept {
    float mn = pm_[0], mx = pm_[0];
    for (float v : pm_) {
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    return mx - mn;
}

void ViterbiDecoder::reset_stats() noexcept { errors_ = 0; }

} // namespace hackrftool::dsp
