# 匹配滤波解调验证（#111 Phase B 决战）：高斯脉冲模板相关替代矩形整窗
# —— 抑制 GFSK 跳变沿 ISI；#109 白化约定（seed=ch）+ CRC 判定
# 用法: python tools/ble-matched.py <file.cs8> [fs_msps=20] [ch=39]
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

def matched_bits(f, sps, phase, sigma_s=0.39):
    """高斯模板相关：bit_k = sign(Σ f·g)，模板支撑 ±1 符号"""
    n_sym = (len(f) - phase) // sps - 2
    if n_sym < 48:
        return None, None
    g = np.exp(-0.5 * ((np.arange(-sps, sps + 1)) / (sigma_s * sps)) ** 2)
    g /= g.sum()
    means = np.empty(n_sym)
    for k in range(n_sym):
        st = phase + k * sps
        means[k] = np.dot(f[st: st + 2 * sps + 1], g)
    return means, (means > 0).astype(np.uint8)

def main():
    path = sys.argv[1]
    fs = float(sys.argv[2]) * 1e6
    ch = int(sys.argv[3])
    sps = int(round(fs / 1e6))
    raw = np.fromfile(path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    del raw, iq
    locked = 0
    crc_ok = 0
    details = []
    for s, e in bursts:
        fb = f[s:min(e, len(f))]
        if len(fb) < sps * 60:
            continue
        best = (999, -1, -1)
        for ph in range(sps):
            _, bits = matched_bits(fb, sps, ph)
            if bits is None:
                continue
            pos, err = bo.search_aa(bits, 2)
            if err < best[0]:
                best = (err, ph, pos)
        if best[0] > 2:
            continue
        _, ph, pos = best
        _, bits = matched_bits(fb, sps, ph)
        if bits is None:
            continue
        locked += 1
        # #109 约定（ble-offline.try_frame 内建）：seed=ch
        bits_l = list(map(int, bits))
        r = bo.try_frame(bits_l, pos, ch, detail=True)
        if r:
            if r["crc_ok"]:
                crc_ok += 1
                if len(details) < 6:
                    details.append(f"  ✓ type={r['type']} len={r['len']} "
                                   f"AdvA={r.get('adv_a')} "
                                   f"name={r.get('name','')[:16]}")
        else:
            # 头都不规范——统计
            pass
    print(f"匹配滤波: AA 锁定 {locked}, CRC 通过 {crc_ok}"
          f"（{crc_ok/max(locked,1)*100:.0f}%）")
    for d in details:
        print(d)
    return 0

if __name__ == "__main__":
    sys.exit(main())
