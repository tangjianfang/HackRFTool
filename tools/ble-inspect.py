# 透视去白化头部：标准约定下 type/len 分布是否合理
import glob

def gen_seq(seed, n, shift_left=True, out_tap=6, tap_a=6, tap_b=3, inv=False):
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
        seq[k] = out ^ (1 if inv else 0)
    return seq

files = sorted(glob.glob(r"C:/tjf/github/HackRFTool/out/build/x64-release/Release/ble-bits-*.txt"))[:10]
for p in files:
    head = open(p).read(60)
    ch = int(head.split("ch=")[1].split()[0])
    q = float(head.split("q=")[1].split()[0])
    bits = [1 if c == '1' else 0 for c in open(p).read().split("bits=")[1].strip()]
    # 找 AA
    aa = [0xD6,0xBE,0x89,0x8E]
    for start in range(0, min(len(bits)-48*8, 4000)):
        b = bits[start:start+8]
        if b not in ([0,1,0,1,0,1,0,1], b[::-1]):
            continue
        ok = all(sum(bits[start+8+j*8+k] << k for k in range(8)) == ab
                 for j, ab in enumerate(aa))
        if not ok:
            continue
        pdu = start + 40
        seq = gen_seq(ch, 16)
        h0 = sum((bits[pdu+k] ^ seq[k]) << k for k in range(8))
        h1 = sum((bits[pdu+8+k] ^ seq[8+k]) << k for k in range(8))
        print(f"{p.split('ble-bits-')[1]:>16} ch={ch} q={q:.2f} type={h0 & 0xF} "
              f"txadd={(h0>>6)&1} len={h1 & 0x3F}")
        break
