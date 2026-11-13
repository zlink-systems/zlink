from pathlib import Path
import json
J=Path(__file__).parent
s=json.loads((J/'measurements.json').read_text())['summary']
lines=['| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Source CPU µs/msg before → after | Target CPU µs/msg before → after |','|---|---:|---:|---:|---:|---:|---:|']
for pattern in ['request-backpressure','send-saturation']:
 for impl in ['zlink-cpp','zlink-framework-cpp']:
  b,a=[s[f'csubmit-{pattern}-{impl}-{v}'] for v in ['before','after']]
  ratio=f"{b['F/B']:.4f} → {a['F/B']:.4f}" if 'F/B' in b else '—'
  cpu=lambda r:f"{b['endpoints'][r]['cpu_us']:.3f} → {a['endpoints'][r]['cpu_us']:.3f}"
  lines.append(f"| {pattern} / {impl} | {b['throughput']:,.1f} | {a['throughput']:,.1f} | {(a['throughput']/b['throughput']-1)*100:+.1f}% | {ratio} | {cpu('source')} | {cpu('target')} |")
lines+=['','| Pattern / implementation | Before range, msg/s | After range, msg/s |','|---|---:|---:|']
for pattern in ['request-backpressure','send-saturation']:
 for impl in ['zlink-cpp','zlink-framework-cpp']:
  b,a=[s[f'csubmit-{pattern}-{impl}-{v}'] for v in ['before','after']]
  rng=lambda r:f"{r['range'][0]:,.1f}–{r['range'][1]:,.1f}"
  lines.append(f'| {pattern} / {impl} | {rng(b)} | {rng(a)} |')
(J/'measurement-tables.md').write_text('\n'.join(lines)+'\n')
lines=['| Pattern | Implementation | Endpoint | Thread name / CPU rank | Before µs/msg | After µs/msg | Change |','|---|---|---|---|---:|---:|---:|']
for pattern in ['request-backpressure','send-saturation']:
 for impl in ['zlink-framework-cpp','zlink-cpp']:
  b,a=[s[f'csubmit-{pattern}-{impl}-{v}'] for v in ['before','after']]
  for role in ['source','target']:
   old=b['endpoints'][role]['ranked_cpu_us'];new=a['endpoints'][role]['ranked_cpu_us']
   for name in sorted(set(old)|set(new), key=lambda n:(n.split(' #')[0],int(n.split(' #')[1]))):
    bv,av=old.get(name,0),new.get(name,0)
    if max(bv,av)<.05:continue
    lines.append(f'| {pattern} | {impl} | {role} | `{name}` | {bv:.3f} | {av:.3f} | {av-bv:+.3f} |')
(J/'per-thread-table.md').write_text('\n'.join(lines)+'\n')
print((J/'measurement-tables.md').read_text())
