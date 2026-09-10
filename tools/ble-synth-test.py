# 合成 BLE 广播帧自检（#111 Phase B 前置）：生成带 CFO+噪声的 GFSK 信号
# → 喂给 ble-offline 管线 → 各变体 CRC 通过率。管线约定有错必然此处先炸。
# 用法: python tools/ble-synth-test.py
import sys
import importlib.util
import numpy as np
spec = importlib.util.spec_from_file_location(
    "ble_offline", "tools/ble-offline.py")
ble_offline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ble_offline)
seq_bits, crc24_bits, EXP = (ble_offline.seq_bits, ble_offline.crc24_bits,
                             ble_offline.EXP)

def whiten_bits(raw_bits, ch):
    key = seq_bits(ch, len(raw_bits))
    return [a ^ b for a, b in zip(raw_bits, key)]

def build_frame(ch, adv_a, name):
    payload = adv_a + [0x02, 0x01, 0x06] + [len(name) + 1, 0x09] + list(name)
    ln = len(payload)
    body = [0x00, ln] + payload   # type=0 ADV_IND
    body_bits = []
    for b in body:
        body_bits += [(b >> k) & 1 for k in range(8)]
    crc = crc24_bits(body_bits)
    frame = body + [crc & 0xFF, (crc >> 8) & 0xFF, (crc >> 16) & 0xFF]
    fb = []
    for b in frame:
        fb += [(b >> k) & 1 for k in range(8)]
    wb = whiten_bits(fb, ch)
    pre = []
    for b in (0xAA, 0xD6, 0xBE, 0x89, 0x8E):
        pre += [(b >> k) & 1 for k in range(8)]
    return pre + wb

def gfsk_mod(bits, fs=20e6, dev=250e3, bt=0.5, amp=60.0, cfo_hz=60e3,
             noise=8.0):
    sps = int(fs / 1e6)
    up = np.repeat(np.where(np.array(bits) > 0, 1.0, -1.0), sps)
    # 高斯脉冲成形（BT=0.5：sigma≈sps*0.39，BLE 1M 典型）
    sig = sps * 0.39
    gx = np.exp(-0.5 * (np.arange(-4 * sps, 4 * sps + 1) / sig) ** 2)
    gx /= gx.sum()
    shaped = np.convolve(up, gx, "same")
    t = np.arange(len(shaped)) / fs
    phase = 2 * np.pi * np.cumsum(shaped * dev + cfo_hz) / fs
    iq = amp * np.exp(1j * phase)
    iq += np.random.normal(0, noise, len(iq)) + 1j * np.random.normal(
        0, noise, len(iq))
    return iq

def main():
    np.random.seed(42)
    frames = []
    n_frames = 20
    for i in range(n_frames):
        bits = build_frame(39, [0x11, 0x22, 0x33, 0x44, 0x55, i],
                           f"SYNTH-{i}".encode())
        frames.append(bits)
    # 拼一段"空中时序"：帧间隔 3ms 静默（模拟广播间隔），CFO 每帧随机 ±80kHz
    fs = 20e6
    quiet_n = int(3e-3 * fs)
    def mk_quiet():
        return (np.random.normal(0, 8.0, quiet_n)
                + 1j * np.random.normal(0, 8.0, quiet_n))
    parts = [mk_quiet()]
    cfo_used = []
    for bits in frames:
        cfo = np.random.uniform(-80e3, 80e3)
        cfo_used.append(cfo)
        parts.append(gfsk_mod(bits, cfo_hz=cfo))
        parts.append(mk_quiet())
    air = np.concatenate(parts)
    raw = np.empty(len(air) * 2, dtype=np.int8)
    raw[0::2] = np.clip(air.real, -127, 127).astype(np.int8)
    raw[1::2] = np.clip(air.imag, -127, 127).astype(np.int8)
    raw.tofile("ble-synth.cs8")
    print(f"合成 {n_frames} 帧（CFO ±80kHz、噪声 σ=8）→ ble-synth.cs8")
    # 跑管线
    sys.argv = ["ble-offline.py", "ble-synth.cs8", "20", "39"]
    ble_offline.main()
    print("（判定门对合成信号期望：full+CFO 或 center/gauss 全绿——约定即验证）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
