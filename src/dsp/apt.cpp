#include "dsp/apt.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fft.hpp"

namespace hackrftool::dsp {

void AptDecoder::envelope_step(float x) {
    // 2.4 kHz I/Q 同步检波（幅度与相位无关，无需锁相）
    const double s = std::sin(nco_phase_);
    const double c = std::cos(nco_phase_);
    nco_phase_ += 2.0 * kPi * 2400.0 / kAudioHz;
    if (nco_phase_ > 2.0 * kPi) nco_phase_ -= 2.0 * kPi;
    const float a = 0.25f;   // 单极点 ~2 kHz（通过 ±2 kHz 调幅边带）
    i_lpf_ += a * (float(x * s) - i_lpf_);
    q_lpf_ += a * (float(x * c) - q_lpf_);
    const float env = std::sqrt(i_lpf_ * i_lpf_ + q_lpf_ * q_lpf_);
    // 慢速 AGC 峰值跟踪（0.999 衰减 ~2 s）
    env_max_ = std::max(env, env_max_ * 0.99992f);
    const float norm = env_max_ > 1e-6f ? env / env_max_ : 0.0f;
    sync_step(norm);
    emit_pixel(norm);
}

void AptDecoder::sync_step(float env) {
    // 1040 Hz 正交检测：同步串 = 包络被 1040 Hz 方波调制；图像区包络平稳
    // → 1040 Hz 分量幅度只在同步串处高（延迟相关法方向反——图像平坦区
    // 相关积反而更大，已被实测证伪）
    sq_phase_ += 2.0 * kPi * 1040.0 / kAudioHz;
    if (sq_phase_ > 2.0 * kPi) sq_phase_ -= 2.0 * kPi;
    const float a = 0.008f;   // ~380 Hz 检测带宽
    sq_i_ += a * (float(env * std::sin(sq_phase_)) - sq_i_);
    sq_q_ += a * (float(env * std::cos(sq_phase_)) - sq_q_);
    sync_sm_ = std::sqrt(sq_i_ * sq_i_ + sq_q_ * sq_q_);
    // 基准：极慢衰减（行周期 0.5s 内不显著）+ 绝对下限滤除图像跳变沿伪峰
    sync_max_ = std::max(sync_sm_, sync_max_ * 0.999998f);
    const float thr = std::max(sync_max_ * 0.6f, 0.10f);

    ++since_peak_;
    if (expect_next_ >= 0 && since_peak_ < 21000) return;   // 不应期（行率先验）
    const bool over = sync_sm_ >= thr && thr > 1e-4f;
    if (!over) {
        in_peak_ = false;
        return;
    }
    if (in_peak_) return;   // 同步串内持续超阈值：只取进入沿为一次峰
    in_peak_ = true;
    {
        const long dist = long(since_peak_);
        if (expect_next_ < 0) {
            // 未锁定：记录峰，等下一个验证周期
            expect_next_ = dist;
            since_peak_ = 0;
        } else {
            const long expect = expect_next_;
            const bool periodic = dist > expect - 1500 && dist < expect + 1500;
            const bool line_rate = dist > long(kLineSamples) - 1500 &&
                                   dist < long(kLineSamples) + 1500;
            if (line_rate) {
                if (lock_hits_ < 3) ++lock_hits_;
                if (lock_hits_ >= 2) synced_.store(true);
                // 行接近满（≥1900 px）：峰比像素预算早到 ~190 样本（补偿后
                // 亏空），用当前均值补齐尾部立即出行——尾部是 telemetry/
                // 间隔区，图像主体不受影响
                if (line_pos_ >= 1900 && line_pos_ < kWidth) {
                    const float fill = px_cnt_ > 0 ? px_acc_ / float(px_cnt_)
                                                   : lo_ + 0.5f * (hi_ - lo_);
                    const float span = std::max(hi_ - lo_, 1e-3f);
                    while (line_pos_ < kWidth)
                        line_[line_pos_++] = std::uint8_t(
                            std::clamp((fill - lo_) / span, 0.0f, 1.0f) * 255.0f +
                            0.5f);
                    {
                        std::lock_guard<std::mutex> g(mtx_);
                        img_.push_line(line_);
                    }
                }
                line_pos_ = 0;
                // 重对齐：峰沿比真实行首晚约 236 样本（检测链群延迟 +
                // 阈值穿越滞后，合成信号校准）——负偏移空跑补偿，期间样本
                // 仍积累进首像素（同步串即行首内容）
                px_pos_ = -236.0;
                px_acc_ = 0;
                px_cnt_ = 0;
                expect_next_ = long(kLineSamples);
                since_peak_ = 0;
            } else if (periodic) {
                expect_next_ = dist;
                since_peak_ = 0;
            } else {
                // 周期崩坏 → 失锁重来
                lock_hits_ = 0;
                synced_.store(false);
                expect_next_ = -1;
                since_peak_ = 0;
            }
        }
    }
}

void AptDecoder::emit_pixel(float env) {
    // 动态范围跟踪（自动对比度）
    lo_ = std::min(lo_, env);
    hi_ = std::max(hi_, env);
    px_acc_ += env;
    ++px_cnt_;
    px_pos_ += 1.0;
    if (px_pos_ >= px_budget_) {
        px_pos_ -= px_budget_;
        const float mean = px_cnt_ > 0 ? px_acc_ / float(px_cnt_) : 0.0f;
        px_acc_ = 0;
        px_cnt_ = 0;
        if (line_pos_ < kWidth) {
            const float span = std::max(hi_ - lo_, 1e-3f);
            const float v = (mean - lo_) / span;
            line_[line_pos_++] =
                std::uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        if (line_pos_ >= kWidth) {
            std::lock_guard<std::mutex> g(mtx_);
            img_.push_line(line_);
            line_pos_ = 0;   // 未同步时继续自由跑行（滚动窗口仍可见）
        }
    }
}

void AptDecoder::feed(const float* audio, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) envelope_step(audio[i]);
}

bool AptDecoder::synced() const { return synced_.load(); }

std::uint64_t AptDecoder::lines() const {
    std::lock_guard<std::mutex> g(mtx_);
    return img_.total_lines;
}

void AptDecoder::snapshot(AptImage& out) {
    std::lock_guard<std::mutex> g(mtx_);
    out = img_;
}

} // namespace hackrftool::dsp
