import json,re,statistics,pathlib,xml.etree.ElementTree as ET
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain2')
rows=json.loads((job/'summary.json').read_text())
def selected(pattern,variant):return [r for r in rows if r['pattern']==pattern and r['variant']==variant]
def med(pattern,variant,key='framework'):
 rs=selected(pattern,variant);assert len(rs)==3
 return statistics.median(r[key]['throughput'] for r in rs)
def spot(variant):return statistics.median(r['spot']['p99_us'] for r in selected('pm',variant))
checks=[]
for pattern in ('request-backpressure','send-saturation'):
 m,b=med(pattern,'main'),med(pattern,'branch');checks.append((pattern,b>=m*.99,m,b,m*.99,f'{100*(b/m-1):+.2f}%'))
m,b=med('pm','main'),med('pm','branch');checks.append(('PM hot throughput',b>=m*4,m,b,m*4,f'{b/m:.3f}×'))
m,b=spot('main'),spot('branch');checks.append(('PM Spot p99 (µs)',b<=m*2,m,b,m*2,f'{b/m:.3f}×'))
met=all(r[1] for r in checks)
lines=['## Acceptance criteria','', '| Criterion | Result | Main median | Branch median | Required branch value | Change / ratio |','|---|---|---:|---:|---:|---:|']
for name,ok,m,b,threshold,delta in checks:
 relation='≤' if 'p99' in name else '≥'
 lines.append(f'| {name} | {"MET" if ok else "UNMET"} | {m:,.3f} | {b:,.3f} | {relation} {threshold:,.3f} | {delta} |')
lines+=['','Cheap-cell and hot-cell values are Framework msg/s; Spot values are the median of the three per-run nearest-rank p99 values. Cheap acceptance compares Framework medians, not Framework/binding ratios. All cells retain the same scripts, condition B and three-round ordering as the previous job.','']
p=job/'report.md';s=p.read_text()
s=re.sub(r'^Status:.*$',f'Status: acceptance {"MET" if met else "UNMET"}; changes remain uncommitted on perf/1639-cpp-multi-drain, based on HEAD 5a585edc93. No commit or push. The working tree was clean at job start.',s,count=1,flags=re.M)
pos=s.index('Spec references:');s=s[:pos]+'\n'.join(lines)+'\n'+s[pos:]
testlog=(job/'test-all.log').read_text();assert '100% tests passed, 0 tests failed out of 309' in testlog
suite=ET.parse(job/'test-all.xml').getroot();assert len(suite.findall('.//testcase'))==309 and not suite.findall('.//skipped') and not suite.findall('.//failure')
elapsed=re.search(r'Total Test time \(real\) =\s*(\S+) sec',testlog).group(1)
s=s.replace('- Full verification: `verify-final.sh`; same 309 non-install tests as the previous job.',f'- Full verification: `verify-final.sh`; **309/309 non-install tests passed**, zero failed/skipped, {elapsed} seconds, the same set as the previous job. The two install-consumer tests are excluded as requested.')
s+='''

## Changed files and purpose

Paths are relative to `framework/languages/cpp/`.

| Files | Purpose |
|---|---|
| `framework/src/runtime/dispatch/offload_executor.cpp`, `.hpp` | Centralize extra-wake eligibility in the existing reservation method; compare post-take counts at W5; rotate helpers using the existing queue and active-work registry; preserve capture destruction before quiescence and the original pre-take budget start. |
| `framework/src/runtime/mesh/service_mailbox.cpp`, `.hpp` | Return the exact untaken count from the atomic record take, replacing the boolean `more`. |
| `framework/src/runtime/mesh/mesh_node_host_service.cpp` | Forward the remaining count through the Node drain source; retain the rejection route. |
| `framework/src/runtime/channels/channel_host_service.cpp` | Forward the same count for ChannelName drains. |
| `framework/src/runtime/stateful/public_host_runtime.cpp`, `.hpp` | Carry the count through the existing internal claim-batch bridge. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_gate_less_drain.cpp` | Require one cheap-batch wake; test maintained and growing backlog, helper rotation, and deterministic last-take publication in the unchanged shutdown assertions. |
| `tests/support/manual_internal_scheduler.hpp` | Adapt the existing manual queue to record presence and remaining count. |

리팩토링 점검: 성능 2건, POSDDD 2건, 불필요 코드 1건 발견. 고친 항목: 무조건 추가 wake, 다중 worker의 다른 owner 실행 기회, callback 소멸 전 quiescence 통지, take 전 예산 시작 보존, 기존 take 직후 무조건 wake·capacity 경로 제거. 넘긴 항목: 없음.

## Verification commands and artifacts

Job-relative commands run from this job directory; format and Git commands run from the worktree.

- Configure: `bash configure-tests.sh`, `bash configure.sh`; logs `configure-tests.log`, `configure-production.log`.
- Focused build: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build test-build --parallel 3 --target test_cpp_framework_gate_less_drain test_cpp_framework_offload_executor test_cpp_framework_mailbox_owner_dispatch test_cpp_framework_handler_entry_permit test_cpp_framework_manual_dispatch_owner test_cpp_framework_mailbox_fifo_cost test_cpp_framework_local_enqueue_allocations zlink_cpp_framework_mesh_node_vertical_test`; log `build-focused-fix.log`.
- Focused CTest: the eight targets above via `ctest --test-dir test-build -R '^(test_cpp_framework_(gate_less_drain|offload_executor|mailbox_owner_dispatch|handler_entry_permit|manual_dispatch_owner|mailbox_fifo_cost|local_enqueue_allocations)|zlink_cpp_framework_mesh_node_vertical_test)$' --output-on-failure --stop-on-failure`, under the shared lock; log `test-focused-fix.log`: `100% tests passed, 0 tests failed out of 8`.
- Final build/test/production build: `bash verify-final.sh`. `instrument-test-commands.py` reconciles the final executor object and dependent links before instrumenting compiler-invoking tests. Logs: `build-all.log`, `build-final-source.log`, `instrument-tests.log`, `test-all.log`, `test-all.xml`, `build-production.log`.
- Full test command: `flock -s /root/jobs/fx/cx/measure.lock ctest --test-dir test-build -E '^test_cpp_(framework|stream_connector)_install_consumer$' --output-on-failure --stop-on-failure --output-junit test-all.xml`.
- Measurements: `bash measure.sh > measure.log 2>&1`. Each cell acquires `flock -o /root/jobs/fx/cx/measure.lock`; raw cells are in `measure/`, summaries in `summary.json`, and matching configuration/harness evidence in `measurement-conditions.json`.
- Final format and whitespace checks: `flock -s /root/jobs/fx/cx/measure.lock bash scripts/format/format.sh --check cpp` and `git diff --check`, both exit 0 (`format.log`, `diff-check.log`).

## Remaining problems

'''
if met:s+='None within the requested non-install-test and measurement scope. Supervisor review and commit remain outside this job.\n'
else:
 for name,ok,*rest in checks:
  if not ok:s+=f'- {name} does not meet the supplied acceptance criterion; the numbers and threshold are above.\n'
 s+='Stopped at these measured results for the supplied design. No additional scheduling rule was introduced after measurement.\n'
s+='''
Owner layer: C++ Framework mailbox and application executor.  
Spec authority: S/04 §3 ready-owner conditions; S/02 §9; S/08 §4.3 W5; API §11.  
Cross-language result: read-only .NET/Java/Kotlin/Node.js comparison above; implementation remains within the explicitly approved C++ scope.  
Change classification: A — supervisor-approved contract adaptation for backlog growth and helper rotation.
'''
p.write_text(s)
for x in checks:print(x)
print('ACCEPTANCE', 'MET' if met else 'UNMET')
