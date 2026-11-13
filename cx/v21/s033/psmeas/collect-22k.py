#!/usr/bin/env python3
"""Window-A only capture: schedstat, perf counters and context switches."""
import argparse, csv, json, pathlib, subprocess, time, urllib.request

p=argparse.ArgumentParser()
for key in ('source','target','source-url','target-url','out'): p.add_argument('--'+key,required=True)
a=p.parse_args(); out=pathlib.Path(a.out); out.mkdir(parents=True,exist_ok=True)
pids={'source':int(a.source),'target':int(a.target)}
def stats(url):
    with urllib.request.urlopen(url+'/bench/stats',timeout=5) as r: return json.load(r)
def snap():
    result={'time':time.monotonic(),'stats':{r:stats(getattr(a,r+'_url')) for r in pids},'processes':{}}
    for role,pid in pids.items():
        threads={}
        for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
            try:
                status={line.split(':',1)[0]:line.split(':',1)[1].strip() for line in (t/'status').read_text().splitlines() if ':' in line}
                threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'voluntary':int(status['voluntary_ctxt_switches']),'involuntary':int(status['nonvoluntary_ctxt_switches'])}
            except (FileNotFoundError,KeyError): pass
        result['processes'][role]={'pid':pid,'threads':threads}
    return result
assert stats(a.source_url)['phase']=='active'
before=snap(); tids=','.join(tid for role in ('target','source') for tid in before['processes'][role]['threads'])
(out/'pids.tsv').write_text(f"source\t{pids['source']}\ntarget\t{pids['target']}\n")
(out/'A-target-before.tsv').write_text(''.join(f"{tid}\t{v['comm']}\t{v['runtime_ns']}\t{v['voluntary']}\t{v['involuntary']}\n" for tid,v in before['processes']['target']['threads'].items()))
(out/'A-source-before.tsv').write_text(''.join(f"{tid}\t{v['comm']}\t{v['runtime_ns']}\t{v['voluntary']}\t{v['involuntary']}\n" for tid,v in before['processes']['source']['threads'].items()))
perf=['perf','stat','--per-thread',f'--tid={tids}','-e','instructions,cycles,msr/aperf/,msr/mperf/,task-clock,syscalls:sys_enter_sendto','-x,','-o',str(out/'A-perfstat.csv'),'--','sleep','5']
proc=subprocess.Popen(perf,stdout=subprocess.DEVNULL,stderr=(out/'A-perfstat.stderr').open('w'))
proc.wait()
if proc.returncode: raise SystemExit(f'perf stat exited {proc.returncode}; see {out}/A-perfstat.stderr')
after=snap()
samples=[before,after]
(out/'A-target-after.tsv').write_text(''.join(f"{tid}\t{v['comm']}\t{v['runtime_ns']}\t{v['voluntary']}\t{v['involuntary']}\n" for tid,v in after['processes']['target']['threads'].items()))
(out/'A-source-after.tsv').write_text(''.join(f"{tid}\t{v['comm']}\t{v['runtime_ns']}\t{v['voluntary']}\t{v['involuntary']}\n" for tid,v in after['processes']['source']['threads'].items()))
(out/'samples.json').write_text(json.dumps(samples))
msgs=after['stats']['source']['completed']-before['stats']['source']['completed']
if msgs<=0: raise SystemExit('no completed messages in window A')
(out/'windows.json').write_text(json.dumps({'A_messages':msgs,'A_seconds':after['time']-before['time'],'A_rate':msgs/(after['time']-before['time'])},indent=2)+'\n')
print(f"captured {out}; messages={msgs}; rate={msgs/(after['time']-before['time']):.0f}/s")
