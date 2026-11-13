from pathlib import Path
import subprocess,xml.etree.ElementTree as E,re,json,math,statistics,hashlib
out=Path(__file__).parent
subprocess.run(['python3',str(out/'make_report.py')],check=True)
ns={'t':'http://microsoft.com/schemas/VisualStudio/TeamTest/2010'}
def races(path):
    data={}
    for r in E.parse(out/path).findall('.//t:UnitTestResult',ns):
        if 'same_handle_submit_races_close' not in r.get('testName',''): continue
        kind=re.search(r'kind: "(.*?)"',r.get('testName')).group(1)
        stdout=r.find('t:Output/t:StdOut',ns).text
        record={'race':{},'closed':{},'close_busy':int(re.search(r'close_busy=(\d+)',stdout).group(1))}
        for phase,kind_outcome,count in re.findall(r'phase=(\w+) outcome=(.*?) count=(\d+)',stdout,re.S):
            if kind_outcome.startswith('other:ObjectDisposedException'): kind_outcome='other:ObjectDisposedException'
            record[phase][kind_outcome]=int(count)
        data[kind]=record
    return data
before=races('raw/main-test-results/main-races.trx')
after=races('raw/final-load-results/final-load.trx')
(out/'raw'/'race-summary-final.json').write_text(json.dumps({'main':before,'after':after},indent=2))
section='''최초 main 재현은 production source를 수정하기 전에 수행했다. Main과 변경 후 모두 CPU stress-ng 4개 worker, cpu-load 70, matrixprod를 `/root/jobs/fx/cx/measure-shared.sh` 안에서 실행했다. 각 실행 시작에 MemAvailable 12 GiB 이상을 확인했고 stress-ng는 SIGTERM으로 종료했다. helper는 `run_loaded.py`다. Core는 `bindings/tools/local_core_runtime.sh`가 선택한 기존 `/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.1.23.0`를 사용했다. 실행 전·후 SHA-256은 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`로 같다. `raw/environment.json`에 보존했다.

각 행은 같은 socket의 submit 한 번과 close 한 번을 동시에 시작한 1,000회다. close의 BUSY는 submit 실패 수에 포함하지 않으며, 반환 후 제출 thread를 join한 다음 close를 완료했다. 이어 같은 socket에서 close 후 submit도 1,000회 검사했다. request는 async 첫 native admission을 경합시키고, close 후 pending Reply의 RequestError(TERMINATED)를 확인한다. 동기 request와 multipart 거부는 별도 ownership 검사로 확인한다.

| 구현 | 경로 | 성공 | TERMINATED | EFAULT | ENOENT | INVALID_HANDLE | NOT_FOUND | 기타 | close BUSY |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
'''
for label,data in [('main',before),('변경 후',after)]:
    for kind in ['send','request','reply','publish']:
        d=data[kind]['race']; other=sum(n for k,n in d.items() if k.startswith('other:'))
        section+=f"| {label} | {kind} | {d.get('success',0)} | {d.get('TERMINATED',0)} | {d.get('EFAULT',0)} | {d.get('ENOENT',0)} | {d.get('INVALID_HANDLE',0)} | {d.get('NOT_FOUND',0)} | {other} | {data[kind]['close_busy']} |\n"
section+='''
main의 기타 658회는 publish의 ObjectDisposedException이다. close 후 검사에서 main send/request/reply는 각각 TERMINATED 1,000회였지만 publish는 ObjectDisposedException 1,000회였다. 변경 후 close 후 검사는 네 경로 모두 TERMINATED 1,000회였다. 분류는 native errno EFAULT/ENOENT를 먼저 적용하고, 이후 typed InvalidHandle/NotFound와 기타 exception을 센다. 존재하지 않은 EFAULT/ENOENT를 관측했다고 적지 않았다. Native Core가 stale 입력에도 TERMINATED를 반환할 수 있으므로 결과 분류만으로 pointer 안전성을 증명하지 않으며 2절의 예약 순서 증명을 함께 적용한다.

main raw는 `raw/main-races.log`, `raw/main-races-stress.log`, `raw/main-test-results/main-races.trx`다. 최종 변경 후 raw는 `raw/final-load.log`, `raw/final-load-stress.log`, `raw/final-load-results/final-load.trx`다. 집계는 `raw/race-summary-final.json`이다. 최초 검사는 main 4개 중 publish 실패, 최종 load 검사는 38/38 통과다.

추가 검사는 validation과 native 입장 사이 close, 중첩 예약 두 개와 마지막 해제 전 BUSY, 별도 thread의 동시 예약, 예약 중 강제 GC, SafeHandle 지연 해제의 true/false 결과, close 후 1/2/9 part 원본 보존이다. 9 part는 stack 한도 8을 넘어 ArrayPool 복원 경로도 검사한다. 기존 mixed 단일·multipart 50,000회 경합은 INVALID_HANDLE/NOT_CONNECTED/ObjectDisposedException/INVALID_ARGUMENT를 허용하던 조건을 제거해 성공과 TERMINATED만 허용하도록 강화했다. 이를 포함한 관련 47개 load 검사는 `raw/after-root-focused.log`에, 마지막 38개는 위 final-load에 있다.

전체 script `bindings/dotnet/tests/run_tests.sh`의 무필터 실행은 두 번 시도했고 완료되지 못했다. `raw/full-suite.log`는 294 통과와 source 검사·task 예외 2개 실패 뒤 진행 정지, `raw/full-suite-02.log`는 229 통과 뒤 진행 정지다. 중지는 SIGTERM/SIGINT로 수행했다. 두 번째 실행의 diag는 정지한 case를 `test_message_gc_lifetime.payload_round_trips_during_concurrent_compacting_gc(size: 30)`으로 특정한다. 해당 검사는 socket submit을 사용하지 않는다. 동일 main DLL을 새 프로세스에 적재한 분리 실행에서도 같은 case가 진행을 멈췄다. `raw/main-message-gc.log`, `raw/main-message-gc-diag.host.*.log`, `raw/main-message-gc-native-stacks.log`와 `raw/full-suite-02-native-stacks.log`에 근거를 보존했다. Core나 message 구현은 수정하지 않았다. 이 기존 검사 3개 data case는 미해결이며 무필터 전체 suite 통과라고 판정하지 않는다.

진행 정지와 별개로 드러난 두 검사는 다음처럼 처리했다. source guard는 native 인자명 `_handle` 자체를 요구하던 항목을 native completion pull 사용과 지속 raw IntPtr 복사 금지로 바꿨다. 기존 background thread/timer/Task.Run 금지 조건은 전부 유지했다. task 예외는 기존 tokenless fixture가 Admitted를 관측하지 않고 Reply만 관측한 원인이다. 기준 main에서 `tokenless → unreturned`를 강제로 순서 실행한 `raw/main-unobserved-ordered.log`가 실패하는 음성 증거다. 두 public task를 모두 검사하도록 tokenless와 synthetic writable fixture를 강화했다. 같은 main DLL에 강화한 fixture를 적용한 `raw/main-fixed-fixture-ordered.log`와 변경 후 `raw/after-fixed-fixture-ordered.log`는 모두 통과했다. 전역 unobserved assertion이나 fixture timeout을 완화하지 않았다.

알려진 concurrent message GC 검사 3건만 명령 필터로 제외한 추가 실행은 361/361 통과와 sample 7/7 통과다. `raw/suite-except-baseline-gc.log`, `raw/suite-except-baseline-gc-results/suite-except-baseline-gc.trx`에 있다. 명령은 `bindings/dotnet/tests/run_tests.sh --filter FullyQualifiedName!~payload_round_trips_during_concurrent_compacting_gc`다. source에 Skip이나 완화 조건을 추가하지 않았다. 별도 sample 실행도 `raw/samples-separate-02.log`에서 7/7 통과했다.

`scripts/format/format.sh`는 Framework source만 선택해 binding에는 적용되지 않는다. 따라서 touched 파일만 대상으로 `dotnet format whitespace`를 적용하고 `--verify-no-changes`를 통과했다. `raw/format-apply-final.log`, `raw/format-verify-final.log`, `raw/format-verify-final.status.json`에 보존했다. `git diff --check`도 통과했다. 전체 변경은 new test 3개를 포함한 `change.diff`다.
'''
records=json.loads((out/'raw'/'pairs-manifest.json').read_text())
assert len(records)==24
perf='''A는 main, B는 변경 후다. 같은 SDK 10.0.112, target net8.0, Release, Optimize 기본값, ServerGC 설정, SourceLink 조회 비활성화로 빌드했다. 빌드 로그는 `raw/build-main-perf.log`와 `raw/build-perf-round2.log`다. 실행 DLL은 `perf-main/`과 `perf-after-final-02/`에 있다. Native Core는 앞 절의 동일 SHA-256 runtime이다.

기존 `bindings/dotnet/perf/single/Zlink.BindingBench` runner를 직접 실행했다. 두 패턴 모두 inproc, 1024 B, application part 1개, active duration 10초, latency sample cap 1,000,000이다. 각 실행을 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 수행했다. `run_pairs.py`, `pin_benchmark.py`가 명령과 순서를 보존한다. 홀수 pair는 AB, 짝수 pair는 BA다.

single runner는 한 프로세스의 sender thread와 receiver main thread를 사용한다. 외부 helper가 sender를 CPU 0–7, receiver를 CPU 8–15에 배정했다. sender 생성 직후 1 ms 간격 관측 세 번 뒤 pin을 적용하므로 시작 직후 짧은 구간은 초기 CPU 집합 0–7이다. 나머지 runtime thread도 초기 집합을 상속한다. 24회 모두 실제 source/target affinity와 종료 코드 0을 각 `.json`에 확인했다. 최초 두 probe는 최종 pair 통계에서 제외했다. 첫 probe의 pin 실패도 `raw/perf-probe-main-pair.*`에 그대로 보존했다.

| 패턴 | pair | 순서 | main msg/s | 변경 후 msg/s | 변화 | raw stdout/stderr |
|---|---:|---|---:|---:|---:|---|
'''
summary={}
for pattern in ['PAIR','DEALER_DEALER']:
    ratios=[]; values=[]
    for pair in range(1,7):
        row=[x for x in records if x['pattern']==pattern and x['pair']==pair]
        assert len(row)==2
        data={}
        for item in row:
            p=out/'raw'/(item['label']+'.log')
            value=float(next(s.rsplit(',',1)[1] for s in p.read_text().splitlines() if ',throughput,' in s))
            meta=json.loads(p.with_suffix('.json').read_text())
            assert meta['pinned'] and meta['source_affinity']==list(range(8)) and meta['target_affinity']==list(range(8,16)) and meta['exit_code']==0
            data[item['variant']]=value
        ratio=data['after']/data['main']; ratios.append(ratio)
        raw=' / '.join('`raw/'+item['label']+'.log`' for item in row)
        perf+=f"| {pattern} | {pair} | {'AB' if pair%2 else 'BA'} | {data['main']:,.1f} | {data['after']:,.1f} | {(ratio-1)*100:+.2f}% | {raw} |\n"
        values.append({'pair':pair,**data,'ratio':ratio})
    gm=math.exp(statistics.mean(map(math.log,ratios)))
    summary[pattern]={'pairs':values,'geometric_mean_ratio':gm,'median_ratio':statistics.median(ratios),'minimum_ratio':min(ratios),'maximum_ratio':max(ratios)}
perf+='\n'
for pattern,d in summary.items():
    perf+=f"{pattern}의 paired ratio 기하평균은 {(d['geometric_mean_ratio']-1)*100:+.2f}%, 중앙값은 {(d['median_ratio']-1)*100:+.2f}%다. pair 범위는 {(d['minimum_ratio']-1)*100:+.2f}%에서 {(d['maximum_ratio']-1)*100:+.2f}%다.\n\n"
perf+='''DEALER_DEALER의 편차가 커서 평균 증가만으로 성능 개선을 확정하지 않는다. 측정은 송신과 수신의 lifecycle 보호 비용을 함께 포함한다. Interlocked +1 목표가 충족됐다는 의미도 아니다. manifest와 runner 출력은 `raw/pairs-manifest.json`, `raw/pairs-runner.log`, 집계는 `raw/performance-summary.json`이다. 각 run의 `.json`에는 명령, runtime 경로, thread ID와 affinity를 남겼고 `.log`에는 throughput·bandwidth·latency 원문을 전부 남겼다.
'''
(out/'raw'/'performance-summary.json').write_text(json.dumps(summary,indent=2))
p=out/'report.md'; s=p.read_text().replace('[TEST_RESULTS]',section).replace('[PERF_RESULTS]',perf).replace('새 지속 상태는 `_state` 하나','새 지속 제어 상태는 `_state` 하나')
s+='''
남은 항목은 동기 send·publish·reply의 Interlocked +1 목표 미충족과 기준 main에서도 재현되는 concurrent message GC 검사다. 이 두 항목을 해결했다고 보고하지 않는다. branch 변경, commit, push는 수행하지 않았다.
'''
p.write_text(s)
print('report.md complete')
