"""Read-only document/source-reference and arithmetic review, not product tests."""
import json
import re
import statistics as st
from pathlib import Path

OUT=Path(__file__).resolve().parents[1]
TREE=Path('/root/worktree/zlink-v21-s033/cpath')
prefixes={
 'C':TREE/'framework/languages/cpp/framework/src/runtime',
 'H':TREE/'framework/languages/cpp/framework/include/zlink/framework',
 'B':TREE/'bindings/cpp',
 'P':TREE/'framework/bench/grpc/cpp',
 'N':TREE/'framework/languages/dotnet/src/Zlink.Framework/Runtime',
 'J':TREE/'framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime'}
body=(OUT/'report.md').read_text()
refs=[]
for m in re.finditer(r'\b([CHBPNJ])/([A-Za-z0-9_./-]+):(\d+)',body):
    p=prefixes[m[1]]/m[2]
    line=int(m[3])
    ok=p.is_file() and line<=len(p.read_text().splitlines())
    refs.append(dict(reference=m[0],exists_and_in_range=ok))
data=json.loads((OUT/'analysis/recomputed.json').read_text())
captures=[c for c in data['captures'] if c['input'].startswith('ctrecv__raw/raw/final/unpaced/binding-')]
io=st.median(c['roles']['source']['top_threads'][0]['cpu_us'] for c in captures)
recv=st.median(next(t['cpu_us'] for t in c['roles']['target']['top_threads']
                    if t['comm']=='bench_cpp_zlink' and t['cpu_us']>.5) for c in captures)
audit=body.split('## 8. 리팩토링 점검\n',1)[1].split('점검은',1)[0]
audit_rows=[x for x in audit.splitlines() if x.startswith('|')][2:]
ccap=OUT/'input/ccall2/ccall2/out-profile/run1/zlink-framework-cpp-request-backpressure-1024/capture'
before=json.loads((ccap/'before.json').read_text())
after=json.loads((ccap/'after.json').read_text())
seconds=after['monotonic']-before['monotonic']
threads={}
for role in ['source','target']:
    threads[role]=[]
    for tid,t in after['processes'][role]['threads'].items():
        old=before['processes'][role]['threads'].get(tid,{'runtime_ns':0})
        cores=(t['runtime_ns']-old['runtime_ns'])/1e9/seconds
        threads[role].append(dict(tid=tid,comm=t['comm'],cores=cores))
    threads[role].sort(key=lambda t:t['cores'],reverse=True)
    threads[role]=threads[role][:4]
results=dict(reference_count=len(refs),reference_failures=[r for r in refs if not r['exists_and_in_range']],
             audit_rows=len(audit_rows),unpaced_binding_five_run_io_cpu_us=io,
             unpaced_binding_five_run_receive_cpu_us=recv,
             ccall_profile_thread_cores=threads,
             raw_ratio_quality={k:sum(bool(r[k]) for r in data['runs']) for k in
                ['errors','abandoned','contaminated','drain_bound_hit']},
             report_lines=len(body.splitlines()),report_bytes=len(body.encode()))
(OUT/'analysis/review.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(results,ensure_ascii=False,indent=2))
