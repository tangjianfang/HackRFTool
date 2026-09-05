#include "dsp/fm.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fft.hpp"

namespace hackrftool::dsp {

float fm_discriminator(std::complex<float> cur, std::complex<float> prev) noexcept {
    const std::complex<float> d = cur * std::conj(prev);
    return std::atan2(d.imag(), d.real()) * float(1.0 / kPi);
}

double afc_correction(const std::vector<float>& db, double bw_mhz,
                      float min_prom_db, double min_off_mhz,
                      double max_off_mhz) noexcept {
    if (db.size() < 8) return 0.0;
    float sum = 0.0f;
    for (const float v : db) sum += v;
    const float avg = sum / float(db.size());
    // 只在允许吸附窗口（|off|≤max_off）内找最强峰——全局最强若在窗外
    // 会永久挡住窗口内的可修正峰（真机 98.0：窗外 98.6 强台遮住窗内
    // 98.3，AFC 永不触发，dump 实测中心始终空频点）
    const double frac = max_off_mhz / bw_mhz;
    const long lo = long(double(db.size()) * (0.5 - frac));
    const long hi = long(double(db.size()) * (0.5 + frac));
    std::size_t best = 0;
    bool found = false;
    for (long i = std::max(lo, 0L); i < std::min(hi, long(db.size())); ++i) {
        if (!found || db[size_t(i)] > db[best]) {
            best = size_t(i);
            found = true;
        }
    }
    if (!found) return 0.0;
    if (db[best] - avg < min_prom_db) return 0.0;
    const double t =
        (double(best) + 0.5) / double(db.size()) - 0.5;   // -0.5..0.5
    const double off = t * bw_mhz;
    if (std::abs(off) < min_off_mhz || std::abs(off) > max_off_mhz) return 0.0;
    return off;
}

namespace {

// hamming 窗 sinc 低通系数（截止 fc，采样 fs，系数和归一为 1）
std::vector<float> lowpass_taps(std::size_t taps, double fs, double fc) {
    std::vector<float> h(taps);
    const std::size_t m = taps - 1;
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double x = double(i) - m / 2.0;
        const double sinc = std::abs(x) < 1e-9
                                ? 1.0
                                : std::sin(kPi * 2.0 * fc * x / fs) /
                                      (kPi * 2.0 * fc * x / fs);
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * double(i) / double(m));
        h[i] = float(sinc * w);
        sum += h[i];
    }
    for (auto& t : h) t = float(t / sum);
    return h;
}

} // namespace

Decimator::Decimator(double fs_in_hz, std::size_t taps)
    : taps_(lowpass_taps(taps, fs_in_hz, 120e3)) {   // FM 信道 ±120 kHz
    l_ = std::max<std::size_t>(std::size_t(fs_in_hz / 250e3 + 0.5), 1);
    hist_.assign(taps_.size(), {0.0f, 0.0f});
}

std::optional<std::complex<float>> Decimator::push(std::complex<float> s) {
    hist_[head_] = s;
    head_ = (head_ + 1) % hist_.size();
    if (++pos_ < l_) return std::nullopt;
    pos_ = 0;
    // 卷积（历史环形缓冲，最新在 head_-1）
    std::complex<float> acc{0.0f, 0.0f};
    std::size_t idx = head_;
    for (const float t : taps_) {
        idx = (idx + hist_.size() - 1) % hist_.size();
        acc += hist_[idx] * t;
    }
    return acc;
}

FmReceiver::FmReceiver(double fs_in_hz, double pll_bw_hz, bool force_mono)
    : dec_(fs_in_hz, 128), pll_bw_(pll_bw_hz), force_mono_(force_mono) {
    lp_taps_ = lowpass_taps(48, kMpxHz, 15e3);
    lpr_hist_.assign(lp_taps_.size(), 0.0f);
    lmr_hist_.assign(lp_taps_.size(), 0.0f);
    de_alpha_ = float(1.0 / (kMpxHz * 50e-6));
    de_alpha_ = std::min(de_alpha_, 0.5f);
    abuf_l_.reserve(480);
    abuf_r_.reserve(480);
}

namespace {
// 环形历史 FIR 推进一个样本，返回输出
float fir_push(std::vector<float>& hist, std::size_t& head,
               const std::vector<float>& taps, float x) {
    hist[head] = x;
    head = (head + 1) % hist.size();
    float acc = 0.0f;
    std::size_t idx = head;
    for (const float t : taps) {
        idx = (idx + hist.size() - 1) % hist.size();
        acc += hist[idx] * t;
    }
    return acc;
}
} // namespace

void FmReceiver::mpx_step(float mpx) {
    // ---- 导频 PLL（19 kHz，二阶环；cos 型 PD → 同相锁定）----
    const double dt = 1.0 / kMpxHz;
    // mpx=A·sin(ωt)，err=mpx·cosθ 均值 = A/2·sin(φp)（φp=导频相位−θ）→ 零点同相
    const double err = double(mpx) * std::cos(theta_);
    const double wn = 2.0 * kPi * pll_bw_;
    const double kp = 2.0 * 0.7 * wn;   // ζ=0.7
    const double ki = wn * wn;
    const double a_est = std::max(double(pilot_), 0.02);   // 导频幅度估计
    freq_ += ki * err * dt / a_est;
    freq_ = std::clamp(freq_, -500.0, 500.0);
    theta_ += (2.0 * kPi * 19e3 + kp * err / a_est + 2.0 * kPi * freq_) * dt;
    if (theta_ > 2.0 * kPi) theta_ -= 2.0 * kPi;
    if (theta_ < 0.0) theta_ += 2.0 * kPi;
    // 导频相关幅度（θ 同相锁定 → sinθ 与导频同相，乘积均值 = A/2）
    const float corr = float(double(mpx) * std::sin(theta_));
    pilot_ += (corr - pilot_) * 0.0005f;   // ~8 Hz 等效带宽

    // ---- L+R / L−R（38 kHz 相干解调；2θ 免 π 模糊，sin 族与导频同源）----
    const float lpr = fir_push(lpr_hist_, lpr_head_, lp_taps_, mpx);
    const float dsc = float(2.0 * std::sin(2.0 * theta_)) * mpx;
    const float lmr = fir_push(lmr_hist_, lmr_head_, lp_taps_, dsc);

    // ---- 立体声矩阵 + 去加重 ----
    float l, r;
    if (stereo_locked() && !force_mono_) {
        l = 0.5f * (lpr + lmr);
        r = 0.5f * (lpr - lmr);
    } else {
        l = r = lpr;
    }
    de_l_ += de_alpha_ * (l - de_l_);
    de_r_ += de_alpha_ * (r - de_r_);

    // ---- 250k → 48k 线性重采样（每输入样本产出 48/250≈0.192 个）----
    res_pos_ += kAudioHz / kMpxHz;
    while (res_pos_ >= 1.0) {
        // 线性插值：last（上一输入）与当前输入之间 —— last 保存的是上一
        // 去加重样本；此处产出的每个音频点用 last 与当前样本插值
        const double frac = res_pos_ - 1.0;
        const float al = last_l_ + float(frac) * (de_l_ - last_l_);
        const float ar = last_r_ + float(frac) * (de_r_ - last_r_);
        abuf_l_.push_back(std::clamp(al, -1.0f, 1.0f));
        abuf_r_.push_back(std::clamp(ar, -1.0f, 1.0f));
        // 表头电平：峰值保持 + 慢衰减（~0.1s 回落）
        peak_ *= 0.9998f;
        peak_ = std::max(peak_, std::abs(al));
        peak_ = std::max(peak_, std::abs(ar));
        res_pos_ -= 1.0;
        if (abuf_l_.size() >= 480) {
            if (cb_ != nullptr) cb_(abuf_l_.data(), abuf_r_.data(), 480, ctx_);
            abuf_l_.clear();
            abuf_r_.clear();
        }
    }
    last_l_ = de_l_;
    last_r_ = de_r_;
}

float FmReceiver::audio_peak() const noexcept { return peak_; }

void FmReceiver::feed(const std::int8_t* iq, std::size_t bytes) {
    const std::size_t n = bytes / 2;
    for (std::size_t i = 0; i < n; ++i) {
        const std::complex<float> s{
            float(iq[i * 2]) / 127.0f, float(iq[i * 2 + 1]) / 127.0f};
        if (const auto d = dec_.push(s)) {
            sig_ += (std::abs(*d) - sig_) * 0.001f;
            if (have_prev_) {
                // 鉴频 → 归一化 MPX（±1 = ±75 kHz 频偏）
                const float dev = fm_discriminator(*d, prev_) *
                                  float(125e3 / 75e3);   // (fs/2)/dev_max
                mpx_step(dev);
            }
            prev_ = *d;
            have_prev_ = true;
        }
    }
}

} // namespace hackrftool::dsp
