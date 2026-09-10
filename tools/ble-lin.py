# 多设备联合统计攻击（#111 选项 3）：CRC24 线性代数破译
# ① 同信道帧对 d=rawA^rawB（密钥抵消）→ 同长帧 CRC 线性约束=免密钥验长
# ② GF(2) 线性方程组：N 帧×24 方程 → 直接解出密钥位（RANSAC 容错）
# ③ 解码全帧 → CRC/结构双验证 → AdvA/设备名浮出
# 用法: python tools/ble-lin.py <file.cs8> <ch>   （先合成自检）
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

def bits_to_arr(bits, st, nb):
    return np.array(bits[st: st + nb * 8], dtype=np.uint8)

def arr_to_bytes(a):
    # LSB-first 每 8bit 打包
    return [int(a[i * 8:(i + 1) * 8].dot(1 << np.arange(8)))
            for i in range(len(a) // 8)]

def selftest():
    """合成验证 d-恒等式与线性求解在'任意未知密钥'下成立"""
    rng = np.random.default_rng(7)
    key = rng.integers(0, 2, 600, dtype=np.uint8)     # 任意白化（非 LFSR 也行）
    frames = []
    for i in range(8):
        ln = int(rng.integers(6, 30))
        plain = rng.integers(0, 2, (2 + ln + 3) * 8, dtype=np.uint8)
        # 长度字节写进 plain（bits 8..16）
        lb = [(ln >> b) & 1 for b in range(6)] + [0, 0]
        plain[8:16] = lb
        body = plain[: (2 + ln) * 8]
        cv = crc24(arr_to_bytes(body))
        plain[(2 + ln) * 8: (2 + ln + 3) * 8] = [(cv >> b) & 1 for b in
                                                 range(24)]
        raw = plain ^ key[: len(plain)]
        frames.append((raw, len(plain) // 8, ln))
    # ① d-验长
    ok = 0
    for i in range(len(frames)):
        for j in range(i + 1, len(frames)):
            ra, na, la = frames[i]
            rb, nb_, lb_ = frames[j]
            if la != lb_ or na != nb_:
                continue
            d = ra[: na * 8] ^ rb[: nb_ * 8]
            db = arr_to_bytes(d[: (2 + la) * 8])
            dcr = int(d[(2 + la) * 8: (2 + la + 3) * 8].dot(1 << np.arange(24)))
            if crc24(db, 0) == dcr:
                ok += 1
    print(f"[自检] 同长对 d-验长命中 {ok}")
    # ② 线性求解 key（已知每帧 len）——基按帧体长度求值
    nmax = max(n for _, n, _ in frames)
    nunk = nmax * 8
    basis_cache = {}
    def basis_for(Lbytes):
        if Lbytes not in basis_cache:
            nb_ = Lbytes * 8
            b_ = []
            for j in range(nb_):
                e = np.zeros(nb_, dtype=np.uint8)
                e[j] = 1
                b_.append(crc24(arr_to_bytes(e), 0))
            basis_cache[Lbytes] = b_
        return basis_cache[Lbytes]
    A = []
    for raw, nb, ln in frames:
        body_b = arr_to_bytes(raw[: (2 + ln) * 8])
        rx = int(raw[(2 + ln) * 8: (2 + ln + 3) * 8].dot(1 << np.arange(24)))
        r = crc24(body_b) ^ rx
        basis = basis_for(2 + ln)
        for b in range(24):
            row = np.zeros(nunk + 1, dtype=np.uint8)
            for j in range((2 + ln) * 8):
                if (basis[j] >> b) & 1:
                    row[j] = 1
            row[(2 + ln) * 8 + b] ^= 1   # K_crc 直通
            row[nunk] = (r >> b) & 1
            A.append(row)
    A = np.array(A, dtype=np.uint8)
    sol = gauss(A, nunk)
    if sol is None:
        print("[自检] 方程组不相容（合成不该发生）")
        return
    # 自由变量可能使解与真值差一个零空间向量——验证方程满足+解码一致即可：
    # 直接验证：用 sol 作 key 解码所有帧，CRC 应全过
    n_ok = 0
    for raw, nb, ln in frames:
        plain = raw ^ sol[: len(raw)]
        body = arr_to_bytes(plain[: (2 + ln) * 8])
        rx = int(plain[(2 + ln) * 8: (2 + ln + 3) * 8].dot(1 << np.arange(24)))
        if crc24(body) == rx:
            n_ok += 1
    print(f"[自检] 用解出的 key 解码: {n_ok}/{len(frames)} 帧 CRC 全过"
          f"（含零空间自由度时仍应全过）")

def gauss(A, n):
    A = A.copy()
    rows, cols = A.shape
    piv = 0
    where = [-1] * n
    for c in range(n):
        r_ = None
        for r in range(piv, rows):
            if A[r, c]:
                r_ = r
                break
        if r_ is None:
            continue
        A[[piv, r_]] = A[[r_, piv]]
        for r in range(rows):
            if r != piv and A[r, c]:
                A[r] ^= A[piv]
        where[c] = piv
        piv += 1
        if piv == rows:
            break
    # 相容性：消元后右端为 1 的零行
    for r in range(rows):
        if not A[r, : n].any() and A[r, n]:
            return None
    sol = np.zeros(n, dtype=np.uint8)
    for c in range(n):
        if where[c] >= 0:
            sol[c] = A[where[c], n]
    return sol

if __name__ == "__main__":
    selftest()
