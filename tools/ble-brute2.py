# 白化全空间穷举 v2（#111 Phase B）：对比特已证稳定的重复帧，两阶段搜索
# 阶段1：头 16bit 过滤（type 有效 + RFU=0）；阶段2：整帧 CRC24。
# 轴：抽头对 28 × 种子 10 × 预移 8 × 输出位 7 × 字节序 2 × 掩码起点偏移 ±16 × CRC 装配 2
# 用法: python tools/ble-brute2.py <file.cs8> [fs_msps=20] [ch=39]
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

def key_stream(taps, seed, pre, out, n):
    reg = seed & 0x7F
    for _ in range(pre):
        fb = ((reg >> taps[0]) ^ (reg >> taps[1])) & 1
        reg = ((reg << 1) | fb) & 0x7F
    o = np.empty(n, dtype=np.int8)
    for k in range(n):
        o[k] = (reg >> out) & 1
        fb = ((reg >> taps[0]) ^ (reg >> taps[1])) & 1
        reg = ((reg << 1) | fb) & 0x7F
    return o

POW8 = 1 << np.arange(8, dtype=np.int64)

def pack_bytes(win):   # win: (nbytes, 8) LSB-first → 字节值
    return (win * POW8).sum(axis=1) & 0xFF

def crc24_bytes(data):
    crc = 0x555555
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x00065B if crc & 1 else crc >> 1
    return crc & 0xFFFFFF

VALID_T = (0, 1, 2, 3, 4, 5, 6, 7)

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
    # 最大重复组的帧比特（已证稳定）
    groups = {}
    for s, e in bursts:
        fb = f[s:min(e, len(f))]
        if len(fb) < sps * 60:
            continue
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
        if bits is None or pos < 0 or pos + 40 + 200 > len(bits):
            continue
        sig = "".join(map(str, bits[pos + 40: pos + 40 + 64]))
        groups.setdefault(sig, []).append((np.array(bits, dtype=np.int8), pos))
    if not groups:
        print("无 AA 帧")
        return 1
    # 取两个最大组都试（一组可能是标准广播、另一组可能是扫描类帧）
    ranked = sorted(groups.items(), key=lambda kv: -len(kv[1]))[:2]
    all_hits = []
    for sig, grp in ranked:
        bits, pos = grp[0]
        pdu = pos + 40
        nbits = (min(len(bits) - pdu, 40 * 8) // 8) * 8
        bits = bits[pdu: pdu + nbits]
        print(f"重复组 {len(grp)} 帧，PDU {nbits} bit")
        taps_all = [(i, j) for i in range(7) for j in range(i + 1, 7)]
        seeds = [ch, ch + 1, ch - 1, 37, 38, ch | 0x40, ch ^ 0x55, (~ch) & 0x7F,
                 0x55, 0x7F]
        offs = range(-16, 17)
        hits = []

        def try_mask(kk, label):
            for lsb in (0, 1):
                x = (bits ^ kk[:nbits]).reshape(-1, 8)
                by = pack_bytes(x if lsb else x[:, ::-1])
                t_, ln = int(by[0]) & 0x0F, int(by[1]) & 0x3F
                if t_ not in VALID_T or not (1 <= ln <= 63):
                    continue
                if by[1] & 0xC0:
                    continue   # RFU=0
                nb = 2 + ln + 3
                if nb > len(by):
                    continue
                calc = crc24_bytes(by[:2 + ln])
                for asm in (0, 1):
                    rx = ((int(by[2 + ln]) | (int(by[2 + ln + 1]) << 8)
                           | (int(by[2 + ln + 2]) << 16)) if asm == 0
                          else ((int(by[2 + ln]) << 16)
                                | (int(by[2 + ln + 1]) << 8)
                                | int(by[2 + ln + 2])))
                    if calc == rx:
                        hits.append((label, lsb, asm, t_, ln, by))
                        print("命中:", label, "lsb", lsb, "asm", asm,
                              "type", t_, "len", ln)
                        print("  帧:", " ".join("%02X" % b for b in by[:nb]))

        # 恒等掩码（无白化）——不在 LFSR 空间内，单独一试
        try_mask(np.zeros(nbits, dtype=np.int8), "identity")
        for taps in taps_all:
            for seed in seeds:
                for pre in range(8):
                    for out in range(7):
                        key = key_stream(taps, seed, pre, out, nbits + 32)
                        for off in offs:
                            if off >= 0:
                                kk = key[off: off + nbits]
                            else:
                                kk = np.concatenate(
                                    [np.zeros(-off, np.int8),
                                     key[:nbits + off]])
                            try_mask(kk, (taps, seed, pre, out, off))
        all_hits += hits
    if not all_hits:
        print("全空间（含 type 3/5、恒等掩码、双组帧）：无命中")
    return 0

if __name__ == "__main__":
    sys.exit(main())
