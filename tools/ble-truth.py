# 已知明文掩码推导 v2（#111）：真值=LightBlue 截图设备 BW2376851
# —— 比特级滑动 + 三锚点（厂商 AD/ASCII/名称 AD）+ 1M/2M 双速率切片
# 跨帧掩码一致 = 真命中 → 打印掩码结构
# 用法: python tools/ble-truth.py [file.cs8 ...]
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

MFG = bytes.fromhex("010F4336632D5632332D31513857464C182B08FC24224A19522B455A56495A")
ANCHORS = [
    ("mfg_ad", bytes([0x22, 0xFF, 0x19, 0x2B]) + MFG),
    ("bw_stable", bytes([0x22, 0xFF, 0x19, 0x2B, 0x01, 0x0F]) + b"C6c-V2"),
    ("bw_name", bytes([0x0A, 0x09]) + b"BW2376851"),
    ("razor_name", bytes([0x11, 0x09]) + b"BlackWidow V3 Mini"),
    ("razor_ascii", b"BlackWidow V3 Mini"),
    ("hid_uuid", bytes([0x03, 0x03, 0x12, 0x18])),
]

def collect_locks(path, ch, fs=20e6):
    sps = 20
    raw = np.fromfile(path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    locks = []
    for s, e in bursts:
        fb = f[s:min(e, len(f))].copy()
        if len(fb) < sps * 60:
            continue
        # 居中：非信道中心采集时（band 猎捕），信号载在 ±f0 偏移上——
        # 鉴频器输出减去突发频率中值，否则比特恒 1，AA 永不命中
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
        # 1M 比特（PDU 起点）与 2M 比特（AA 后按 10sps 续切）
        _, bits1 = bo.slice_burst(fb, sps, ph, "full", 0, fs)
        st2 = ph + (pos + 40) * sps
        n2 = (len(fb) - st2) // 10
        bits2 = None
        if n2 > 100:
            w = fb[st2: st2 + n2 * 10].reshape(n2, 10).mean(axis=1)
            bits2 = (w > 0).astype(np.uint8)
        locks.append((ch, list(map(int, bits1)) if bits1 is not None else None,
                      bits2, pos))
    return locks

def scan(locks, which):
    from collections import defaultdict
    hits = defaultdict(list)
    for idx, (ch, b1, b2, pos) in enumerate(locks):
        bits = b1 if which == "1M" else b2
        if bits is None or len(bits) == 0:
            continue
        base = (pos + 40) if which == "1M" else 0
        for aname, anc in ANCHORS:
            nb = len(anc) * 8
            for bitoff in range(0, min(len(bits) - base - nb, 64 * 8)):
                m = bytearray()
                for i in range(nb):
                    m.append(bits[base + bitoff + i] ^ ((anc[i >> 3] >>
                                                         (i & 7)) & 1))
                hits[(aname, ch, which, bitoff)].append((bytes(m), idx))
    return hits

def consensus_report(hits):
    """模糊共识：每键取多数投票掩码，报 ≥2 帧且最佳帧一致度 ≥85% 的键
    （BER~1% 下精确全等几乎不可能——两帧 144bit 都无错才相等）"""
    out = []
    for key, lst in hits.items():
        if len(lst) < 2:
            continue
        aname = key[0]
        ms = np.array([list(m) for m, _ in lst], dtype=np.uint8)
        n_bits = ms.shape[1]
        vote = (ms.mean(axis=0) > 0.5).astype(np.uint8)
        agree = (ms == vote).mean(axis=1)   # 每帧与共识的一致度
        best = agree.max()
        n_strong = int((agree >= 0.85).sum())
        if n_strong >= 2 and best >= 0.85 and len(aname) >= 0:
            if n_bits >= 8 * 8 or best >= 0.99:   # 短锚点要求近精确
                out.append((key, n_strong, best, vote, [lst[i][1] for i in
                                                        np.argsort(-agree)[:3]]))
    return out

def main():
    files = sys.argv[1:] or ["out/build/x64-release/Release/n39.cs8",
                             "out/build/x64-release/Release/n37.cs8"]
    all_locks = []
    for p in files:
        ch = 39 if "n39" in p or "ble39" in p else (37 if "37" in p else 38)
        locks = collect_locks(p, ch)
        print(f"{p.split('/')[-1]}: {len(locks)} 锁定")
        all_locks += locks
    for which in ("1M", "2M"):
        hits = scan(all_locks, which)
        print(f"== {which} 切片掩码共识命中：")
        reps = consensus_report(hits)
        reps.sort(key=lambda r: -r[1] * r[2])
        n = 0
        for key, n_strong, best, vote, fidx in reps[:6]:
            aname, ch, w, bitoff = key
            print(f"  [{aname}] ch{ch} off={bitoff}bit 强帧{n_strong} "
                  f"一致度{best*100:.0f}% 帧{fidx}")
            print("   mask:", " ".join("%02X" % int(b) for b, _ in
                                       zip(vote[:24], range(24))))
            n += 1
        if n == 0:
            print("  无")
    return 0

if __name__ == "__main__":
    sys.exit(main())
