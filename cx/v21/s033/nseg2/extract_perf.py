#!/usr/bin/env python3
import json, pathlib, re, subprocess, sys
base=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
w=json.loads((base/'window.json').read_text())
pids={str(x) for x in w['pids'].values()}
tids=set(pids)
for side in ('thread_ticks_start','thread_ticks_end'):
    for group in w.get(side,[]): tids.update(group.keys())
tid_re=re.compile(r'(?<!\d)(?:'+ '|'.join(map(re.escape,tids)) +r')(?!\d)')
cmd=['perf','script','-i',str(base/'sched.data'),'--ns','-F','comm,pid,tid,cpu,time,event,trace']
with out.open('w') as f:
    p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,errors='replace')
    for line in p.stdout:
        if 'raw_syscalls:sys_' in line:
            m=re.search(r' (\d+)/(\d+) \[\d+\] [0-9.]+:',line)
            if m and m.group(1) in pids and re.search(r'NR (?:44|45|46|47|202|232|281|1|0|7)(?: |$)',line): f.write(line)
        elif 'sched:sched_' in line:
            if tid_re.search(line): f.write(line)
    rc=p.wait()
    if rc: raise SystemExit(rc)
print(out, out.stat().st_size)
