# BLE 白化/CRC 约定离线穷举（#109）：对真机 AA 命中比特转储，穷举
# 白化 LFSR 全部合理约定组合，找 CRC24 能通过 ≥N 帧的变体
import glob, sys

def read_dumps():
    out = []
    for p in glob.glob(r"C:/tjf/github/HackRFTool/out/build/x64-release/Release/ble-bits-*.txt"):
        head = open(p).read(64)
        ch = int(head.split("ch=")[1].split()[0])
        bits_txt = open(p).read().split("bits=")[1].strip()
        bits = [1 if c == '1' else 0 for c in bits_txt]
        out.append((ch, bits))
    return out

def find_aa(bits):
    # 前导 0xAA/0x55 + 接入码 D6BE898E（空口序），返回起始位下标列表
    hits = []
    n = len(bits)
    pre = [0,1,0,1,0,1,0,1]
    pre2 = pre[::-1]
    aa = [0xD6,0xBE,0x89,0x8E]
    for start in range(0, n - 40*8):
        b = bits[start:start+8]
        if b != pre and b != pre2:
            continue
        ok = True
        for j, ab in enumerate(aa):
            byte = bits[start+8+j*8 : start+16+j*8]
            v = 0
            for k, x in enumerate(byte):
                v |= x << k
            if v != ab:
                ok = False
                break
        if ok:
            hits.append(start)
    return hits

def gen_lfsr_seq(seed, n, shift_left, out_tap, tap_a, tap_b, invert_out):
    # 返回前 n 个白化比特
    seq = [0]*n
    reg = seed & 0x7F
    for k in range(n):
        if shift_left:
            out = (reg >> out_tap) & 1
            fb = ((reg >> tap_a) ^ (reg >> tap_b)) & 1
            reg = ((reg << 1) | fb) & 0x7F
        else:
            out = (reg >> out_tap) & 1
            fb = ((reg >> tap_a) ^ (reg >> tap_b)) & 1
            reg = (reg >> 1) | (fb << 6)
        seq[k] = out ^ (1 if invert_out else 0)
    return seq

def crc24(data_bits, init, msb_first):
    crc = init
    for i in range(0, len(data_bits) - 7, 8):
        byte = 0
        for k in range(8):
            byte |= data_bits[i+k] << k
        if msb_first:
            for k in range(7, -1, -1):
                bit = (byte >> k) & 1
                fb = (crc & 1) ^ bit
                crc >>= 1
                if fb: crc ^= 0x00065B
        else:
            for k in range(8):
                bit = (byte >> k) & 1
                fb = (crc & 1) ^ bit
                crc >>= 1
                if fb: crc ^= 0x00065B
    return crc & 0xFFFFFF

def main():
    dumps = read_dumps()
    print(f"dumps: {len(dumps)}")
    if not dumps:
        return
    best = []
    variants = []
    for shift_left in (True, False):
        for out_tap in (6, 0):
            for taps in ((6,3), (4,0), (6,4)):
                if taps == (out_tap, taps[1]) and out_tap == taps[0]:
                    pass
                for seed_mode in ("ch", "ch2", "ch_lsb_first", "ch_msb"):
                    for inv in (False, True):
                        variants.append((shift_left, out_tap, taps, seed_mode, inv))
    def seed_of(mode, ch):
        if mode == "ch": return ch & 0x7F
        if mode == "ch2": return (ch | 2) & 0x7F
        if mode == "ch_lsb_first": return ch & 0x7F
        if mode == "ch_msb":
            r = 0
            for k in range(7):
                r |= ((ch >> k) & 1) << (6 - k)
            return r
    # 每个变体：对每个 dump 的每个 AA 位置试 CRC24
    from collections import Counter
    counter = Counter()
    for shift_left, out_tap, taps, seed_mode, inv in variants:
        ok_frames = 0
        for ch, bits in dumps:
            for start in find_aa(bits)[:3]:
                pdu_off = start + 40
                n = len(bits) - pdu_off
                if n < 2*8 + 24:
                    continue
                # 收集足够比特（最多 40 字节）
                max_bytes = min(40, (n - 24)//8)
                seq = gen_lfsr_seq(seed_of(seed_mode, ch), max_bytes*8,
                                   shift_left, out_tap, taps[0], taps[1], inv)
                data_bits = [bits[pdu_off+k] ^ seq[k] for k in range(max_bytes*8)]
                h0 = sum(data_bits[k] << k for k in range(8))
                h1 = sum(data_bits[8+k] << k for k in range(8))
                ln = h1 & 0x3F
                if ln == 0 or ln > 63:
                    continue
                if (2 + ln) * 8 + 24 > len(data_bits):
                    continue
                crc_rx_bits = data_bits[(2+ln)*8 : (2+ln)*8+24]
                for msb in (False, True):
                    rx = sum(crc_rx_bits[k] << k for k in range(24))
                    if msb:
                        rx2 = sum(crc_rx_bits[k] << (23-k) for k in range(24))
                    else:
                        rx2 = rx
                    calc = crc24(data_bits[:(2+ln)*8], 0x555555, msb)
                    if calc == rx2 or calc == rx:
                        ok_frames += 1
                        counter[(shift_left, out_tap, taps, seed_mode, inv, msb)] += 1
        best.append((ok_frames, (shift_left, out_tap, taps, seed_mode, inv)))
    best.sort(reverse=True)
    for cnt, v in best[:8]:
        print(cnt, v)
    print("---- top exact hits ----")
    for combo, c in counter.most_common(5):
        print(c, combo)

main()
