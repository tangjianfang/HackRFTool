// FM 广播接收链（#53 收音机）：int8 IQ → FIR 抽取 250 kHz → 正交鉴频 →
// 19 kHz 导频 PLL → 38 kHz DSBSC 立体声矩阵 → 50 µs 去加重 → 48 kHz 音频。
// 纯 C++（无 UI/硬件依赖），dsp_test 以合成信号直测。
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hackrftool::dsp {

// 鉴频器：相邻样本共轭相乘取幅角，归一化 (-1,1]（×fs/2 即 Hz）
[[nodiscard]] float fm_discriminator(std::complex<float> cur,
                                     std::complex<float> prev) noexcept;

// AFC 自动频率微调（#55e）：给定频谱 bins（覆盖中心 ±bw/2），若最强峰
// 显著（峰均差≥min_prom）且偏移在 [min_off, max_off]，返回向峰方向的
// 修正量（MHz，含符号）；无需修正返回 0。纯函数可单测。
[[nodiscard]] double afc_correction(const std::vector<float>& db,
                                    double bw_mhz, float min_prom_db,
                                    double min_off_mhz,
                                    double max_off_mhz) noexcept;

// 频谱点击峰值吸附（#55f）：在点击频率 ±search_mhz 内找最强显著峰的
// 精确偏移（替代按像素换算——手点必偏，吸附后即台中心）；无显著峰
// 返回 click_off_mhz 原值
[[nodiscard]] double peak_snap(const std::vector<float>& db, double bw_mhz,
                               double click_off_mhz, double search_mhz,
                               float min_prom_db) noexcept;

// hamming 窗 sinc 低通抽取器：fs_in → fs_in/L（L 定死输出 250 kHz）
class Decimator {
public:
    // fc_hz：信道低通截止（默认 ±120k 广播；窄带 50k 供卫星/窄 FM）
    Decimator(double fs_in_hz, std::size_t taps, double fc_hz = 120e3);

    // 逐样本喂入；每 L 个输入产出一个输出（无则为空）
    [[nodiscard]] std::optional<std::complex<float>> push(std::complex<float> s);

    [[nodiscard]] std::size_t decim() const noexcept { return l_; }

private:
    std::vector<float> taps_;
    std::vector<std::complex<float>> hist_;   // 环形历史（长度 taps）
    std::size_t l_ = 1;
    std::size_t pos_ = 0;   // 输入计数 % L
    std::size_t head_ = 0;  // 历史写头
};

// FM 广播接收机（立体声，导频丢失自动回退单声道）
class FmReceiver {
public:
    static constexpr double kMpxHz = 250e3;
    static constexpr double kAudioHz = 48000.0;

    using AudioCb = void (*)(const float* l, const float* r, std::size_t n,
                             void* ctx);

    // pll_bw_hz：导频环路带宽（收听 80 Hz 稳；测试用 300 Hz 加速锁定）
    // force_mono：诊断/弱信号模式——禁用 38k DSBSC 分支（噪声减半）
    // bw_hz：信道滤波截止（±120k 广播 / ±80k / ±50k 窄带）
    explicit FmReceiver(double fs_in_hz, double pll_bw_hz = 80.0,
                        bool force_mono = false, double bw_hz = 120e3);

    // 运行中切带宽（重建抽取滤波器，状态复位可接受）
    void set_bandwidth(double bw_hz);

    void set_force_mono(bool m) noexcept { force_mono_ = m; }

    void set_audio_callback(AudioCb cb, void* ctx) noexcept { cb_ = cb; ctx_ = ctx; }

    [[nodiscard]] double bandwidth_hz() const noexcept { return bw_hz_; }

    // 任意长度 int8 IQ（libusb 回调直喂）
    void feed(const std::int8_t* iq, std::size_t bytes);

    [[nodiscard]] bool stereo_locked() const noexcept { return pilot_ > kPilotLock; }
    [[nodiscard]] float pilot_level() const noexcept { return pilot_; }
    [[nodiscard]] float audio_peak() const noexcept;   // 表头电平（读取自衰减）
    [[nodiscard]] float signal_level() const noexcept { return sig_; }

private:
    void mpx_step(float mpx);

    Decimator dec_;
    double fs_in_hz_ = 2e6;
    double bw_hz_ = 120e3;
    // 鉴频状态
    std::complex<float> prev_{1.0f, 0.0f};
    bool have_prev_ = false;
    float sig_ = 0.0f;   // 鉴频输入幅度均值（|IQ|）
    // L+R / L−R 低通（15 kHz @250k）
    std::vector<float> lp_taps_;
    std::vector<float> lpr_hist_, lmr_hist_;
    std::size_t lpr_head_ = 0, lmr_head_ = 0;
    // 导频 PLL
    double pll_bw_;
    bool force_mono_ = false;
    double theta_ = 0.0;    // 19 kHz NCO 相位
    double freq_ = 0.0;     // 环路频偏估计（Hz）
    float pilot_ = 0.0f;    // 导频相关幅度（LPF 后）
    static constexpr float kPilotLock = 0.02f;
    // 去加重 50 µs（音频域，每声道）
    float de_l_ = 0.0f, de_r_ = 0.0f;
    float de_alpha_ = 0.08f;   // 1/(fs_mpx*50µs)
    // 250k → 48k 线性重采样（15k 低通后无 >24k 分量）
    double res_pos_ = 0.0;
    float last_l_ = 0.0f, last_r_ = 0.0f;
    // 音频块缓冲（480 样本 = 10 ms）
    std::vector<float> abuf_l_, abuf_r_;
    AudioCb cb_ = nullptr;
    void* ctx_ = nullptr;
    float peak_ = 0.0f;
};

} // namespace hackrftool::dsp
