// HackRFTool M1 —— 频谱检测工具（骨架）
#include <windows.h>

#include <flux/flux.hpp>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    flux::enable_per_monitor_dpi_v2();
    flux::Host host;
    host.set_root_builder([] { return flux::label(L"HackRFTool M1"); });
    flux::Host::Config cfg;
    cfg.title = L"HackRFTool";
    cfg.width = 1200;
    cfg.height = 800;
    if (!host.create(cfg, instance)) return 1;
    return host.run();
}
