from pathlib import Path
import json,statistics,datetime
J=Path(__file__).parent;s=json.loads((J/'summary.json').read_text());M=statistics.median
names=['C++ binding echo','Real envelope creation, encode and decode','Real raw port + public task/admission/completion bridge','Pending entry + deadline index + registry completion dispatcher','Scheduled operation owner lane','Target mailbox and ingress permit','Application executor','Typed serializer, DI, handler scope and filter invocation','Canonical service-wire application and request/reply wrappers','Full public API and production RouteMesh runtime']
def cpu(row,side,key):return row['groups'].get(side,{}).get(key,0)
def thread_vector(row,side):
 # Identity varies per process; same-name threads are ranked by CPU demand in each run.
 ranks=[]
 for c in row['runs']:
  if 'roles' not in c:continue
  d={}
  for t in c['roles'][side]['threads'].values():d.setdefault(t['role'],[]).append(t['us'])
  for name in d:d[name].sort(reverse=True)
  ranks.append(d)
 keys=set().union(*(d for d in ranks));parts=[]
 for name in sorted(keys):
  count=max(len(d.get(name,[])) for d in ranks)
  values=[M(d.get(name,[])[i] if i<len(d.get(name,[])) else 0 for d in ranks) for i in range(count)]
  values=[v for v in values if v>=.05]
  if values:parts.append(f'`{name}` '+','.join(f'{v:.2f}' for v in values))
 return '; '.join(parts)
lines=['# C++ bottom-up request layer ladder — 0.33.0 / #1639','', 'Source: `/root/worktree/zlink-3867s`, HEAD `c8869b63b19c8504a727459c766bf3ccee995708`. Product files are unchanged. All new sources, builds, runners and data are under this directory. No sub-agents, other models, docs synchronization, repository mutation, GitHub access, pull or fetch was used.','',
'## Measurement and interpretation','',
'Source CPUs 0–7; target CPUs 8–15. Each reported rung has three independent process pairs, 30 s warm-up and 20 s active time. Throughput is the median of the complete active phases. Per-thread CPU is the median of the 100 ms `/proc/<pid>/task/<tid>/schedstat` timelines over a 12 s interior window, divided by completed requests in that same window. CPU time is not latency. Same-name threads are ranked by CPU within each run before taking medians; all TIDs and each run are in `threads.csv` and `summary.json`. Independently taken medians need not sum exactly. A thread that retires between snapshots can lose its last sampling interval.','',
'Builds use the shared measure lock and `cpp-build-slot.sh`, with one compiler per build (at most two build slots; below the requested parallelism ceiling of three). Measurements and perf use the exclusive `flock -o /root/jobs/fx/cx/measure.lock`. The collector checks MemAvailable >= 12 GiB at every snapshot. Endpoint and perf cleanup uses SIGTERM/SIGINT only.','',
'The existing `cthin/run-B.sh`, `runner-common.sh` and `collect.py` were copied locally. Rung 0 removes Framework from the executable link closure and uses a small POSIX HTTP listener for control; the binding request/echo pump, Protobuf payload, 29-byte body validation, metrics, phases and count checks are copied from the repository benchmark. `nm` checks there are no Framework runtime symbols in this executable. Rungs 1–8 use the same local control listener; rung 9 runs the real public benchmark and its real host. Production libraries were copied from the current `csubmit/build`; all 13 recorded source hashes match current sources (`manifest.json`). The benchmark metadata still labels Framework 0.32.0; HEAD and source hashes, not that inherited string, identify this 0.33.0 investigation.','',
'## Baseline check: the requested window differs from the published cell','',
'`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:1012` returns an unlimited window for `request-backpressure`; window 100 belongs to `request-window`. This ladder explicitly forces 100 without changing the public request path. `LADDER_UNBOUNDED=1` enables the separate comparison. The published 246,826.1 binding and 49,602.0 Framework values are unlimited-window, 5 s active references; these new active phases last 20 s. A 5% reproduction claim must state which of these conditions it compares.','',
'| Case | Runs | msg/s, median | Difference from published same implementation |','|---|---:|---:|---:|']
for key,title,ref in [('binding-reference','Unchanged existing binding benchmark, unlimited',246826.1),('rung0','Standalone binding, window 100',246826.1),('rung0-unbounded','Standalone binding, unlimited',246826.1),('binding-reference-window100','Existing benchmark binding path, window 100',246826.1),('rung9-unbounded','Full public Framework, unlimited',49602.0)]:
 if key in s:
  r=s[key];lines.append(f"| {title} | {len(r['runs'])} | {r['throughput']:,.1f} | {100*(r['throughput']/ref-1):+.2f}% |")
lines+=['','<!-- BASELINE_CONCLUSION -->','','## Cumulative rungs','',
'The message path remains binding request/reply. The additions are actual Framework functions, linked from the copied production archive and instantiated from repository headers. The job code supplies glue and configurations; it does not reimplement the codec, registry, lane, mailbox, permit, executor or handler invocation. Intermediate rungs are diagnostic component compositions, not alternate supported Framework APIs or production optimizations.','',
'Rung 2 changes to the real Framework benchmark’s task/admission observation loop and the real raw port, adding a dedicated source completion poller. Rung 3 separates the logical registry cost from that task bridge. Callback publication is measured with registry terminal completion rather than with unregister-only dummy work. Rung 4 adds the real scheduled owner; the ownerless rung 3 drives its real deadline management from the poller at `next_deadline()`. This keeps completed index references bounded.','',
'The service-wire wrapper is added after the leaf operations so its packing cost is separately visible. In the real path it encloses the envelope and registry submission; it is not a second transport. Rung 9 restores the full composition, including peer/liveness checks, public result projections, real ingress/management turns and dispatch/reply adapters. The explicit `request_to_node` RID does not use the ChannelName selector; adding a generic route-selection algorithm would measure another API. The integration residual at rung 9 is disclosed separately below.','',
'F/B below divides each rung by the standalone window-100 binding median. Δ CPU shows the change in total CPU demand at each endpoint; it is not a latency delta. Δ wall is the change in inverse throughput (µs/message).','',
'| Rung | Added real work | Runs | msg/s | F/B | Rate Δ | Δ wall µs | Source CPU µs/msg | Target CPU µs/msg | Δ CPU S/T µs |','|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|']
prev=None
for n,name in enumerate(names):
 key=f'rung{n}'
 if key not in s:continue
 r=s[key];t=r['throughput'];st=r['endpoint_us']['source'];tt=r['endpoint_us']['target']
 rate='—' if prev is None else f'{100*(t/prev[0]-1):+.1f}%';wall='—' if prev is None else f'{1e6/t-1e6/prev[0]:+.3f}';dc='—' if prev is None else f'{st-prev[1]:+.3f}/{tt-prev[2]:+.3f}'
 lines.append(f"| {n} | {name} | {len(r['runs'])} | {t:,.1f} ({min(x['throughput'] for x in r['runs']):,.0f}–{max(x['throughput'] for x in r['runs']):,.0f}) | {t/s['rung0']['throughput']:.4f} | {rate} | {wall} | {st:.3f} | {tt:.3f} | {dc} |")
 prev=t,st,tt
lines+=['','### Per-thread CPU µs/message','', 'Every value >= 0.05 µs/message is shown. Comma-separated values are distinct threads sharing the displayed name, not a summed pool. The complete unrounded rows, including idle threads, are in `threads.csv`.','', '| Rung | Source threads | Target threads |','|---:|---|---|']
for n in range(10):
 key=f'rung{n}'
 if key in s:lines.append(f"| {n} | {thread_vector(s[key],'source')} | {thread_vector(s[key],'target')} |")
lines+=['','<!-- TOP_RUNG_ANALYSIS -->','','<!-- RECONCILIATION -->','','## Validation and raw data','',
'- `src/`: standalone and component harnesses; `build.py` and `build-config.json`: exact compile/link dependency closure; `lib/`: copied production libraries.','- `raw/rung{0..9}/run{1..3}/<implementation>-request-backpressure-1024/`: result, endpoint logs, target snapshot and `capture/{before,after,samples,meta}.json`.','- `raw/binding-reference*`, `raw/rung0-unbounded`, `raw/rung9-unbounded`: independent baseline comparisons.','- `raw/perf-rung*/run1/.../capture/`: perf data, commands, stderr and decoded stack caches. `profiles.json`: CPU-normalized per-thread exclusive categories and inclusive/self function estimates. Inclusive scopes overlap and must not be summed.','- `summary.json`, `threads.csv`, `quality.json`, `manifest.json`: run-level rates, per-thread CPU, counter/affinity checks and source/library identity.','- `negative-count.json` and the positive/negative results in `quality.json`: the existing counter checker accepts the valid capture and rejects a deliberately missing target receive.','- `smoke/`, `smoke.log`, `smoke-final.log`: setup checks, including the first harness-only executor/mailbox claim failure and the corrected real mailbox-ready/settlement callback path. This was not a product-code failure. Failed setup data is excluded from all medians.','',
'## Limits','',
'<!-- LIMITS -->','']
(J/'report.md').write_text('\n'.join(lines))
print('wrote report tables')
