#!/usr/bin/env python3
import datetime,json,pathlib,statistics,sys
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l0e')
patt,size=sys.argv[1],int(sys.argv[2]); agg=J/f'raw/aggregates/{patt}-{size}.json'
x=json.loads(agg.read_text());rows={r['implementation']:r for r in x['rows']}
paths=list((J/f'raw/L0/{patt}-{size}').glob('run*/*/results.json'))
def row(impl):
 r=rows[impl]; rate=r['median']['throughput_per_second'];entries=[p for p in paths if p.parent.name.startswith(impl+'-') and json.loads(p.read_text())['cells'][0]['throughput_per_second']==rate]
 assert len(entries)>=1,(impl,rate)
 p=entries[0];return f"[{rate:,.0f}]({p.relative_to(J)}) · {r['runs']}회 · G5 {r['g5_status']} ({r['g5_spread_percent']:.2f}%)"
a=rows['zlink-framework-cpp']['median']['throughput_per_second'];b=rows['zlink-cpp']['median']['throughput_per_second'];ratio=a/b
text=f"\n- {datetime.datetime.now(datetime.timezone.utc).isoformat()} {patt} {size}: L0 {row('zlink-framework-cpp')}; binding {row('zlink-cpp')}; 비율 [{ratio:.4f}]({agg.relative_to(J)}) · {'0.95 통과' if ratio>=.95 else '미달'}.\n"
with (J/'progress.md').open('a') as f:f.write(text)
print(text)
