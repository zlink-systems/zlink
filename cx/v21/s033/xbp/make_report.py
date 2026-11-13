import pathlib,json,statistics,math,datetime,csv
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp');R=pathlib.Path('/root/worktree/zlink-v21-s033/xbp')
a=json.loads((J/'summary.json').read_text());runs=json.loads((J/'summary-runs.json').read_text());assert len(a)==12
names={'cpp':'C++','dotnet':'.NET','java':'Java'}
def f(x,n=2):return f'{x:,.{n}f}'
def q(v):return '해당 없음' if v is None else f'{v[0]:.3f}–{v[1]:.3f}'
def bytesv(v):
 if v and v%1048576==0:return str(v//1048576)+' MiB'
 if v and v%1024==0:return str(v//1024)+' KiB'
 return f'{v:,} B'
def hwms(ranges):
 vals=[bytesv(lo) if lo==hi else bytesv(lo)+'~'+bytesv(hi) for lo,hi in ranges]
 return '/'.join(vals[:2])+'; '+'/'.join(vals[2:])
lines=[]
def add(s=''):lines.append(s)
add('현재 HEAD의 request-backpressure admission 비교 결과입니다. 소스와 대상의 소켓 HWM은 monitor status의 실제 적용 값을 읽었습니다. 제품 소스는 변경하지 않았습니다.')
add()
add('## 빌드와 처리량')
add()
build=json.loads((J/'raw/build-validation.json').read_text());add(f"HEAD는 `{build['head']}`입니다. Core와 세 언어의 binding·Framework를 지정한 워크트리에서 새로 빌드했습니다. C++와 .NET은 Release이며 Java는 JDK 25로 빌드한 배포본을 기본 JIT 설정으로 실행했습니다. C++ 빌드는 `cpp-build-slot.sh`를 직접 사용하고 `--parallel 3`과 가상 주소 공간 25 GiB 제한을 적용했습니다.")
add()
for path,sha in build['core'].items():add(f'모든 source·target 실행에서 사용한 libzlink SHA-256은 `{sha}`입니다. 각 프로세스의 `/proc/PID/maps`와 실제 파일 hash를 `raw/*/source-native.json`, `target-native.json`에 보존했습니다.')
add()
add('처리량은 10초 warmup과 12초 active를 사용한 3회 실행의 중앙값입니다. 일반 실행과 계수 실행은 별도 프로세스로 실행했습니다. 두 번째 회차는 계수→일반 순서로 바꾸었습니다.')
add()
add('| 크기 | 구현 | 일반 msg/s | 계수 msg/s | 계수/일반 변화 |')
add('|---|---|---:|---:|---:|')
for p in [1024,4096]:
 for l in ['cpp','dotnet','java']:
  for k in ['framework','binding']:
   x=next(x for x in a if (x['language'],x['kind'],x['payload'])==(l,k,p))
   add(f"| {p} B | {names[l]} {k} | {f(x['plainMedian'],0)} | {f(x['countMedian'],0)} | {x['countVsPlainPercent']:+.2f}% |")
add()
add('## Admission 계수')
add()
add('각 계수 실행의 active 시작 후 2~10초, 8초 동안 집계했습니다. 표의 횟수는 세 실행의 계수를 합쳐 같은 창에서 binding에 전달된 정상 REQUEST completion 수로 나눈 요청 1,000건당 값입니다. 제출은 native 공개 `zlink_request` 진입 시각, completion은 공개 `zlink_completion_recv` 반환 시각으로 창에 포함했습니다. Framework API에는 별도 SubmitResult가 없으므로 Framework 경로도 그 아래 binding/Core 공개 제출 결과를 사용했습니다.')
add()
add('WRITABLE은 요청의 context와 completion ID가 일치한 token만 집계했습니다. 진행은 그 token 뒤 재제출이 OK인 경우이며, 무진행은 다시 BACKPRESSURED인 경우입니다. 이 무진행 분류가 곧 잘못된 알림이라는 뜻은 아닙니다. 창 안에서 받은 token의 결과는 창 뒤에도 추적했습니다. 지연은 BACKPRESSURED 반환부터 같은 요청의 다음 native 제출 진입까지이며, 64개 구간으로 나눈 2진 로그 histogram의 분위수 구간을 µs로 표시했습니다. in-flight는 기존 벤치의 공개 통계 값을 약 10 ms마다 읽은 표본의 p50/p99/max입니다.')
add()
for p in [1024,4096]:
 add(f'### {p} B')
 add()
 add('| 구현 | 완료 수 | BACKPRESSURED/1천 | WRITABLE/1천 | 진행 / 무진행/1천 | 재제출/1천 | BP→재제출 p50 / p99 µs | in-flight p50/p99/max | 적용 HWM: 소스 송/수신; 대상 송/수신 |')
 add('|---|---:|---:|---:|---:|---:|---|---|---|')
 for l in ['cpp','dotnet','java']:
  for k in ['framework','binding']:
   x=next(x for x in a if (x['language'],x['kind'],x['payload'])==(l,k,p));c=x['per1000'];flight='/'.join(map(str,x['appFlight']))
   add(f"| {names[l]} {k} | {x['completed']:,} | {f(c['bp'],3)} | {f(c['wakes']-c['wake_unmatched'],3)} | {f(c['wake_progress'],3)} / {f(c['wake_no_progress'],3)} | {f(c['resubmits'],3)} | {q(x['bpDelayP50Us'])} / {q(x['bpDelayP99Us'])} | {flight} | {hwms(x['hwmRanges'])} |")
 add()
add('계수기는 errno와 반환값을 보존하는 작업 전용 native proxy입니다. 동일 native library를 호출하면서 요청별 BACKPRESSURED token만 연결하고, hot path에서 파일 로그나 요청별 동적 할당을 하지 않습니다. 소켓별 계수 잠금과 시각 읽기는 추가됩니다. source 0–7, target 8–15 CPU를 사용하고 실행마다 독점 `flock -o`를 사용했습니다. 메모리 하한과 count·header·drain 검증 결과는 각 `validation.json`에 있습니다.')
add()
add('## 계수 비용과 검증')
add()
add('위 처리량 차이는 계수 잠금·시각 읽기·in-flight HTTP 표본·monitor 조회 비용과 실행 간 변동을 함께 포함합니다. 비용을 보정해 계수를 일반 실행의 수치로 환산하지 않았습니다. 기존 Framework monitor를 재사용하고, raw 소켓에 monitor가 없을 때만 계수 창에서 event mask 0, queue HWM 512 B인 진단 monitor를 열었습니다. 데이터 소켓의 HWM·window·retry 설정은 바꾸지 않았습니다. 옵션 getter의 설정 값도 raw counter에 남겼지만, 표에는 monitor의 applied 값을 사용했습니다.')
add()
fixture=json.loads((J/'raw/counter-validation/validation.json').read_text());add(f"검증 입력은 공개 binding의 기존 HWM contract test를 작업 폴더에 복사해 실행했습니다. 송신·수신 HWM 512 B에서 32개 요청을 완료했고 BACKPRESSURED {fixture['backpressured']:,}회, WRITABLE {fixture['wakes']:,}회, 재제출 {fixture['resubmits']:,}회를 관측했습니다. WRITABLE 합계를 일부러 1 증가시킨 입력은 검증기가 거부했습니다. 원본 binary와 두 검증 결과는 `raw/counter-validation/`에 있습니다.")
add()
add('| 구현·크기 | native 대기 요청 p50/p99/max | native admission 완료 요청 p50/p99/max | WRITABLE→재제출 p50 / p99 µs |')
add('|---|---|---|---|')
for x in a:
 add(f"| {names[x['language']]} {x['kind']} {x['payload']} B | {'/'.join(map(str,x['nativeFlight']))} | {'/'.join(map(str,x['nativeAdmittedFlight']))} | {q(x['wakeDelayP50Us'])} / {q(x['wakeDelayP99Us'])} |")
add()
add('## 원인과 비교 위치')
add()
add('원인 판정은 측정 후 본문에 기록합니다.')
add()
add('## 요청 경로의 반복 검사')
add()
add('아래는 같은 결과나 조건을 다시 검사하는 위치입니다. 초기 제출·재제출의 공통 판정과 상위 계층의 결과 관찰을 구분했습니다. 제품 코드를 수정하지 않았으므로 판정 위치 수는 변경 전후가 같습니다.')
add()
add('| 언어 | 반복 검사 위치 | window·재제출 소유 |')
add('|---|---|---|')
add('| C++ | `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:200`의 거절 검사를 초기·재제출이 `completion_owner.cpp:298`에서 공유합니다. Framework `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:42`와 raw 벤치 `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:690`은 결과를 다시 분류합니다. | Framework 벤치 `bench_cpp_client.cpp:915`, `:995`가 같은 `common/bench_common.hpp:60` 조건을 사용합니다. `bench_cpp_client.cpp:1018`은 window를 0으로 정합니다. 재제출은 binding `completion_owner.cpp:696`에서 NO_DATA 후 수행합니다. |')
add('| .NET | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:152`, `:1196`이 공통 `IsWritableWait`(`:529`)를 초기·재제출에서 호출합니다. Framework `ZLinkOneWaySubmitOutcome.cs:125`와 raw 벤치 `BenchMeasurement.cs:375`가 결과를 관찰합니다. | Framework 벤치 `BenchMeasurement.cs:278`은 admission만 기다리며 reply 수를 제한하지 않습니다. raw 벤치 `:347`도 pending admission만 검사합니다. 재제출은 binding `CompletionOwner.cs:308`에서 NO_DATA 후 수행합니다. |')
add('| Java | `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:194`, `:812`가 공통 `isWritableWait`(`:842`)를 초기·재제출에서 호출합니다. Framework `ZLinkJavaSocketSupport.java:132`와 raw 벤치 `BenchDrivers.java:272`가 결과를 관찰합니다. | Framework 벤치 `BenchDrivers.java:585`는 admission만 기다리며 reply 수를 제한하지 않습니다. raw 벤치 `:239`도 pending admission만 검사합니다. 재제출은 binding `CompletionOwner.java:560`에서 NO_DATA 후 수행합니다. |')
add()
add('소켓 byte admission과 실제 HWM은 공통 Core가 정합니다. `core/src/runtime/core/auto_hwm_policy.cpp:42`가 profile 예산을, `core/src/runtime/sockets/common/socket_base.cpp:272`가 연결에 적용하는 계획을 담당합니다. monitor applied 조회는 `core/src/runtime/sockets/common/socket_base_monitor.cpp:94`입니다. 위 Framework 결과 분류에서 Core HWM이나 WRITABLE retry를 별도로 구현한 것은 이 요청 경로에서 확인하지 못했습니다.')
add()
add('모든 원시 출력은 `raw/`에 보존했습니다. 셀별 세 실행 수치는 `summary-runs.json`, 합산 수치는 `summary.json`, 실행 명령은 각 셀의 `source-command.json`·`target-command.json`과 driver log에 있습니다. 일반 실행 36회와 계수 실행 36회의 결과 및 남은 실패는 검증 후 확정합니다.')
(J/'report.md').write_text('\n'.join(lines)+'\n')
print('report draft written; replace the cause paragraph and final verification sentence before delivery')
