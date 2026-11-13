#!/usr/bin/env python3
import json,pathlib,statistics,sys
root=pathlib.Path(sys.argv[1]);out=[]
clk=100
import os
clk=os.sysconf('SC_CLK_TCK')
for result_path in sorted(root.glob('run*/*/results.json')):
    cell=result_path.parent
    bpath=cell/'proc-before.json'; apath=cell/'proc-after.json'
    if not bpath.exists() or not apath.exists(): continue
    try:
        result=json.loads(result_path.read_text()); before=json.loads(bpath.read_text()); after=json.loads(apath.read_text())
        row=result['cells'][0]; msgs=int(row.get('completed',0))
        values={}
        for role in ('source','target'):
            b=before.get(role,{}).get('threads',{});a=after.get(role,{}).get('threads',{})
            ticks=sum(max(0,int(a.get(tid,{}).get('ticks',0))-int(b.get(tid,{}).get('ticks',0))) for tid in set(b)|set(a))
            values[role+'_us_per_msg']=(ticks*1_000_000/clk/msgs) if msgs else None
        out.append({'run':result_path.parts[-3],'cell':cell.name,'messages':msgs,**values,'source_pid':before.get('source',{}).get('pid'),'target_pid':before.get('target',{}).get('pid')})
    except (KeyError,ValueError,TypeError): continue
(root/'cpu-per-cell.json').write_text(json.dumps(out,indent=2)+'\n')
print(f'cpu cells: {len(out)} -> {root}/cpu-per-cell.json')
for r in out: print(r['run'],r['cell'],r['messages'],r['source_us_per_msg'],r['target_us_per_msg'])
