# 真机比特白化穷举（#111 Phase B）：对确定性重复帧（同 AdvA 同载荷）穷举
# LFSR 变体，约束=头规范 AND CRC24 通过——比 #109 的 RFU=0 弱约束强得多
# 用法: python tools/ble-real-brute.py <file.cs8> [fs_msps=20] [ch=39]
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

def variant_key_stream(taps, seed, pre_shift, out_bit, n):
    """taps=(i,j)：反馈 bit_i^bit_j；out_bit：输出取 state 的哪一位"""
    reg = seed & 0x7F
    for _ in range(pre_shift):
        fb = ((reg >> taps[0]) ^ (reg >> taps[1])) & 1
        reg = ((reg << 1) | fb) & 0x7F
    out = []
    for _ in range(n):
        out.append((reg >> out_bit) & 1)
        fb = ((reg >> taps[0]) ^ (reg >> taps[1])) & 1
        reg = ((reg << 1) | fb) & 0x7F
    return out

def crc24_bytes(data):
    crc = 0x555555
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x00065B if crc & 1 else crc >> 1
    return crc & 0xFFFFFF

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out/build/x64-release/Release/ble39.cs8"
    fs = float(sys.argv[2] if len(sys.argv) > 2 else 20) * 1e6
    ch = int(sys.argv[3] if len(sys.argv) > 3 else 39)
    sps = int(round(fs / 1e6))
    raw = np.fromfile(path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    del raw, iq
    # AA 锁定一个突发（复用管线逻辑）
    target = None
    for bi, (s, e) in enumerate(bursts):
        fb = f[s:min(e, len(f))]
        if len(fb) < sps * 48:
            continue
        best = (999, -1, -1)
        for ph in range(sps):
            _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
            if bits is None:
                continue
            pos, err = bo.search_aa(bits, 2)
            if err < best[0]:
                best = (err, ph, pos)
        if best[0] <= 2 and best[2] >= 0:
            target = (fb, best[1], best[2])
            break
    if target is None:
        print("无 AA 锁定突发")
        return 1
    fb, ph, pos = target
    _, bits = bo.slice_burst(fb, sps, ph, "full", 0, fs)
    bits = list(map(int, bits))
    print(f"突发 AA@{pos}，可用 {len(bits)} bit")
    pdu = pos + 40
    # 变体空间：抽头对 21 × 种子 5 × 预移 8 × 输出位 7 × 字节序 2 × CRC 装配序 2
    # 关键放宽（相对 #109 弱约束版）：type 不限（现代安卓主流 ADV_EXT_IND=7）、
    # len 放宽 1..63、CRC 三字节装配两种序
    taps_all = [(i, j) for i in range(1, 7) for j in range(i + 1, 7)]
    seeds = [ch, ch | 0x40, ch ^ 0x55, (ch + 1) & 0x7F, (~ch) & 0x7F]
    hits = []
    for taps in taps_all:
        for seed in seeds:
            for pre in range(8):
                for ob in range(7):
                    # 头 2 字节（含 CRC 需要的长度）——type 不再过滤
                    ks = variant_key_stream(taps, seed, pre, ob, 16)
                    h0 = sum(bits[pdu + t] << t for t in range(8)) ^ sum(
                        ks[t] << t for t in range(8))
                    h1 = sum(bits[pdu + 8 + t] << t for t in range(8)) ^ sum(
                        ks[8 + t] << t for t in range(8))
                    ln = h1 & 0x3F
                    if not (1 <= ln <= 63):
                        continue
                    nb = 2 + ln + 3
                    if pdu + nb * 8 > len(bits):
                        continue
                    for lsb in (True, False):
                        ks2 = variant_key_stream(taps, seed, pre, ob, nb * 8)
                        frame = []
                        for b in range(nb):
                            v = 0
                            for t in range(8):
                                bt = bits[pdu + b * 8 + t] ^ ks2[b * 8 + t]
                                v |= bt << (t if lsb else 7 - t)
                            frame.append(v)
                        calc = crc24_bytes(frame[:2 + ln])
                        for asm in (0, 1):
                            if asm == 0:
                                rx = (frame[2 + ln] | (frame[2 + ln + 1] << 8)
                                      | (frame[2 + ln + 2] << 16))
                            else:
                                rx = ((frame[2 + ln] << 16)
                                      | (frame[2 + ln + 1] << 8)
                                      | frame[2 + ln + 2])
                            if calc == rx:
                                hits.append((taps, seed, pre, ob, lsb, asm,
                                             h0 & 0x0F, ln, frame))
    if not hits:
        print("穷举（type 全域/len≤63/CRC 双装配序）：无命中——链路层假设整体出局")
        return 2
    print(f"命中 {len(hits)} 个变体：")
    seen = set()
    for taps, seed, pre, ob, lsb, asm, t_, ln, frame in hits:
        sig = (taps, seed, pre, ob, lsb, asm)
        if sig in seen:
            continue
        seen.add(sig)
        adv = "%02X%02X%02X%02X%02X%02X" % tuple(frame[2:8])
        nm = "".join(chr(c) if 32 <= c < 127 else "." for c in frame[10:22])
        print(f"  taps=b{taps[0]}^b{taps[1]} seed={seed} pre={pre} out=b{ob} "
              f"lsb={int(lsb)} crcasm={asm} → type={t_} len={ln} AdvA={adv} "
              f"name={nm}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
