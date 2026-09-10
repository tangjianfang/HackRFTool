# 终审（#111）：17bit 头部掩码穷举 × CRC24——42 帧全过 = 链路层破译
# 依据：ch38 掩码共识（PDU bit17 起已知 280bit，位置稳定）
# 用法: python tools/ble-final.py
import importlib.util
import sys
import numpy as np

spec = importlib.util.spec_from_file_location("bo", "tools/ble-offline.py")
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)

MFG = bytes.fromhex("010F4336632D5632332D31513857464C182B08FC24224A19522B455A56495A")
KNOWN = bytes([0x22, 0xFF, 0x19, 0x2B]) + MFG
OFF_BIT = 17
FILE = "out/build/x64-release/Release/s38.cs8"
VALID_T = (0, 1, 2, 3, 4, 5, 6, 7)

def crc24(bb):
    crc = 0x555555
    for byte in bb:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x00065B if crc & 1 else crc >> 1
    return crc & 0xFFFFFF

def main():
    sps, fs = 20, 20e6
    raw = np.fromfile(FILE, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    mag = np.abs(iq)
    bursts = bo.find_bursts(mag, fs)
    f = (np.angle(iq[1:] * np.conj(iq[:-1])).astype(np.float32)
         * float(fs / (2 * np.pi)))
    nb_anchor = len(KNOWN) * 8
    anc = np.array([(KNOWN[i >> 3] >> (i & 7)) & 1 for i in range(nb_anchor)],
                   dtype=np.uint8)
    frames = []       # 每个: (raw_bits ndarray, pdu)
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
        bits = np.array(bits, dtype=np.uint8)
        st = pos + 40
        if st + 320 > len(bits):
            continue
        frames.append((bits, st))
        masks.append(bits[st + OFF_BIT: st + OFF_BIT + nb_anchor] ^ anc)
    print(f"{len(frames)} 帧；掩码共识核验…")
    ms = np.array(masks)
    vote = (ms.mean(axis=0) > 0.5).astype(np.uint8)
    agree = (ms == vote).mean(axis=1)
    strong = np.flatnonzero(agree > 0.95)
    print(f"强共识 {len(strong)} 帧")
    if len(strong) < 5:
        print("共识不足")
        return 1
    # 用强共识帧做终审
    sf = [frames[i] for i in strong]
    known_mask = vote    # PDU bit17 起的掩码
    # 2^17 穷举头部掩码
    # 每帧取前 320 bit（40B）；头 17bit 掩码 = h；bit17 起用 known_mask
    hits = []
    for h in range(1 << 17):
        hb = np.array([(h >> i) & 1 for i in range(17)], dtype=np.uint8)
        b0_bits = sf[0][0][sf[0][1]: sf[0][1] + 8] ^ hb[:8]
        b1_bits = sf[0][0][sf[0][1] + 8: sf[0][1] + 16] ^ hb[8:16]
        b0 = int(b0_bits.dot(1 << np.arange(8)))
        b1 = int(b1_bits.dot(1 << np.arange(8)))
        t_, ln = b0 & 0x0F, b1 & 0x3F
        if t_ not in VALID_T or not (1 <= ln <= 32):
            continue   # 掩码覆盖 PDU 前 37 字节（280+17bit）
        nb = 2 + ln + 3
        # 全帧解码+CRC（掩码=hb ++ known_mask）
        ok = 0
        for bits, st in sf[:6]:
            if st + nb * 8 > len(bits):
                break
            mk = np.concatenate([hb, known_mask[:nb * 8 - 17]])
            by = []
            for bI in range(nb):
                v = int((bits[st + bI * 8: st + bI * 8 + 8] ^
                         mk[bI * 8: bI * 8 + 8]).dot(1 << np.arange(8)))
                by.append(v)
            if crc24(by[:2 + ln]) == (by[2 + ln] | (by[2 + ln + 1] << 8)
                                      | (by[2 + ln + 2] << 16)):
                ok += 1
        if ok >= 4:
            hits.append((h, t_, ln, ok))
            print(f"命中: h=0x{h:05X} type={t_} len={ln} "
                  f"前 6 帧 CRC 过 {ok}")
    if not hits:
        print("17bit 穷举无命中（锚点内容或有出入）")
        return 2
    # 众帧全验
    h, t_, ln, _ = hits[0]
    hb = np.array([(h >> i) & 1 for i in range(17)], dtype=np.uint8)
    mk = np.concatenate([hb, known_mask[: (2 + ln + 3) * 8 - 17]])
    n_ok = 0
    for bits, st in sf:
        if st + (2 + ln + 3) * 8 > len(bits):
            continue
        by = []
        for bI in range(2 + ln + 3):
            v = int((bits[st + bI * 8: st + bI * 8 + 8] ^
                     mk[bI * 8: bI * 8 + 8]).dot(1 << np.arange(8)))
            by.append(v)
        if crc24(by[:2 + ln]) == (by[2 + ln] | (by[2 + ln + 1] << 8)
                                  | (by[2 + ln + 2] << 16)):
            n_ok += 1
    print(f"全验: {n_ok}/{len(sf)} 帧 CRC 通过")
    if n_ok > len(sf) * 0.5:
        print("*** 破译成功：掩码/头/CRC 全链闭合 ***")
        print("帧样例:", " ".join("%02X" % b for b in by[:2 + ln + 3]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
