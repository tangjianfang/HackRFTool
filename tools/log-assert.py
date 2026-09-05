# 日志断言工具（#60）：读 jsonl 断言事件序列——替代视觉识别的验收入口
# 用法: python tools/log-assert.py <jsonl> "LIFE:app.start,RADIO:tune,UI:cmd"
#   每项 "cat:event"，全部存在 → PASS（退出码 0）；缺任一 → FAIL（退出码 1）
#   附加: --tail N 打印最后 N 条
import sys, json

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tail_n = 0
    if '--tail' in sys.argv:
        i = sys.argv.index('--tail')
        tail_n = int(sys.argv[i + 1])
    path, wants = args[0], args[1].split(',')
    seq = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            try:
                e = json.loads(line)
                seq.append((e.get('cat', '?'), e.get('event', '?')))
            except json.JSONDecodeError:
                seq.append(('<bad>', line.strip()[:30]))
    ok = True
    for w in wants:
        cat, ev = (w.split(':', 1) + [''])[:2]
        hit = (cat, ev) in seq
        ok = ok and hit
        print(('PASS' if hit else 'FAIL'), w)
    if tail_n:
        print('--- tail', tail_n)
        for c, e in seq[-tail_n:]:
            print(' ', c, e)
    print('ASSERT:', 'ALL PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)

main()
