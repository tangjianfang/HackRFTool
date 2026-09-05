// UI 设置持久化（#57）：所有界面参数（页签/频率/增益/带宽/筛选/音量…）
// 序列化到 exe 旁 settings.tsv，交互启动时恢复，命令行自测模式不加载
// （selftest 断言依赖出厂默认）。纯数据结构+文本编解码，dsp_test 直测。
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace hackrftool::app {

struct Settings {
    int page = 0;                    // 0=频谱 1=监测 2=抓包 3=收音 4=云图
    double center_mhz = 2450.0;      // 频谱/监测/抓包中心
    double radio_mhz = 98.0;         // 收音机页频率
    int rate_index = 4;              // kRatesMsps 下标
    unsigned lna = 16;               // 8..40 步进 8
    unsigned vga = 16;               // 2..62 步进 2
    bool amp = false;                // 板载功放
    bool auto_track = true;          // 监测自动跟踪最强
    bool afc_on = true;              // 收音页自动频率微调
    bool stereo_opt = true;          // 立体声解码（不勾=强制单声）
    double threshold = -70.0;        // 活动阈值 dB
    double burst_thr = -40.0;        // 突发阈值 dB
    int symrate_idx = 0;             // 0=1Mbps 1=2Mbps
    int fm_bw = 0;                   // 0=±120k 1=±80k 2=±50k
    int audio_dev = -1;              // WaveOut 设备下标（-1=默认）
    int vol = 80;                    // 音量 0..100
    int sig_cat = 0;                 // 信号库筛选：0=电台 1=卫星 2=ISM 3=全部
    int sig_online = 1;              // 1=仅在线
    int sig_sort = 0;                // 0=强度 1=频率
};

// TSV：每行 "key<TAB>value"，# 开头为注释。未知键忽略（前向兼容），
// 非法值跳过该键保留默认。
[[nodiscard]] std::string serialize(const Settings& s);

// 解析整文件内容；任何行都容错（损坏一行不丢全部设置）。
[[nodiscard]] std::optional<Settings> deserialize(std::string_view text);

} // namespace hackrftool::app
