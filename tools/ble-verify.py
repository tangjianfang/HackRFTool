# 胜出约定的 CRC24 全量验证
import glob

def seq_bits(seed, n, pre_shift=1):
    reg = seed & 0x7F
    for _ in range(pre_shift):
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
    out = []
    for k in range(n):
        o = reg & 1
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
        out.append(o)
    return out

def crc24(data_bits):
    crc = 0x555555
    for i in range(0, len(data_bits) - 7, 8):
        byte = sum(data_bits[i+k] << k for k in range(8))
        for k in range(8):
            bit = (byte >> k) & 1
            fb = (crc & 1) ^ bit
            crc >>= 1
            if fb:
                crc ^= 0x00065B
    return crc & 0xFFFFFF

ok, total, names = 0, 0, []
for p in sorted(glob.glob(r"C:/tjf/github/HackRFTool/out/build/x64-release/Release/ble-bits-*.txt")):
    head = open(p).read(60)
    ch = int(head.split("ch=")[1].split()[0])
    bits = [1 if c == '1' else 0 for c in open(p).read().split("bits=")[1].strip()]
    aa = [0xD6,0xBE,0x89,0x8E]
    for start in range(0, min(len(bits)-500, 4000)):
        b = bits[start:start+8]
        if b not in ([0,1,0,1,0,1,0,1], [1,0,1,0,1,0,1,0]):
            continue
        if not all(sum(bits[start+8+j*8+k] << k for k in range(8)) == ab
                   for j, ab in enumerate(aa)):
            continue
        pdu = start + 40
        if pdu + 70*8 > len(bits):
            continue
        s = seq_bits(ch, 70*8)
        dbits = [bits[pdu+k] ^ s[k] for k in range(70*8)]
        h0 = sum(dbits[k] << k for k in range(8))
        h1 = sum(dbits[8+k] << k for k in range(8))
        ln = h1 & 0x3F
        total += 1
        if crc24(dbits[:(2+ln)*8]) == sum(dbits[(2+ln)*8+k] << k for k in range(24)):
            ok += 1
            typ = h0 & 0xF
            mac = sum(dbits[6*8+j*8+k] << (8*j+k) if False else 0 for j in range(0) for k in range(0))
            name = ""
            i = 48   # AD 区起点（6B AdvA 后）
            end = (2+ln)*8
            while i + 8 <= end:
                alen = sum(dbits[i+k] << k for k in range(8))
                if alen == 0 or i + (alen+1)*8 > end:
                    break
                atype = sum(dbits[i+8+k] << k for k in range(8))
                if atype == 0x09:
                    name = "".join(chr(sum(dbits[i+16+t*8+k] << k for k in range(8)))
                                   for t in range(alen-1))
                i += (alen+1)*8
            names.append(f"type={typ} len={ln} name={name!r}")
print(f"CRC PASS: {ok}/{total}")
for n in names[:12]:
    print(" ", n)
