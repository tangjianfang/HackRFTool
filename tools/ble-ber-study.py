# BLE BER 结构分析（#111 Phase A）：30 份比特转储 → 判别 H1 滑步/H2 均匀劣化/H3 不可行
# 用法: python tools/ble-ber-study.py
# 输出: 头部规范率 / CRC 距离分布 / 滑步探针（±bit 偏移重试）/ AD 结构中断点分布
import glob
import sys

def seq_bits(seed, n, pre_shift=1):
    # #109 破译约定：种子=信道号，预移 1，输出 bit0，反馈 bit2^bit6
    reg = seed & 0x7F
    for _ in range(pre_shift):
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
    out = []
    for _ in range(n):
        o = reg & 1
        fb = ((reg >> 2) ^ (reg >> 6)) & 1
        reg = ((reg << 1) | fb) & 0x7F
        out.append(o)
    return out

def crc24(data_bits):
    crc = 0x555555
    for i in range(0, len(data_bits) - 7, 8):
        byte = sum(data_bits[i + k] << k for k in range(8))
        for k in range(8):
            bit = (byte >> k) & 1
            fb = (crc & 1) ^ bit
            crc >>= 1
            if fb:
                crc ^= 0x00065B
    return crc & 0xFFFFFF

def read_lsb(bits, off, n):
    return sum(bits[off + i] << i for i in range(n) if off + i < len(bits))

def find_aa(bits):
    # 精确 AA（0xAA/0x55 前导 + D6 BE 89 8E LSB-first）
    aa = [0xD6, 0xBE, 0x89, 0x8E]
    out = []
    for s in range(len(bits) - 8 * 5 - 1):
        pre = read_lsb(bits, s, 8)
        if pre not in (0xAA, 0x55):
            continue
        if all(read_lsb(bits, s + 8 + j * 8, 8) == ab for j, ab in enumerate(aa)):
            out.append(s)
    return out

def dewhiten(bits, off, nbytes, ch):
    key = seq_bits(ch, nbytes * 8)
    frame = []
    for b in range(nbytes):
        v = read_lsb(bits, off + b * 8, 8)
        m = sum(key[b * 8 + k] << k for k in range(8))
        frame.append(v ^ m)
    return frame

AD_TYPES = set([0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0D,0x16,0xFF,0x19,0x1B,0x06])

def ad_break_point(payload):
    # AD 链走多远才出现不可能结构（len=0/越界/类型离谱）——中断点早=错误靠前
    i = 0
    while i + 1 < len(payload):
        ln = payload[i]
        if ln == 0 or i + 1 + ln > len(payload):
            return i
        if payload[i + 1] not in AD_TYPES:
            return i + 1
        i += 1 + ln
    return len(payload)   # 全程走通

def main():
    files = sorted(glob.glob(
        r"C:/tjf/github/HackRFTool/out/build/x64-release/Release/ble-bits-*.txt"))
    if not files:
        print("no dumps"); return 1
    stats = {"aa": 0, "hdr_ok": 0, "frames": 0, "crc_dist": [], "slip_hits": [],
             "klag_hits": [], "ad_break": [], "hdr_types": {}}
    keyall_cache = {}
    for p in files:
        head = open(p).read(60)
        ch = int(head.split("ch=")[1].split()[0])
        bits = [1 if c == '1' else 0
                for c in open(p).read().split("bits=")[1].strip()]
        for s in find_aa(bits):
            stats["aa"] += 1
            pdu = s + 40
            if pdu + 8 * 5 > len(bits):
                continue
            h = dewhiten(bits, pdu, 2, ch)
            t = h[0] & 0x0F
            stats["hdr_types"][t] = stats["hdr_types"].get(t, 0) + 1
            ln = h[1] & 0x3F
            rfu = (h[0] >> 4) & 0x0F   # ChSel/TxAdd/RxAdd/RFU——高 4 位中 RFU=0
            hdr_ok = (t in (0, 1, 2, 4, 6)) and 6 <= ln <= 37
            if hdr_ok:
                stats["hdr_ok"] += 1
                stats["frames"] += 1
                nbytes = 2 + ln + 3
                frame = dewhiten(bits, pdu, nbytes, ch)
                if len(keyall_cache.get(ch, ())) < nbytes * 8:
                    keyall_cache[ch] = seq_bits(ch, 40 * 1024)
                keyall = keyall_cache[ch]
                body = []
                for b in frame:
                    body += [(b >> k) & 1 for k in range(8)]
                calc = crc24(body[: (2 + ln) * 8])
                rx = (frame[2 + ln] | (frame[2 + ln + 1] << 8)
                      | (frame[2 + ln + 2] << 16))
                d0 = bin(calc ^ rx).count("1")
                stats["crc_dist"].append(d0)
                # 滑步探针：CRC 读位在 ±24bit 内滑窗重试（calc 不变=载荷前移/后移
                # 的删除/插入滑步会让真 CRC 出现在偏移处）
                best, best_off = d0, 0
                for off in range(-24, 25):
                    if off == 0:
                        continue
                    pos = pdu + (2 + ln) * 8 + off
                    if pos < 0 or pos + 24 > len(bits):
                        continue
                    key = seq_bits(ch, (2 + ln + 3) * 8)
                    rxv = 0
                    for k in range(24):
                        bit = bits[pos + k] ^ key[(2 + ln) * 8 + k]
                        rxv |= bit << k
                    dd = bin(calc ^ rxv).count("1")
                    if dd < best:
                        best, best_off = dd, off
                if best <= 2:
                    stats["slip_hits"].append((p.split("-")[-1], best, best_off))
                # k-lag 重对齐测试（mid-frame 滑步的决定性判据）：载荷区若在
                # header 后发生过一次 k 比特删除/插入，则真载荷比特落在
                # stream 位 j−k 处而白化键仍按 j 取——用 read(j−k)^key(j)
                # 重建载荷+CRC，k 命中时 CRC 必然精确通过
                for k in range(-16, 17):
                    if k == 0:
                        continue
                    okk = True
                    rec = []
                    for b in range(2, nbytes):
                        v = 0
                        for t in range(8):
                            pos = pdu + b * 8 + t - k
                            if pos < 0 or pos + 1 > len(bits):
                                okk = False
                                break
                            keyb = keyall[b * 8 + t]
                            v |= (bits[pos] ^ keyb) << t
                        if not okk:
                            break
                        rec.append(v)
                    if okk and len(rec) == nbytes - 2:
                        calc_k = crc24(
                            [frame[0], frame[1]] + rec[:ln])
                        # 帧体比特序列（header 用已验证值 + 重对齐载荷）
                        bodyk = []
                        for b in [frame[0], frame[1]] + rec[:ln]:
                            bodyk += [(b >> t) & 1 for t in range(8)]
                        calc_k = crc24(bodyk)
                        rx_k = (rec[ln] | (rec[ln + 1] << 8)
                                | (rec[ln + 2] << 16))
                        if calc_k == rx_k:
                            stats["klag_hits"].append(
                                (p.split("-")[-1], k))
                # AD 结构中断点（type 0/2/4：AdvA 6B 后 AD 链；SCAN_RSP=4 同构）
                if t in (0, 2, 4):
                    stats["ad_break"].append((ln, ad_break_point(frame[2 + 8:2 + ln])))
    import collections
    print("== AA 命中:", stats["aa"], " 头部规范:", stats["hdr_ok"],
          "（类型分布:", dict(sorted(stats["hdr_types"].items())), "）")
    if stats["crc_dist"]:
        cd = stats["crc_dist"]
        print("== CRC 距离@0 偏移: min=%d mean=%.1f max=%d（随机期望 12）"
              % (min(cd), sum(cd) / len(cd), max(cd)))
        print("   直方:", dict(sorted(collections.Counter(cd).items())))
    print("== 滑步探针（min 距离≤2 即实锤）:", stats["slip_hits"] or "无")
    print("== k-lag 重对齐（mid-frame 滑步实锤）:", stats["klag_hits"] or "无")
    if stats["ad_break"]:
        print("== AD 中断点（payload_len, 走通字节数）:", stats["ad_break"])
    return 0

if __name__ == "__main__":
    sys.exit(main())
