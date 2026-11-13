import re,sys,collections
txt=open(sys.argv[1]).read(); want=sys.argv[2]; msgs=float(sys.argv[3])
pat=re.compile(r'@'+want+r'\[(\d+), \n(.*?)\]: (\d+)',re.S)
agg=collections.defaultdict(lambda: collections.Counter()); tot=collections.Counter()
skip=('__malloc','malloc','operator new','__libc','calloc','realloc','pthread_mutex','__lll','std::','__gnu','0x','futex','syscall','__GI','___pthread')
for pid,stack,n in pat.findall(txt):
    n=int(n); tot[pid]+=n
    frames=[l.strip() for l in stack.strip().split('\n')]
    site='?'
    for fr in frames:
        if not fr.startswith(skip):
            site=re.sub(r'\(.*','',fr); break
    agg[pid][site]+=n
for pid in tot:
    print(f'== pid {pid} total {tot[pid]} ({tot[pid]/msgs:.2f}/msg)')
    for s,n in agg[pid].most_common(12): print(f'{n/msgs:7.2f}/msg  {s[:150]}')
