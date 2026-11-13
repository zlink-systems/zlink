import json,statistics
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/dfix')
records=json.loads((job/'raw/ab-results.json').read_text())
assert len(records)==32, f'Expected 32 runs, found {len(records)}'
assert len({(r['round'],r['pattern'],r['payload'],r['variant']) for r in records})==32
binding={('request-backpressure',1024):58990,('request-backpressure',4096):49454,('request-serial',1024):11303,('request-serial',4096):11320}
rows=['| Pattern / bytes | Round | A main msg/s | B fix msg/s | A / binding | B / binding |','|---|---:|---:|---:|---:|---:|']
medians=[]
for (pattern,payload),base in binding.items():
    subset=[r for r in records if (r['pattern'],r['payload'])==(pattern,payload)]
    for round in range(1,5):
        a=next(r for r in subset if r['round']==round and r['variant']=='A')['mps']
        b=next(r for r in subset if r['round']==round and r['variant']=='B')['mps']
        rows.append(f'| {pattern} / {payload} | {round} | {a:,.1f} | {b:,.1f} | {a/base:.3f} | {b/base:.3f} |')
    a=statistics.median(r['mps'] for r in subset if r['variant']=='A')
    b=statistics.median(r['mps'] for r in subset if r['variant']=='B')
    medians.append(dict(pattern=pattern,payload=payload,a=a,b=b,binding=base,ratio_a=a/base,ratio_b=b/base,change_percent=100*(b/a-1),pass_target=b/base>=.6))
    rows.append(f'| **{pattern} / {payload}** | **median** | **{a:,.1f}** | **{b:,.1f}** | **{a/base:.3f}** | **{b/base:.3f}** |')
text='\n'.join(rows)+'\n\n'
text+='| Cell | B versus A median | B / binding ≥ 0.6 |\n|---|---:|---|\n'
for r in medians:
    text+=f"| {r['pattern']} / {r['payload']} | {r['change_percent']:+.2f}% | {'yes' if r['pass_target'] else 'no'} |\n"
text+=f"\nMinimum sampled MemAvailable during the 32 queued/run intervals: {min(r['mem_min_gib'] for r in records):.2f} GiB (limit: 12 GiB; no aborts).\n"
(job/'raw/ab-medians.json').write_text(json.dumps(medians,indent=2)+'\n')
(job/'ab-table.md').write_text(text)
print(text)
