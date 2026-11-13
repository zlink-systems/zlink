#!/usr/bin/env python3
import json, pathlib, sys, time
out=pathlib.Path(sys.argv[1]); stage=sys.argv[2]; pids=[int(x) for x in sys.argv[3:]]
rows={}
def descendants(pid):
    found={pid}; todo=[pid]
    while todo:
        p=todo.pop()
        try: kids=[int(x) for x in pathlib.Path(f'/proc/{p}/task/{p}/children').read_text().split()]
        except (OSError, ValueError): kids=[]
        for kid in kids:
            if kid not in found: found.add(kid); todo.append(kid)
    return found
for root_index,root in enumerate(pids):
    for pid in descendants(root):
        taskdir=pathlib.Path(f'/proc/{pid}/task')
        try: tids=list(taskdir.iterdir())
        except OSError: continue
        for t in tids:
            tid=int(t.name)
            try:
                comm=(t/'comm').read_text().strip()
                sched=(t/'schedstat').read_text().split()
                status=(t/'status').read_text().splitlines()
                ctxt={}
                for line in status:
                    if line.startswith(('voluntary_ctxt_switches:', 'nonvoluntary_ctxt_switches:')):
                        key,val=line.split(':',1); ctxt[key]=int(val)
                    elif line.startswith('Cpus_allowed_list:'):
                        ctxt['cpus_allowed_list']=line.split(':',1)[1].strip()
                rows[str(tid)]={'pid':pid,'root_index':root_index,'tid':tid,'comm':comm,'runtime_ns':int(sched[0]),**ctxt}
            except (OSError, ValueError, IndexError): pass
out.parent.mkdir(parents=True,exist_ok=True)
(out.parent/f'{stage}.json').write_text(json.dumps({'time_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns(),'stage':stage,'roots':pids,'threads':rows},indent=2)+'\n')
