import json
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/jl57bp')
rows=json.loads((job/'raw/results-summary.json').read_text())
parts=['\n### Counted 실행 — active 5초\n\n| 경로 | payload | submitted | completed | received | capacity / timeout | peak in-flight | raw |\n|---|---:|---:|---:|---:|---:|---:|---|']
for r in rows:
 if r['kind']!='counted' or r['phase']!='active':continue
 parts.append('| '+r['mode']+' | '+str(r['size'])+' | '+f"{r['submitted']:,}"+' | '+f"{r['completed']:,}"+' | '+f"{r['received']:,}"+' | '+f"{r['capacity_unavailable']:,} / {r['timeouts']:,}"+' | '+f"{r['peak_in_flight']:,}"+' | [result]('+r['raw']+') |')
parts.append('\n### 같은 counted 실행 — 내부 warmup 10초\n\n| 경로 | payload | submitted | completed | capacity / timeout | peak in-flight | raw |\n|---|---:|---:|---:|---:|---:|---|')
for r in rows:
 if r['kind']!='counted' or r['phase']!='warmup':continue
 parts.append('| '+r['mode']+' | '+str(r['size'])+' | '+f"{r['submitted']:,}"+' | '+f"{r['completed']:,}"+' | '+f"{r['capacity_unavailable']:,} / {r['timeouts']:,}"+' | '+f"{r['peak_in_flight']:,}"+' | [warmup]('+r['raw']+') |')
(job/'raw/tables.md').write_text('\n'.join(parts)+'\n')
