# 多设备联合攻击·真机阶段 1（#111 选项 3）：d-验长统计 + 设备分组
# d=rawA^rawB 同信道同长帧：crc24(d_body,0)==d_crc → 免密钥知长度
# d[0:16]==0 → 同头（同设备同帧型）；d[16:64]==0 → 同 AdvA（同设备）
# 用法: python tools/ble-lin2.py
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

def crc24(bb, init=0x555555):
    crc = init
    for byte in bb:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x00065B if crc & 1 else crc >> 1
    return crc & 0xFFFFFF

def to_bytes(bits, st, nb):
    out = []
    for b in range(nb):
        v = 0
        for t in range(8):
            v |= int(bits[st + b * 8 + t]) << t
        out.append(v)
    return out

FILES = [("out/build/x64-release/Release/s37.cs8", 37),
         ("out/build/x64-release/Release/s38.cs8", 38),
         ("out/build/x64-release/Release/s39.cs8", 39),
         ("out/build/x64-release/Release/ble37.cs8", 37),
         ("out/build/x64-release/Release/ble38.cs8", 38),
         ("out/build/x64-release/Release/ble39d.cs8", 39)]

def load_frames(path, ch):
    sps, fs = 20, 20e6
    raw = np.fromfile(path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    out = []
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
            nb = (len(bits) - (pos + 40)) // 8
            if nb >= 12:
                out.append((ch, np.array(bits[pos + 40: pos + 40 + nb * 8],
                                         dtype=np.uint8)))
    return out

def main():
    frames = []
    for p, ch in FILES:
        try:
            fr = load_frames(p, ch)
            print(f"{p.split('/')[-1]} ch{ch}: {len(fr)} 帧")
            frames += fr
        except FileNotFoundError:
            print(f"{p.split('/')[-1]}: 缺")
    by_ch = {}
    for ch, b in frames:
        by_ch.setdefault(ch, []).append(b)
    for ch, fl in by_ch.items():
        n = len(fl)
        # 设备分组：d[0:16]==0（同头）——完全相同的帧
        groups = []
        for i, b in enumerate(fl):
            for g in groups:
                if np.array_equal(b[:16], fl[g[0]][:16]):
                    g.append(i)
                    break
            else:
                groups.append([i])
        # d-验长（全对 × len 1..40）
        len_votes = {}
        pair_hits = 0
        for i in range(n):
            for j in range(i + 1, n):
                a, b = fl[i], fl[j]
                m = min(len(a), len(b)) // 8
                for ln in range(1, min(m - 4, 40)):
                    nb = 2 + ln + 3
                    if nb > m:
                        continue
                    d = a[: nb * 8] ^ b[: nb * 8]
                    if d.any() == 0:
                        continue   # 完全同帧不给长度信息
                    db = to_bytes(d, 0, 2 + ln)
                    dcr = int(d[(2 + ln) * 8: (2 + ln + 3) * 8].dot(
                        1 << np.arange(24)))
                    if crc24(db, 0) == dcr:
                        len_votes.setdefault(ln, []).append((i, j))
                        pair_hits += 1
        print(f"== ch{ch}: {n} 帧，{len(groups)} 头组，d-验长命中 {pair_hits}")
        for ln, pairs in sorted(len_votes.items()):
            ids = set()
            for i, j in pairs:
                ids.add(i)
                ids.add(j)
            print(f"   len={ln}: {len(pairs)} 对，涉及 {len(ids)} 帧")
    return 0

if __name__ == "__main__":
    sys.exit(main())
