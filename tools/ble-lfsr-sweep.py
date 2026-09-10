# 宽域穷举：以"去白化后头 2 字节满足 BLE 规范硬约束"为目标
# （h0 bit4/5=RFU 必 0；type<=7；len 6..63），扫 LFSR 全变体
import glob, itertools

def load(p):
    head = open(p).read(60)
    ch = int(head.split("ch=")[1].split()[0])
    bits = [1 if c == '1' else 0 for c in open(p).read().split("bits=")[1].strip()]
    return ch, bits

dumps = []
for p in sorted(glob.glob(r"C:/tjf/github/HackRFTool/out/build/x64-release/Release/ble-bits-*.txt"))[:10]:
    dumps.append(load(p))

def find_aa(bits):
    aa = [0xD6,0xBE,0x89,0x8E]
    res = []
    for start in range(0, min(len(bits)-48*8, 4000)):
        b = bits[start:start+8]
        if b not in ([0,1,0,1,0,1,0,1], [1,0,1,0,1,0,1,0]):
            continue
        if all(sum(bits[start+8+j*8+k] << k for k in range(8)) == ab
               for j, ab in enumerate(aa)):
            res.append(start)
    return res

def seq_bits(seed, n, left, out_tap, ta, tb, inv, pre_shift):
    reg = seed & 0x7F
    for _ in range(pre_shift):
        if left:
            fb = ((reg >> ta) ^ (reg >> tb)) & 1
            reg = ((reg << 1) | fb) & 0x7F
        else:
            fb = ((reg >> ta) ^ (reg >> tb)) & 1
            reg = (reg >> 1) | (fb << 6)
    out = []
    for k in range(n):
        if left:
            o = (reg >> out_tap) & 1
            fb = ((reg >> ta) ^ (reg >> tb)) & 1
            reg = ((reg << 1) | fb) & 0x7F
        else:
            o = (reg >> out_tap) & 1
            fb = ((reg >> ta) ^ (reg >> tb)) & 1
            reg = (reg >> 1) | (fb << 6)
        out.append(o ^ (1 if inv else 0))
    return out

seeds = lambda ch: [ch & 0x7F, (ch | 2) & 0x7F,
                    int(format(ch & 0x7F, '07b')[::-1], 2), (ch << 1) & 0x7F]

taps = [(a, b) for a in range(7) for b in range(7) if a != b]
scores = {}
for left, out_tap, (ta, tb), smode, inv, pre in itertools.product(
        (True, False), range(7), taps, range(4), (False, True), range(3)):
    total, good = 0, 0
    for ch, bits in dumps:
        for start in find_aa(bits)[:2]:
            pdu = start + 40
            if pdu + 16 > len(bits):
                continue
            chv = seeds(ch)[smode]
            s = seq_bits(chv, 16, left, out_tap, ta, tb, inv, pre)
            h0 = sum((bits[pdu+k] ^ s[k]) << k for k in range(8))
            h1 = sum((bits[pdu+8+k] ^ s[8+k]) << k for k in range(8))
            total += 1
            if (h0 & 0x30) == 0 and (h0 & 0xF) <= 7 and 6 <= (h1 & 0x3F) <= 63:
                good += 1
    if total >= 8:
        scores[(left, out_tap, (ta, tb), smode, inv, pre)] = (good, total)
for k, v in sorted(scores.items(), key=lambda x: -x[1][0])[:6]:
    print(v, k)
