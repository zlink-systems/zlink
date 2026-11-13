import json, pathlib, statistics
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/r4190m')
agg=job/'raw/aggregates'
langs=['cpp','dotnet','java','node']
patterns=['request-serial','request-backpressure','send-saturation']
sizes=[1024,4096]

def fmt(v,d=2): return '—' if v is None else f'{v:,.{d}f}'
def status(s): return {'published':'공개가능','pass':'통과','fail':'미달','unsupported':'미판정','incomplete':'미완료'}.get(s,s)
def loadcpu(path):
    return json.loads(path.read_text())

def cpu_stat(records, cell, role):
    xs=[]
    for r in records:
        if r['cell'] != cell: continue
        v=r.get(role+'_us_per_msg')
        if v is None: continue
        if 'snapshot_complete' in r and not r['snapshot_complete']: continue
        xs.append(float(v))
    return statistics.median(xs) if xs else None, len(xs)

md=[]
md += ['# #4190 재측정 및 #4162 A/A', '',
       '측정 commit: `d3fd955131c1942dc46203fde365f1c690b3d0f6` (#4245 merge).',
       '기본 runner 값은 warmup 2초, active 5초, judged run 3회이며 요청 패턴은 직렬 요청·역압 요청·send 포화, payload는 1 KiB·4 KiB다. Framework 언어 드라이버는 8 submit stream 설정을 사용했다. C raw 기준은 C runner를 그대로 사용했다. C raw는 18셀, 언어별 측정은 gRPC 역압 요청 n/a를 빼고 48셀, Kotlin 보충은 serial 1 KiB 6셀이다.', '']
md += ['## 처리량 및 기준 판정', '', '처리량은 run 중앙값(msg/s)이다. 비율 열은 `framework/binding`(기준 0.60)과 역압의 `binding/C`(기준 0.80)이다. 숫자는 기준과 직접 비교한 값이고 괄호는 #4230 aggregator 판정이다. aggregator `미판정`은 G5/G6 또는 분모 셀의 기준 실패로 7.2 judgement가 지원되지 않는 경우다.', '']
for lang in langs:
    data=json.loads((agg/f'{lang}.json').read_text())
    rows={(r['implementation'],r['pattern'],r['payload_size']):r for r in data['rows']}
    judgments={(j['formula'],j['pattern'],j['payload_size']):j for j in data['judgements']}
    cpu_paths={'cpp':'cpp-rerun1','dotnet':'dotnet-cpu-rerun','java':'java','node':'node'}
    cr=loadcpu(job/f"raw/bench/{cpu_paths[lang]}/cpu-per-cell.json")
    cc=loadcpu(job/'raw/bench/c-cpu-supplement3/cpu-per-cell.json')
    name={'cpp':'C++','dotnet':'.NET','java':'Java','node':'Node'}[lang]
    md += [f'### {name}', '', f"언어 종합: **{status(data['language_verdict']['status'])}** — {data['language_verdict']['reason']}.", '', '| 패턴 | 크기 | gRPC | binding | Framework | Framework/binding (0.60) | binding/C (역압 0.80) |', '|---|---:|---:|---:|---:|---:|---:|']
    for pattern in patterns:
      for size in sizes:
        def row(impl): return rows.get((impl,pattern,size))
        a=row('grpc-'+lang); b=row('zlink-'+lang); f=row('zlink-framework-'+lang); c=row('zlink-c')
        if not a or not b or not f or not c: continue
        fw=judgments.get((f'zlink-framework-{lang} / zlink-{lang}',pattern,size))
        bc=judgments.get((f'zlink-{lang} / zlink-c',pattern,size)) if pattern=='request-backpressure' else None
        def jfmt(j,threshold):
          if not j:return '—'
          raw=fmt(j['value'],3)
          aggstatus = status(j['verdict']) if j['status']=='published' else status(j['status'])
          return f"{raw} (수치{'통과' if j['value'] >= threshold else '미달'}; agg {aggstatus})"
        md.append(f"| {pattern} | {size} | {fmt(a['median'].get('throughput_per_second'),0)} | {fmt(b['median'].get('throughput_per_second'),0)} | {fmt(f['median'].get('throughput_per_second'),0)} | {jfmt(fw,.6)} | {jfmt(bc,.8)} |")
    md += ['', f'#### {name} CPU', '', 'source/target CPU µs/msg՝ `/proc/<pid>/task` TID union의 before/after tick 차이 ÷ 완료 메시지. 표의 값은 반복 중앙값. C CPU는 처리량 판정에 사용하지 않은 별도 1회 보충 실행에서 TID union으로 수집했다.', '', '| 패턴 | 크기 | gRPC source/target | binding source/target | Framework source/target | C source/target |', '|---|---:|---:|---:|---:|---:|']
    for pattern in patterns:
      for size in sizes:
        cells=[('grpc-'+lang,cr),('zlink-'+lang,cr),('zlink-framework-'+lang,cr),('zlink-c',cc)]
        values=[]
        for impl,records in cells:
          cell=f'{impl}-{pattern}-{size}'
          s,sn=cpu_stat(records,cell,'source'); t,tn=cpu_stat(records,cell,'target')
          val=f'{fmt(s,1)}/{fmt(t,1)}'
          if impl=='zlink-c': val += f' (n={min(sn,tn)}/1, CPU 보충)'
          values.append(val)
        # gRPC BP is skipped by runner; omit rather than infer.
        if pattern=='request-backpressure': values[0]='n/a'
        md.append(f'| {pattern} | {size} | {values[0]} | {values[1]} | {values[2]} | {values[3]} |')
    md.append('')
# Kotlin supplementary
kdata=loadcpu(job/'raw/bench/kotlin/cpu-per-cell.json')
md += ['## Kotlin 보충 셀', '', '요청 직렬 @1 KiB, 3회; gRPC Kotlin 및 Framework Kotlin. 처리량은 중앙값이다.', '', '| 구현 | msg/s | source/target CPU µs/msg |', '|---|---:|---:|']
for impl,label in [('grpc-kotlin','gRPC Kotlin'),('zlink-framework-kotlin','Framework Kotlin')]:
    res=[]
    for p in sorted((job/'raw/bench/kotlin').glob('run*/*/results.json')):
        x=json.loads(p.read_text())['cells'][0]
        if x.get('implementation')==impl: res.append(x.get('throughput_per_second'))
    rec=[r for r in kdata if r['cell']==f'{impl}-request-serial-1024']
    s,sn=cpu_stat(rec,f'{impl}-request-serial-1024','source'); t,tn=cpu_stat(rec,f'{impl}-request-serial-1024','target')
    md.append(f'| {label} | {fmt(statistics.median(res) if res else None,0)} | {fmt(s,1)}/{fmt(t,1)} |')
md += ['', '## A/A (#4162)', '']
aa=json.loads((job/'tools/raw/aa-analysis.json').read_text())
md += [f"`aa-clean`, separate, 20회 실행. analyzer가 p50을 제공하지 않아 요청한 p50 ±5% 기준은 **실패(판정 자료 없음)**이며, p50 범위는 N/A다. 참고로 제공된 평균 지연 중앙값은 {fmt(aa['median_mean_ms'],6)} ms, 범위는 {fmt(aa['mean_min_ms'],6)}–{fmt(aa['mean_max_ms'],6)} ms이고 평균 기준 ±5%도 {'통과' if aa['within_5_percent_of_median'] else '실패'}다.", '']
md += ['## 측정 조건 및 기록', '', '- governor: 각 측정 lock window에서 cpu0–15 `performance`; lock 해제 시 trap으로 `powersave` 복원.', '- placement B: source 0–7, target 8–15. Kotlin runner의 별도 process도 source 0–7, target 8–15에 배치.', '- 잠금 창(UTC): cpp 08:37:59–08:50:23; .NET 09:12:07–09:32:51; Java+Kotlin 09:34:02–09:55:25; Node 09:56:28–10:05:09; C+A/A 10:25:35–10:32:26. C와 A/A는 같은 exclusive lock window였다. C CPU 보충 10:57:13–10:58:54.', '- uptime/load, free memory, cpu0–15 `scaling_cur_freq`는 각 셀의 `conditions-before.txt`/`conditions-after.txt`에 저장했다. 전체 raw 로그는 `raw/`에 보존했다.', '- A/A 조건 전/후는 `raw/aa-clean-conditions-before.txt`와 `raw/aa-clean-conditions-after.txt`; run별 raw는 f4162 경로 아래 `aa-clean` label로 보존되어 있다.', '- CPU 스냅샷 완전성: 기본 C 18셀의 첫 `/proc` watcher는 7셀에서만 양쪽 양수 TID delta를 얻었고 conditions-before 16/18을 작성했다. 별도 C CPU 보충 실행은 6개 pattern/size 행의 CPU와 conditions-before/after를 모두 수집했고, CPU 표는 그 보충 실행을 사용한다. 다른 유효한 주요 데이터셋의 CPU rows는 C++ 48, .NET 48, Java 48, Kotlin 6, Node 48이며 null CPU 값은 없다.', '- 첫 C 실행은 gRPC 보조 실행 파일이 없어 runner preflight에서 종료됐다. 그 보조 타깃은 C++ build slot에서 빌드한 후 같은 코드로 C raw를 실행했다. 첫 .NET raw는 launcher CPU를 잡아 CPU 집계에서 제외하고 유효 재측정 48셀을 사용했다. 첫 C++ 시도도 불완전 raw로 보존했으며 judgement에는 재측정 48셀만 넣었다.', '']
md += ['## 검증 및 결과 위치', '', '- 실행: C/C++ `cpp-build-slot.sh`로 빌드; Java/Node/.NET/C/Kotlin의 각 `run_local*.sh`; `bench_aggregate.py --lang {cpp,dotnet,java,node} --min-runs 3`; `aggregate_cpu.py`와 C 전용 TID-union 집계 및 `proc_capture_c.py` 보충 watcher; `observe.py --runs 20 --label aa-clean --placement separate`; `analyze-aa.py`.', '- aggregator markdown/JSON: `raw/aggregates/{cpp,dotnet,java,node}.{md,json}`. Runner raw와 조건·CPU 스냅샷: `raw/bench/`. Driver/build logs: `raw/*-driver.log`, `build-logs/`.', '- language verdict: ' + '; '.join(f"{ {'cpp':'C++','dotnet':'.NET','java':'Java','node':'Node'}[l]} {status(json.loads((agg/f'{l}.json').read_text())['language_verdict']['status'])}" for l in langs)+'.', '- CPU 측정은 C raw에서 불완전하며, A/A p50은 analyzer 출력에 없다. 이 외 source 코드·bench 코드 변경은 없다.', '']
text='\n'.join(md)
(job/'docs-page.md').write_text(text+'\n')
(job/'report.md').write_text(text+'\n')
print(f'wrote docs-page.md and report.md ({len(text)} chars)')
