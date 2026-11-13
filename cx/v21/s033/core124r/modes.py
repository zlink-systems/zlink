import re,sys,pathlib
C=pathlib.Path('/root/jobs/fx/cx/v21/s033/core124b/confirm')
rows=[l.split('|') for l in open('/root/jobs/fx/cx/v21/s033/core124b/report.md') if re.match(r'\| (multi|single) ',l)]
col={'latency':3,'latency_p95':4,'latency_p99':5,'throughput':0}
def runs(v,s,p,t,size):
    out=[];cur=False
    for l in open(C/f'{v}-{s}-{p}-{t}.txt'):
        m=re.search(r'Testing (\S+) \| (\d+)B:',l)
        if m: cur=(m.group(1)==t and m.group(2)==size); continue
        if cur and re.match(rf'\s*\| {size}B ',l):
            out.append(float(re.findall(r'([\d.]+) (?:Kmsg/s|Kops/s|ms)',l)[col.get(met,0)] if True else 0))
    return out
for r in rows:
    s,p,t,size,met=[x.strip() for x in r[1:6]]; r5=float(r[8])
    if not size.isdigit(): continue
    bad = r5>=2 if 'latency' in met else r5<=0.5
    if not bad: continue
    vals={v:runs(v,s,p,t,size) for v in ('B1','C','B2')}
    print(f"{s} {p} {t} {size} {met} r5={r5}: "+' '.join(f"{v}={[round(x,2) for x in a]}" for v,a in vals.items()))
