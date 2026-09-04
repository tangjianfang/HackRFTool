// 纯 C++ DSP 单元测试 —— 沿用 WinFlux 的裸 main + check 模式，无测试框架
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    check(true, "自检");   // 骨架自检（Task 2 起加入真实用例）
    if (failures == 0) std::printf("HackRFToolTest: 全部通过\n");
    return failures == 0 ? 0 : 1;
}
