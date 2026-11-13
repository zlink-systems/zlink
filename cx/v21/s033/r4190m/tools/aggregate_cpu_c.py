#!/usr/bin/env python3
import json, os, pathlib, sys
root=pathlib.Path(sys.argv[1]); clk=os.sysconf('SC_CLK_TCK'); out=[]
for result_path in sorted(root.glob('run*/*/results.json')):
    cell=result_path.parent
    try:
        row=json.loads(result_path.read_text())['cells'][0]
        msgs=int(row.get('extra',{}).get('completed',0))
        before=json.loads((cell/'proc-before.json').read_text())
        after=json.loads((cell/'proc-after.json').read_text())
        values={}; ticks_by_role={}
        for role in ('source','target'):
            b=before.get(role,{}).get('threads',{}); a=after.get(role,{}).get('threads',{})
            ticks=sum(max(0,int(a.get(tid,{}).get('ticks',0))-int(b.get(tid,{}).get('ticks',0))) for tid in set(b)|set(a))
            ticks_by_role[role]=ticks
            values[role+'_us_per_msg']=ticks*1_000_000/clk/msgs if msgs else None
        out.append({'run':result_path.parts[-3],'cell':cell.name,'messages':msgs,**values,'source_pid':before.get('source',{}).get('pid'),'target_pid':before.get('target',{}).get('pid'),'ticks':ticks_by_role,'snapshot_complete':before.get('source',{}).get('pid') is not None and all(ticks_by_role.values())})
    except (KeyError,ValueError,TypeError,ZeroDivisionError):
        continue
(root/'cpu-per-cell.json').write_text(json.dumps(out,indent=2)+'\n')
print(f'cpu cells: {len(out)} -> {root}/cpu-per-cell.json')
