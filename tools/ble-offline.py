# BLE 离线解调管线（#111 Phase B）：IQ → 突发检测 → 多旋钮解调 → CRC 判定
# 判定门：某旋钮组合下 AA 锁定突发的 CRC24 通过率 ≥15% → 立项 C++ 移植
# 用法: python tools/ble-offline.py <file.cs8> [fs_msps=20] [ch=39]
import sys
import numpy as np

def seq_bits(seed, n, pre_shift=1):
    reg = seed & 0x7F
    for _ in range(pre_shift):
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
    out = []
    for _ in range(n):
        o = reg & 1
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
        out.append(o)
    return out

def crc24_bits(body_bits):
    crc = 0x555555
    for i in range(0, len(body_bits) - 7, 8):
        byte = sum(body_bits[i + k] << k for k in range(8))
        for k in range(8):
            bit = (byte >> k) & 1
            fb = (crc & 1) ^ bit
            crc >>= 1
            if fb:
                crc ^= 0x00065B
    return crc & 0xFFFFFF

def dewhiten_bytes(raw_bits, off, nbytes, ch):
    key = seq_bits(ch, nbytes * 8)
    frame = []
    for b in range(nbytes):
        v = sum(raw_bits[off + b * 8 + t] << t for t in range(8))
        m = sum(key[b * 8 + t] << t for t in range(8))
        frame.append(v ^ m)
    return frame

# 期望 40bit：前导 0xAA + AA D6 BE 89 8E（LSB-first，1=正频偏——#109 实证极性）
EXP = []
for byte in (0xAA, 0xD6, 0xBE, 0x89, 0x8E):
    EXP += [(byte >> k) & 1 for k in range(8)]
EXP = np.array(EXP, dtype=np.int8)

def find_bursts(mag, fs, thr_db=10.0, min_gap_s=8e-6, min_len=128):
    # 自适应底噪 + dB 门限（与 C++ burst_detector 同思路）
    floor = max(np.median(mag), 1e-6)
    thr = floor * (10 ** (thr_db / 20))
    hit = mag > thr
    # 滑窗平滑（64 样本）消毛刺——float 卷积，75% 占空才认
    k = np.ones(64, dtype=np.float32) / 64
    hit = np.convolve(hit.astype(np.float32), k, "same") > 0.75
    idx = np.flatnonzero(np.diff(hit.astype(np.int8)) == 1) + 1
    out = []
    gap = int(min_gap_s * fs)
    for s in idx:
        if out and s - out[-1][1] < gap:
            out[-1] = (out[-1][0], s)
            continue
        out.append((s, s))
    # 找每段终点
    res = []
    for s, _ in out:
        e = s
        while e < len(hit) and hit[e]:
            e += 1
        if e - s >= min_len:
            res.append((max(0, s - 200), min(len(mag), e + 200)))  # 前后留 12.5µs
    return res

def slice_burst(f, sps, phase, weight_mode, cfo_hz, fs):
    """按相位切符号窗，返回每符号加权均值（Hz）与比特（向量化）。weight_mode:
    full=整窗（复现 C++ 现状）/ center=中央 50% / gauss=高斯权"""
    n_sym = (len(f) - phase) // sps
    if n_sym < 48:
        return None, None
    if weight_mode == "full":
        w = np.ones(sps)
    elif weight_mode == "center":
        w = np.zeros(sps)
        w[sps // 4: sps - sps // 4] = 1
    else:
        t = (np.arange(sps) - (sps - 1) / 2) / sps
        w = np.exp(-0.5 * (t / 0.35) ** 2)
    m = f[phase: phase + n_sym * sps].reshape(n_sym, sps) - cfo_hz
    means = m @ (w / w.sum())
    return means, (means > 0).astype(np.uint8)

def search_aa(bits, max_err=2):
    """AA 搜索（相关法向量化）：e = (40−corr)/2，返回 (pos, err)"""
    b = bits.astype(np.int8) * 2 - 1
    ref = (EXP * 2 - 1)[::-1]
    corr = np.convolve(b, ref, "valid")
    p = int(np.argmax(corr))
    return p, (40 - int(corr[p])) // 2

def try_frame(bits, pos, ch, detail=False):
    """AA 后链路层（头/CRC/AD）——返回 dict 或 None"""
    pdu = pos + 40
    if pdu + 8 * 11 > len(bits):
        return None
    h = dewhiten_bytes(bits, pdu, 2, ch)
    t, ln = h[0] & 0x0F, h[1] & 0x3F
    if t not in (0, 1, 2, 4, 6) or not (6 <= ln <= 37):
        return None
    nb = 2 + ln + 3
    if pdu + nb * 8 > len(bits):
        return None
    frame = dewhiten_bytes(bits, pdu, nb, ch)
    body = []
    for b in frame[: 2 + ln]:
        body += [(b >> k) & 1 for k in range(8)]
    calc = crc24_bits(body)
    rx = (frame[2 + ln] | (frame[2 + ln + 1] << 8) | (frame[2 + ln + 2] << 16))
    r = {"type": t, "len": ln, "crc_ok": calc == rx,
         "dist": bin(calc ^ rx).count("1")}
    if detail:
        adv_off = 6 if t in (3, 5) else 0
        r["adv_a"] = "%02X%02X%02X%02X%02X%02X" % tuple(
            frame[2 + adv_off: 2 + adv_off + 6])
        # AD 链走通深度（type 0/2/4：AdvA 后）
        if t in (0, 2, 4):
            i, ok = 0, 0
            pay = frame[8: 2 + ln]
            while i + 1 < len(pay):
                L = pay[i]
                if L == 0 or i + 1 + L > len(pay) or pay[i + 1] > 0x2B:
                    break
                i += 1 + L
                ok = i
            r["ad_ok"] = ok
            r["name"] = "".join(chr(c) if 32 <= c < 127 else "."
                                for c in frame[10: 10 + 12])
    return r

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "ble39.cs8"
    fs = float(sys.argv[2] if len(sys.argv) > 2 else 20) * 1e6
    ch = int(sys.argv[3] if len(sys.argv) > 3 else 39)
    sps = int(round(fs / 1e6))
    dev = 250e3
    raw = np.fromfile(path, dtype=np.int8)
    iq = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32))
    print(f"IQ {len(iq)/fs:.1f}s @ {fs/1e6:.0f}Msps sps={sps} ch={ch}")
    mag = np.abs(iq)
    bursts = find_bursts(mag, fs)
    print(f"突发 {len(bursts)} 个")
    # 频率鉴别（全段一次算好，burst 切片）；算完释放 IQ 省内存
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    del raw, iq
    stats = {}
    for mode in ("full", "center", "gauss"):
        stats[mode] = {"aa": 0, "hdr": 0, "crc": 0, "aa_err": [],
                       "cfo_hz": [], "passed": set()}
    aa_locked = []   # (burst_idx, phase, pos)——相位/位置由基线定死，各变体共用
    peak_db = []
    for bi, (s, e) in enumerate(bursts):
        fb = f[s:min(e, len(f))]
        if len(fb) < sps * 48:
            continue
        peak_db.append(20 * np.log10(np.percentile(mag[s:e], 99) + 1e-9))
        # 基线：全相位×全窗找 AA（复现 C++ 16 相位选择——sps 相位）
        best = (1 << 30, -1, -1)
        for ph in range(sps):
            _, bits = slice_burst(fb, sps, ph, "full", 0, fs)
            if bits is None:
                continue
            pos, err = search_aa(bits, 2)
            if err < best[0]:
                best = (err, ph, pos)
        if best[0] > 2:
            continue
        _, ph, pos = best
        aa_locked.append((bi, ph, pos))
        # —— 各变体共用 (bi, ph, pos)，测纯修复效果 ——
        # CFO 估计：先从 AA 40 符号自估有效偏差（高斯成形下全窗均值≈0.75×dev，
        # 硬扣 250k 会把比特图案泄漏进 CFO），再按期望图案减出频偏。
        # 注意 EXP 是 0/1 比特——CFO 式用双极性符号（0/1 直接当符号会把
        # 0 比特算成零频偏，估计全歪）
        EXP_SGN = np.where(EXP > 0, 1.0, -1.0)
        means, _ = slice_burst(fb, sps, ph, "full", 0, fs)
        if means is not None and pos + 40 <= len(means):
            seg = means[pos: pos + 40]
            pos_means = seg[EXP_SGN > 0]
            neg_means = seg[EXP_SGN < 0]
            if len(pos_means) >= 4 and len(neg_means) >= 4:
                dev_eff = 0.5 * (np.mean(pos_means) - np.mean(neg_means))
                cfo = float(np.mean(seg - EXP_SGN * dev_eff))
            else:
                cfo = 0.0
        else:
            cfo = 0.0
        for mode in ("full", "center", "gauss"):
            st = stats[mode]
            st["aa"] += 1
            # 无 CFO（现状等效）与 +CFO 各试一次——帧级去重（passed 集合）
            _, bits0 = slice_burst(fb, sps, ph, mode, 0, fs)
            r0 = try_frame(bits0, pos, ch) if bits0 is not None else None
            _, bits1 = slice_burst(fb, sps, ph, mode, cfo, fs)
            r1 = try_frame(bits1, pos, ch) if bits1 is not None else None
            for r in (r0, r1):
                if r:
                    st["hdr"] += 1
                    if r["crc_ok"]:
                        st["passed"].add(bi)
            if mode == "full":
                st["cfo_hz"].append(cfo)
                if bits1 is not None:
                    e1 = int(np.count_nonzero(bits1[pos: pos + 40] != EXP))
                    st["aa_err"].append(e1)
    print(f"AA 锁定 {len(aa_locked)}/{len(bursts)} 突发"
          f"（峰值 dB p50={np.percentile(peak_db,50) if peak_db else 0:.0f}）")
    if aa_locked:
        cf = stats["full"]["cfo_hz"]
        print(f"CFO 估计(Hz): {['%.0f' % c for c in cf[:8]]} ...")
        print(f"CFO p50={np.percentile(cf,50)/1e3:.1f}kHz "
              f"min={min(cf)/1e3:.1f} max={max(cf)/1e3:.1f}")
        print(f"去 CFO 后 AA 区误码@固定相位: mean={np.mean(stats['full']['aa_err']):.2f}bit/40")
    print(f"{'变体':<8}{'头规范次':<8}{'CRC帧':<8}{'判定(≥15% 立项)'}")
    for mode in ("full", "center", "gauss"):
        st = stats[mode]
        n = max(len(aa_locked), 1)
        rate = len(st["passed"]) / n * 100
        gate = "✓ 立项" if rate >= 15 else ("△ 边缘" if rate >= 5 else "✗")
        print(f"{mode:<8}{st['hdr']:<8}{len(st['passed']):<8}"
              f"{len(st['passed'])}/{len(aa_locked)} = {rate:.0f}%  {gate}")
    # 帧级诊断（full+CFO 变体）：AdvA/AD 中断点/CRC 距离——错误定位
    print("== 帧诊断（full+CFO）：")
    for bi, ph, pos in aa_locked:
        s, e = bursts[bi]
        fb = f[s:min(e, len(f))]
        _, bits = slice_burst(fb, sps, ph, "full", 0, fs)
        if bits is None:
            continue
        # 用同一 CFO 估计（此处只重算 full 变体展示）
        r = None
        m0, b0 = slice_burst(fb, sps, ph, "full", 0, fs)
        # CFO 复算（与主循环同式）
        seg = m0[pos:pos + 40]
        sg = np.where(EXP > 0, 1.0, -1.0)
        de = 0.5 * (np.mean(seg[sg > 0]) - np.mean(seg[sg < 0]))
        cf = float(np.mean(seg - sg * de))
        _, bits1 = slice_burst(fb, sps, ph, "full", cf, fs)
        rr = try_frame(bits1 if bits1 is not None else b0, pos, ch, detail=True)
        if rr:
            print(f"  t={rr['type']} len={rr['len']:<3} dist={rr['dist']:<3} "
                  f"AdvA={rr.get('adv_a','?')} ad_ok={rr.get('ad_ok','?'):>2} "
                  f"name={rr.get('name','')}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
