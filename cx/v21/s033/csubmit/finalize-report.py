import json,pathlib,statistics
j=pathlib.Path(__file__).resolve().parent
data=json.loads((j/'summary.json').read_text());rows=data['records'];summ=data['summary']
def get(pattern,variant):return next(r for r in summ if r['pattern']==pattern and r['variant']==variant)
a=get('request-backpressure','main');b=get('request-backpressure','branch')
drop=1-b['submitter_cpu_us_per_completed_median']/a['submitter_cpu_us_per_completed_median']
cpu_pass=drop>=.30
ratios_pass=all(get(p,'branch')['ratio_of_medians']>get(p,'main')['ratio_of_medians'] for p in ('request-backpressure','send-saturation'))
paired_pass=all(get(p,'branch')['median_run_ratio']>get(p,'main')['median_run_ratio'] for p in ('request-backpressure','send-saturation'))
status=lambda v:'PASS' if v else 'NOT MET'
lead=f'''# C++ source submit path (#3867) — partial implementation

The complete job is **not finished**: Part A.1/A.2/A.4 and the native close-on-lane subitem require the scope/design decisions below. Changes remain uncommitted.

| Done criterion | Result | Evidence |
|---|---|---|
| Request 1K source submitter CPU decreases ≥30% | **{status(cpu_pass)}** | Main {a['submitter_cpu_us_per_completed_median']:.3f} → branch {b['submitter_cpu_us_per_completed_median']:.3f} CPU µs/completed message; reduction **{drop*100:.2f}%** (three-run medians). |
| Both 1K Framework/binding throughput medians rise | **{status(ratios_pass)}**, ratio of throughput medians; **{status(paired_pass)}**, median of paired run ratios | Both definitions and raw throughput medians are reported below. |
| Full C++ non-install tests and format | **PASS** | 309/309 CTest entries; C++ format and diff checks pass. |

'''
s=(j/'report.md').read_text();start=s.index('Baseline:');s=lead+s[start:]
lines=['## Measurements','',
 'Condition B: source CPUs 0–7, target 8–15. Frozen main and branch use the same Core/binding 1.23.0 packages, Release `-O3 -DNDEBUG`, probes off and identical runner options. Each cell has 30 s warm-up and 5 s active time; three runs per variant and pattern. Binding and Framework run back-to-back; main and branch alternate. All cells hold exclusive `flock -o /root/jobs/fx/cx/measure.lock`; builds/tests hold it shared. `measure.sh`, `run-B.sh`, `runner-common.sh` and `collect.py` are copies of the provided cdrain harness, relocated into this job.', '',
 'The submitter TID is recorded once at its first Framework submit. `/proc/<pid>/task/<tid>/schedstat` is sampled in the same approximately three-second active window as the completed-message counters. CPU figures cover the complete source driver/submitter thread, including payload creation and completion reaping, as in the original profile. The primary denominator is the source completed-message delta; the target-received denominator is retained independently in `summary.json`. No per-message logging is added. All 24 cells have zero errors/abandoned/rejected messages and matching final completed/received counts.', '',
 '| Pattern | Variant | Binding median msg/s | Framework median msg/s | Ratio of medians | Median paired ratio | Submitter CPU µs/completed |',
 '|---|---|---:|---:|---:|---:|---:|']
for p in ('request-backpressure','send-saturation'):
 for v in ('main','branch'):
  r=get(p,v);lines.append(f"| {p} 1K | {v} | {r['binding_median']:,.1f} | {r['framework_median']:,.1f} | {r['ratio_of_medians']:.4f} | {r['median_run_ratio']:.4f} | {r['submitter_cpu_us_per_completed_median']:.3f} |")
lines+=['','| Pattern | Run | Variant | Binding msg/s | Framework msg/s | Framework/binding | Submitter TID | CPU µs/completed |','|---|---:|---|---:|---:|---:|---|---:|']
for r in rows:
 f=r['framework'];lines.append(f"| {r['pattern']} | {r['run']} | {r['variant']} | {r['binding']['throughput']:,.1f} | {f['throughput']:,.1f} | {r['ratio']:.4f} | {f['submitter_tid']} | {f['submitter_cpu_us_per_completed']:.3f} |")
lines+=['', 'Commands and artifacts: `bash measure.sh`, `python3 analyze.py`; `measure.log`, `measurement-summary.log`, `summary.json`, and `measure/<round>/<pattern>/<variant>/run1/<implementation>-<pattern>-1024/{results.json,source.log,target.log,capture/}`. Raw before/after/samples include the per-thread counters and source/target completed counts. Baseline/package hashes are in `versions.json`; final measured targets have no pending Ninja work (`bench-main-noop.log`, `bench-branch-noop.log`).', '']
if (j/'census-summary.json').exists():
 cs=json.loads((j/'census-summary.json').read_text())
 lines+=['## Allocation census','', 'The provided `salloc/census.sh`, its collector and `alloc_trace.so` still run. The relocated `run-census.sh` uses condition B, 10 s warm-up and 20 s active time with an eight-second capture, one run per variant. These instrumented throughput values are excluded from the three-run throughput results above. Heap calls count malloc/calloc/realloc; aligned allocation and direct mmap remain outside the provided counter.', '', '| Variant | Submitter TID | Completed in capture | Submitter heap calls/message | Whole-source heap calls/message |','|---|---|---:|---:|---:|']
 for r in cs:lines.append(f"| {r['variant']} | {r['tid']} | {r['completed_messages']:,} | {r['submitter_heap_calls_per_completed']:.3f} | {r['process_heap_calls_per_completed']:.3f} |")
 lines+=['', 'The Part A allocation target is ≤12 heap calls/message. Raw evidence: `census.log`, `census-summary.json`, `census/{main,branch}/run1/zlink-framework-cpp-request-backpressure-1024/capture/` and the corresponding allocator trace/map files. Command: `bash run-census.sh` under exclusive measurement locks.', '']
s+='\n'+'\n'.join(lines)
(j/'report.md').write_text(s)
print('CPU reduction',drop,'ratio-of-medians pass',ratios_pass,'paired-ratio median pass',paired_pass)
