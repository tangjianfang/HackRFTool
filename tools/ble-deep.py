# 深度探针（#111 Phase B）：对重复帧（同设备同载荷多次接收）做
# ① 比特级一致区测量 ② 样本级相关定位 ③ CRC 全约定矩阵
# 用法: python tools/ble-deep.py <file.cs8> [fs_msps=20] [ch=39]
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

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
    # 收集所有 AA 锁定：{头签名: [(f段, ph, pos)]}
    groups = {}
    for bi, (s, e) in enumerate(bursts):
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
        if bits is None or pos < 0:
            continue
        # 用头+AdvA 前缀做分组签名（当前约定解，仅用于聚类）
        pdu = pos + 40
        if pdu + 64 > len(bits):
            continue
        pre_bits = "".join(map(str, bits[pdu:pdu + 64]))
        groups.setdefault(pre_bits, []).append((fb, ph, pos))
    big = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    print("重复组（前缀 64bit 一致）:", [(len(v)) for k, v in big[:5]])
    if not big or len(big[0][1]) < 2:
        print("无重复帧可用")
        return 1
    grp = big[0][1]
    # ① 比特级一致区（AA 后逐符号比）
    b1 = bo.slice_burst(grp[0][0], sps, grp[0][1], "full", 0, fs)[1]
    b2 = bo.slice_burst(grp[1][0], sps, grp[1][1], "full", 0, fs)[1]
    p1, p2 = grp[0][2] + 40, grp[1][2] + 40
    n = min(len(b1) - p1, len(b2) - p2)
    diff = [i for i in range(n) if b1[p1 + i] != b2[p2 + i]]
    print(f"两次接收 AA 后 {n} bit 中差异 {len(diff)} 处",
          f"首个差异@符号 {diff[0] if diff else '无'}",
          f"（前缀一致 {diff[0] if diff else n} bit）")
    # ② 样本级相关（按符号窗均值向量）
    m1 = bo.slice_burst(grp[0][0], sps, grp[0][1], "full", 0, fs)[0][p1 - 40:p1 + n]
    m2 = bo.slice_burst(grp[1][0], sps, grp[1][1], "full", 0, fs)[0][p2 - 40:p2 + n]
    c = np.corrcoef(m1, m2)[0, 1]
    print(f"符号窗均值序列相关系数: {c:.4f}（>0.99=波形同源）")
    # ③ CRC 全约定矩阵（固定帧：当前约定解出的 22B/len=17 或该组实际头）
    h = bo.dewhiten_bytes(list(map(int, b1)), p1, 2, ch)
    ln = h[1] & 0x3F
    nb = 2 + ln + 3
    if p1 + nb * 8 > len(b1):
        print("帧不完整，CRC 矩阵跳过")
        return 0
    frame = bo.dewhiten_bytes(list(map(int, b1)), p1, nb, ch)
    print(f"帧 {nb}B: {' '.join('%02X' % b for b in frame)}")
    import itertools
    polys = [0x00065B, 0x00DA6B, 0x00065B ^ 0xFFFFFF, 0x100001, 0x864CFB]
    inits = [0x555555, 0x000000, 0xFFFFFF, 0xAAAAAA]
    hit = False
    for poly in polys:
        for init in inits:
            for rev in (0, 1):
                crc = init
                for byte in frame[:2 + ln]:
                    if rev:
                        byte = int(f"{byte:08b}"[::-1], 2)
                    crc ^= byte
                    for _ in range(8):
                        crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
                crc &= 0xFFFFFF
                for asm in (0, 1):
                    rx = ((frame[2 + ln] | (frame[2 + ln + 1] << 8)
                           | (frame[2 + ln + 2] << 16)) if asm == 0 else
                          ((frame[2 + ln] << 16) | (frame[2 + ln + 1] << 8)
                           | frame[2 + ln + 2]))
                    if crc == rx:
                        print(f"CRC 命中: poly={poly:#08x} init={init:#08x} "
                              f"rev={rev} asm={asm}")
                        hit = True
    if not hit:
        print("CRC 矩阵（20 组合）：无命中——CRC 约定不是问题，"
              "比特流或白化掩码本身错")
    return 0

if __name__ == "__main__":
    sys.exit(main())
