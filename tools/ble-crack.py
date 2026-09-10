# 终局（#111）：从已知明文掩码拟合白化 LFSR → 推导完整约定 → 全帧 CRC 验证
# 依据：ble-truth 共识命中 ch38 off=17bit（mfg_ad 280bit 掩码，48 帧一致）
# 用法: python tools/ble-crack.py
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

MFG = bytes.fromhex("010F4336632D5632332D31513857464C182B08FC24224A19522B455A56495A")
KNOWN = bytes([0x21, 0xFF, 0x19, 0x2B]) + MFG
OFF_BIT = 17          # ble-truth 共识命中
FILE = "out/build/x64-release/Release/s38.cs8"

def get_mask_run():
    """从 ch38 锁定帧取多数投票掩码（OFF_BIT 起 KNOWN*8 bit）"""
    sps = 20
    fs = 20e6
    raw = np.fromfile(FILE, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    nb = len(KNOWN) * 8
    anc = np.array([(KNOWN[i >> 3] >> (i & 7)) & 1 for i in range(nb)],
                   dtype=np.uint8)
    masks = []
    for s, e in bursts:
        fb = f[s:min(e, len(f))].copy()
        if len(fb) < sps * 60:
            continue
        fb -= np.median(fb)
        best = (999, -1, -1)
        for ph in range(sps):
            _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
            if bits is None:
                continue
            pos, err = bo.search_aa(bits, 2)
            if err < best[0]:
                best = (err, ph, pos)
        if best[0] > 2:
            continue
        _, ph, pos = best
        _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
        if bits is None or pos < 0:
            continue
        st = pos + 40 + OFF_BIT
        if st + nb > len(bits):
            continue
        masks.append(np.array(bits[st:st + nb], dtype=np.uint8) ^ anc)
    if len(masks) < 3:
        return None, 0
    ms = np.array(masks)
    vote = (ms.mean(axis=0) > 0.5).astype(np.uint8)
    agree = (ms == vote).mean(axis=1)
    return vote, int((agree > 0.95).sum())

def lfsr_fit(mask):
    """在全 (初始状态, 抽头对, 输出位) 空间拟合：mask[k] = out(state_k)，
    state 转移 = 左移+反馈。命中返回 (taps, seed_state, out_bit)"""
    n = len(mask)
    for t0 in range(7):
        for t1 in range(t0 + 1, 7):
            for out in range(7):
                for seed in range(1 << 7):
                    reg = seed
                    ok = True
                    for k in range(n):
                        if ((reg >> out) & 1) != mask[k]:
                            ok = False
                            break
                        fb = ((reg >> t0) ^ (reg >> t1)) & 1
                        reg = ((reg << 1) | fb) & 0x7F
                    if ok:
                        return (t0, t1), seed, out
    return None

def main():
    mask, nframes = get_mask_run()
    if mask is None:
        print("掩码提取失败")
        return 1
    print(f"掩码 {len(mask)} bit（{nframes} 帧强共识）")
    print("  bits:", "".join(map(str, mask[:64])))
    fit = lfsr_fit(mask)
    if fit is None:
        print("!! 7bit-2tap 左移 LFSR 拟合失败——试右移族...")
        return try_rightshift(mask)
    taps, seed, out = fit
    print(f"命中 LFSR: 反馈=b{taps[0]}^b{taps[1]} 输出=b{out} "
          f"起始状态=0x{seed:02X}（={seed}）")
    # 推论：掩码从 PDU bit17 起等于该 LFSR 从状态 seed 的输出流——
    # 则 PDU bit0 对应 LFSR 早 17 步的状态：
    reg = seed
    for _ in range(17):   # 反推 17 步（需可逆：左移反馈 LFSR 的逆=右移）
        b0 = reg & 1
        reg >>= 1
        # 逆：被移出的低位 bit 原是反馈位 → 与 taps 反解最高位
        # 简化：枚举验证而不是解析逆——直接枚举 128 个起点找能到达 seed 的
    # 全帧验证：用拟合的 LFSR（从 bit17 命中点向前外推）解码全帧并 CRC
    return verify(taps, out, seed)

def try_rightshift(mask):
    n = len(mask)
    for t0 in range(7):
        for t1 in range(t0 + 1, 7):
            for out in range(7):
                for seed in range(1 << 7):
                    reg = seed
                    ok = True
                    for k in range(n):
                        if ((reg >> out) & 1) != mask[k]:
                            ok = False
                            break
                        b6 = (reg >> 6) & 1
                        fb = ((reg >> t0) ^ (reg >> t1)) & 1
                        reg = (reg >> 1) | (fb << 6)
                    if ok:
                        print(f"命中右移 LFSR: taps=b{t0}^b{t1} out=b{out} "
                              f"seed=0x{seed:02X}")
                        return 0
    print("两族均未拟合——掩码不是 7bit LFSR 输出")
    return 2

def verify(taps, out, seed):
    """用命中 LFSR 解码 s38 全部锁定帧：key 从 PDU bit0 =
    LFSR 在 seed 前 17 步的流——枚举 128 个可能起点，CRC 通过率最高者胜"""
    sps, fs = 20, 20e6
    raw = np.fromfile(FILE, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    frames = []
    for s, e in bursts:
        fb = f[s:min(e, len(f))].copy()
        if len(fb) < sps * 60:
            continue
        fb -= np.median(fb)
        best = (999, -1, -1)
        for ph in range(sps):
            _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
            if bits is None:
                continue
            pos, err = bo.search_aa(bits, 2)
            if err < best[0]:
                best = (err, ph, pos)
        if best[0] > 2:
            continue
        _, ph, pos = best
        _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
        if bits is not None and pos >= 0:
            frames.append((list(map(int, bits)), pos + 40))
    best_v = (0, None)
    for seed0 in range(1 << 7):
        # 生成从 PDU bit0 的 key 流（seed0 = bit0 时刻的状态）
        key = np.empty(64 * 8, dtype=np.uint8)
        reg = seed0
        for k in range(len(key)):
            key[k] = (reg >> out) & 1
            fbk = ((reg >> taps[0]) ^ (reg >> taps[1])) & 1
            reg = ((reg << 1) | fbk) & 0x7F
        n_ok = 0
        for bits, pdu in frames:
            nb = min((len(bits) - pdu) // 8, 64)
            by = []
            for b in range(nb):
                v = 0
                for t in range(8):
                    v |= (bits[pdu + b * 8 + t] ^ int(key[b * 8 + t])) << t
                by.append(v)
            t_, ln = by[0] & 0x0F, by[1] & 0x3F
            if not (1 <= ln <= 63) or 2 + ln + 3 > nb:
                continue
            body = bytes(by[:2 + ln])
            crc = 0x555555
            for byte in body:
                crc ^= byte
                for _ in range(8):
                    crc = (crc >> 1) ^ 0x00065B if crc & 1 else crc >> 1
            rx = by[2 + ln] | (by[2 + ln + 1] << 8) | (by[2 + ln + 2] << 16)
            if crc & 0xFFFFFF == rx:
                n_ok += 1
        if n_ok > best_v[0]:
            best_v = (n_ok, seed0)
    print(f"全帧验证: 最佳起始状态 0x{best_v[1]:02X} → CRC 通过 "
          f"{best_v[0]}/{len(frames)} 帧")
    if best_v[0] > 0:
        print("*** BLE 链路层破译成功 ***")
    return 0

if __name__ == "__main__":
    sys.exit(main())
