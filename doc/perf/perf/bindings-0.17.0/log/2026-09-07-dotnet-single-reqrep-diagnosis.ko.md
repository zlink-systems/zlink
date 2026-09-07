# .NET single REQREP completion 진행 진단

## 결과

분리된 requester/progress thread 구조에서 `completionPoller.Wait(50ms)`가 반복해서
50ms 가까이 대기하며 처리량을 제한했다. inFlight는 64까지 올라갔고, 제출 함수는
평균 3.83µs에 반환했다. thread pool continuation을 제거해도 thread를 분리하면
3,218 ops/s에 머물렀다. 따라서 thread pool 사용은 §1.1.5 위반이지만, 낮은 처리량의
주원인으로 단정할 수 없다.

전용 requester thread가 제출·poll·완료 집계를 교대로 수행하는 구조는 임시 계측을
제거한 측정에서 **281,288 ops/s**였다. 같은 세션에서 복원한 이전 구조의
**2,930 ops/s 대비 96.0배**다. 감독자 제공 3,542 ops/s 대비 79.4배이며, 제공된
C 774,901 ops/s의 36.3%다. C 수치는 이번에 재측정하지 않았고, single의 언어 간
비율을 성능 gate 통과 판정으로 사용하지 않는다.

## 작업 범위와 기준 사본

작업 진입 시 `PerfReqRep.cs`에는 이미 전용 thread가 `Task.IsCompleted`를 확인하는
수정안이 존재했다. 해당 파일을 `.artifacts/dotnet-reqrep-diagnosis/PerfReqRep.entry.cs`에
보존하고 그 위에서 작업했다. 이후 다른 작업이 기존 수정안을 `be4608bd79`에 포함해
커밋했다. 이 작업에서는 commit/push를 실행하지 않았다.

이전 구조는 당시 Git blob `df27dadb57`의 `RunRequestLoop`를 가져와, 요청에 명시된
`maxOutstanding=64` gate를 적용해 격리 사본에 복원했다. 나머지 코드는 진입 시점
사본을 유지했다. 따라서 감독자의 이전 실행 바이너리와 동일하다고 주장하지 않는다.
복원한 소스는 `.artifacts/dotnet-reqrep-diagnosis/PerfReqRep.baseline.cs`에 있다.

최종 추가 변경:

- `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs`: 기존 전용 thread
  구조를 보존하고, pending 목록을 소유 thread 내부에서 기존 상한 크기로 생성한다.
  thread pool이 3.5k의 실측 원인이라는 잘못된 주석과, 상한이 정상 깊이보다 훨씬 크다는
  근거 없는 주석을 정정한다.
- `bindings/dotnet/perf/single/run_emit.py`: §1.1.3에 따라 REQREP의 실제 미완료 상한을
  Effective Options의 `reqrep_max_outstanding`으로 표시한다. 기본값은 그대로 64다.
- 이 진행 기록.

binding 본체, Core, Framework, 다른 언어 러너, 정책·스펙 문서는 수정하지 않았다.
공유 작업 트리의 기존 변경과 untracked 파일은 보존했다.

## 측정 조건과 실행 경로

모든 perf 실행은 아래 한 셀을 직렬로 수행했다. 각 실행 직전 `uptime`과 실행 중인
perf process를 확인했다. 다른 러너·셀은 실행하지 않았다.

```bash
PERF_FAIL_FAST=1 ZLINK_CORE_SOURCE=local bindings/dotnet/perf/run_benchmarks.sh \
  --pattern DEALER_ROUTER_REQREP --transports tcp --msg-sizes 64 --duration 1 --runs 1
```

- Core: `core/build/lib/libzlink.so.0.17.1`
- Build ID: `f7e2a5397eb55df4001f5a447f4ca0522be09e7c` — 시작·중간·검증 종료 시 일치
- Release, TCP, 64B, duration 1초, runs 1, multipart 2, IO thread 1
- requester socket 1, 미완료 상한 64, context auto-HWM
- request timeout 200ms, receive timeout 200ms, completion poll 대기 상한 50ms,
  종료 후 drain 한도 10,000ms — 모두 유지
- handshake, header stamp, active deadline, reply 유효성 검사, 완료 count와 latency
  sample, RESULT 확정 위치의 의미 유지

주 작업 트리를 보존하기 위해 `.artifacts/dotnet-reqrep-diagnosis/baseline/`에 .NET
러너 사본을 만들었다. 진단 도중 `core/src/runtime/core/msg.cpp`의 mtime이 artifact보다
새로워져 스크립트의 stale 검사에서 한 번 실행이 거부됐다. Core를 재빌드하거나 mtime을
변경하지 않았다. 격리 사본의 `core/build`를 지정 artifact가 있는 기존 build directory에
연결했다. 스크립트에 이미 있는 외부 build symlink 처리 경로를 사용했으며, native
runtime은 위 Build ID로 고정했다. 최종 before/after는 모두 이 격리 사본에서 실행했다.

최종 before 실행 직전 load average는 `0.20, 0.37, 0.35`, after 직전은
`0.16, 0.34, 0.35`였다. 테스트와 perf를 동시에 실행하지 않았다.

## 계측과 인과관계

### inFlight와 제출 시간

첫 새 계측의 원자료는 `.artifacts/dotnet-reqrep-diagnosis/baseline-diag.txt` 첫 줄이다.

| 항목 | 실측 |
|---|---:|
| 제출 | 3,337 |
| active 내 완료 | 3,273 |
| 최대 inFlight | 64 |
| 상한에 도달해 Yield한 횟수 | 5,412,583 |
| submit 누적 시간 | 12.769ms |
| submit 평균 시간 | 3.83µs |
| pool에서 재개된 continuation | 3,337 |
| 다른 thread에서 재개된 continuation | 0 |
| request timeout | 0 |

계측 위치는 `PerfReqRep.baseline-instrumented.cs:513`의 inFlight 증가,
`:518` 부근의 submit 전후 timestamp, `:552`의 await 이후 thread 확인이다.
상한 대기 횟수는 요청 수나 시간 표본이 아니라 loop 반복 횟수다.

### 50ms poll 대기

같은 계측에서 poll 90회의 누적 소요는 1,036.609ms였고, 그중 20회가 45ms 이상이었다.
이는 active 이후 drain까지 포함한다. 완료 latency는 평균 19.077ms,
p95 50.575ms, p99 50.800ms였다. 감독자 제공 3,542 ops/s의 역수인 282µs를 개별 request의
왕복 latency로 읽으면 안 된다. 여러 request가 약 50ms의 대기를 공유하는 구조였다.

이전 코드의 대기 위치는 `PerfReqRep.baseline.cs:440`이다. binding 공개 poller는
`bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs:268`에서 native wait에 들어가고,
`:298`에서 completion을 drain한다. `CompletionOwner.cs:305`의 drain은 제출과 같은
lock으로 보호되며 `:333` 이후 completion을 NO_DATA까지 읽는다.

50ms 외 상수도 대조했다. `PerfReqRep.cs:632` 부근의 request timeout은 200ms,
`PerfCommon.cs:447`의 receive timeout은 200ms이고 request timeout 실측은 0건이다.
`Thread.Yield()`에는 지정 sleep 간격이 없다. binding의 runtime pump에 있는 25ms
대기는 `CompletionOwner.cs:630`의 public-owner 검사 때문에 이 requester에서 실행되지
않는다. poller 등록이 먼저 sole completion owner를 획득하기 때문이다.
`PerfCommon.cs`는 `sndTimeo` 값을 읽지만 이 함수에서 send timeout을 소켓에 대입하지는
않는다. 이번 request admission은 공개 Async terminal의 DONTWAIT 경로다.

추가 trace에서는 50.262ms 걸린 poll 안에서 새 제출 64건이 poll 시작 후
0.407~0.727ms에 끝났다. 이전 batch의 continuation 63건도 0.253~0.667ms에 실행됐다.
즉 thread pool이 50ms 동안 실행되지 않아 멈춘 현상은 아니다. 시간순 원자료는
`.artifacts/dotnet-reqrep-diagnosis/baseline-trace.csv`에 보존했다.

### thread pool 제거 대조군

격리 사본에서 제출/progress thread와 50ms poll 대기는 유지하고, 완료된 Task를
progress thread가 직접 확인해 집계하도록 바꿨다. timeout·상한·payload는 바꾸지 않았다.

| 구조 | throughput | 최대 미완료 | 45ms 이상 poll |
|---|---:|---:|---:|
| 분리 thread + pool continuation, 계측 | 3,273 | 64 | 20 |
| 분리 thread + 직접 완료 집계, 계측 | 3,218 | 64 | 20 |
| 같은 thread에서 제출·poll·집계, 조건부 Wait(0)/Wait(50), 계측 | 302,341 | 64 | 0 |
| 같은 thread에서 제출·poll·집계, 항상 Wait(50), 계측 | 298,715 | 64 | 0 |

대조군 소스는 `PerfReqRep.split-sync.cs`, 결과는 `split-sync-diag.txt`와
`split-sync-run.log`다. 직접 집계 대조군의 poll 누적 소요도 1,029.060ms였다.
공유 pool을 없애는 것만으로는 병목이 해소되지 않았다.

최종 구조는 제출 직후 Wait(0), 새 제출이 없을 때 Wait(50)을 사용한다.
poll 10,832회 중 50ms 상한으로 호출한 poll은 587회이고, 누적 poll 소요는
265.365ms였다. 이 결과는 thread 통합과 poll 호출 조건 변경을 함께 적용한 값이다. 제출 302,391건과
완료 정리 302,391건이 일치했으며, requester/progress/settlement managed thread ID는
모두 `5`, `IsThreadPoolThread=False`였다. 원자료는 `fixed-diag.txt`, 계측 위치는
`PerfReqRep.fixed-instrumented.cs:441`, `:460`, `:505`다.

독립 리뷰에서 poll 호출 조건이라는 추가 변수를 지적해, 같은 전용 thread에서
**모든 poll에 기존 50ms 상한을 적용하는 대조군**을 실행했다. 298,715 ops/s,
poll 9,859회, 누적 251.247ms, 45ms 이상 대기 0회였다. 따라서 Wait(0) 사용 없이도
같은 thread에서 제출·완료 집계를 진행하면 높은 처리량이 유지된다. 이 대조는
분리 구조의 긴 poll 대기와 낮은 처리량이 함께 사라진다는 결론을 지지한다.
내부 notification 원인까지 확정하는 것은 아니다. 소스는 `PerfReqRep.same-thread-50.cs`,
원자료는 `same-thread-50-diag.txt`와 `same-thread-50-run.log`다. 실행 직전 load average는
`0.03, 0.09, 0.21`이었다.

여기서 실측으로 확정한 병목 경계는 **분리 구조의 completion poll 대기**다.
그 아래 Core notification이 왜 해당 동시 실행에서 즉시 poll을 깨우지 못하는지는
이번 러너 수정으로 내부 원인까지 확정하지 않았다. 이를 thread pool 비용으로
바꾸어 설명하거나 Core 결함으로 확정하지 않는다. public API만 사용한 분리 구조
재현 사본을 남겼으므로, 하위 계층 진단 여부는 감독자가 판단할 수 있다.

## 수정 구조와 정책 대응

최종 `PerfReqRep.cs:434`에서 만든 전용 OS thread가 다음 진행을 모두 소유한다.

1. `:516`에서 상한까지 request를 연속 제출하고 Task를 pending 목록에 보관한다.
2. `:493`에서 public completion poller를 진행한다.
3. `:443`에서 완료된 Task만 GetResult로 읽고 reply를 검증·집계·해제한다.
4. deadline 이후 `:551`의 bounded drain은 새 요청이나 완료 count를 추가하지 않는다.

§1.1.1: reply를 기다려야 다음 admission을 하는 규칙을 두지 않는다.
§1.1.2: requester의 연속 제출과 completion 진행을 같은 전용 thread가 교대로 한다.
§1.1.3: 합쳐진 awaitable은 기다리지 않고 보관한다. 기존 상한 64를 유지하며,
WRITABLE 처리와 재제출은 binding에 맡긴다. report에 상한을 표시한다.
§1.1.5: 측정 구간에 pool continuation을 등록하지 않는다. Task 완료는 public poller를
호출하는 thread가 진행하고, 같은 thread가 결과를 소비한다.

비교한 대안은 분리 thread에서 직접 집계하는 구조와 제출·poll·집계를 같은 thread에
두는 구조다. 전자는 상태 동기화가 남고 실측 병목도 유지되므로 채택하지 않았다.

진행 소유 규칙 수는 **수정 전 3개(제출 thread·poll thread·pool 집계) → 수정 후
1개(requester thread)**다. 별도 inFlight 복사와 집계 lock 대신 pending 목록이
미완료 개수를 소유한다. 신규 retry, timer, 소켓, public API는 없다.

## 다른 언어와의 대조

| 러너 | completion 진행 및 완료 집계 |
|---|---|
| C | `bindings/c/perf/single/common/perf_single_reqrep.hpp:417`에서 연속 제출 중 64회마다 drain하고, backpressure 뒤 completion을 진행한다. |
| C++ | `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:445`의 requester가 poller와 자체 ready queue를 소유한다. `:467`에서 ready queue를 직접 실행한다. |
| Java | `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfSocketReqRep.java:185`에서 제출과 public completion poll을 교대로 수행하고, `:274`의 whenComplete를 poll 진행 thread에서 실행한다. |
| Node | `bindings/node/perf/single/perf_socket_reqrep.ts:142`의 pending Promise 집합을 `:162`의 연속 제출과 `:173`의 자기 loop 진행으로 정리한다. replier worker와 requester의 진행을 분리하되 requester Promise를 다른 thread의 loop에 넘기지 않는다. |
| 이전 .NET | requester와 completion poller가 다른 thread에 있고, await 이후 집계는 RunContinuationsAsynchronously에 의해 pool로 간다. |
| 수정 .NET | requester가 public poller로 Task를 완료시키고, 같은 thread가 IsCompleted/GetResult로 정리한다. |

.NET Task completion source는
`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1169`에서
`RunContinuationsAsynchronously`를 지정한다. Java의 whenComplete나 C++의 자체
ready queue와 달리, 이전 .NET 러너의 ConfigureAwait(false)는 requester로 복귀하는
장치가 아니다. binding을 변경하지 않고 러너에서 continuation 등록 자체를 없앴다.

## 계측 없는 before/after와 검증

| 조건 | throughput | mean latency | p95 | p99 |
|---|---:|---:|---:|---:|
| 감독자 제공 before | 3,542 | 미제공 | 미제공 | 미제공 |
| 복원한 before, 계측 없음 | 2,930 | 21.251ms | 50.619ms | 51.164ms |
| 수정 after, 계측 없음 | 281,288 | 0.183ms | 0.326ms | 0.405ms |

원본 stdout은 `.artifacts/dotnet-reqrep-diagnosis/before-run.log`와 `after-run.log`다.
각각 success 1, fail 0, RESULT 5줄이며, after에서 임시 계측은 모두 제외했다.
추가로 진입 시점 수정안을 주 작업 트리에서 측정한 결과는 299,574 ops/s였다.
report 상한 필드 추가는 이 최종 측정 뒤 수행한 출력 변경이며 측정 loop는 동일하다.

- `dotnet build bindings/dotnet/perf/single/Zlink.BindingBench/Zlink.BindingBench.csproj -c Release -m:1`:
  warning 0, error 0.
- `dotnet build bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj -c Release -m:1`:
  warning 0, error 0.
- 지정 native artifact를 `ZLINK_LIBRARY_PATH`로 설정한 Release 테스트:
  `test_request_reply|test_pull_completion_contract` 14 passed, 0 failed, 0 skipped.
- 같은 환경의 전체 기존 테스트: 232 passed, 0 failed, 0 skipped, 12초.
- Effective Options 함수의 기본값·최솟값·잘못된 값·Int32 범위 초과 입력 8건 확인 통과.
- 격리 사본과 작업 트리의 binding source 135개 파일 내용 일치, 측정한 최종
  `PerfReqRep.cs`의 SHA-256도 작업 트리와 일치.
- 수정 파일 `git diff --check` 통과. 소스에 임시 계측은 남기지 않았다.

첫 focused test 명령은 클래스명 filter가 맞지 않아 0건이었다. 실제 클래스명으로
수정해 위 14건을 실행했으며 0건 실행은 통과 수에 포함하지 않았다. 빌드 중 nullable
경고 1건은 기존 non-null annotation을 복구해 해결했고 최종 빌드는 warning 0이다.

## 독립 리뷰 반영

원칙 준수와 소스·실측 부합을 독립 리뷰했다. 중대 finding은 없었다.

- poll 호출 조건도 바뀌었다는 지적을 채택했다. 최종 구조의 Wait(0)/Wait(50) 조건을
  명시하고, 항상 Wait(50)인 추가 대조군을 실행해 결과를 위에 기록했다.
- `blockingWaits`는 실제 block 여부가 아니라 양수 timeout 호출 수라는 지적을
  계측 코드로 확인하고 표현을 정정했다.
- 282µs의 기준 throughput을 감독자 제공 3,542로 명시했다.

Markdown을 HTML로 렌더해 표 4개와 heading 구조를 확인했다. 렌더 결과는
`.artifacts/dotnet-reqrep-diagnosis/diagnosis.html`에 보존했다.

## 감독자 판정 사항

- 러너 소유: 측정 실행과 Task 결과 소비. 근거는 single 정책 §1.1.2·§1.1.3·§1.1.5.
- binding 소유: admission, WRITABLE 재제출, completion drain 및 Task 완료.
  공개 근거는 `Contracts/Messaging/OperationContracts.cs:165`와
  `Contracts/Eventing/Poller.cs:21`의 remarks.
- 교차언어 대조: C++·Java와 같은 thread 내 제출/progress 교대 구조로 정렬했다.
  Node는 자신이 소유한 loop에서 Promise 진행을 수행하는 구조다.
- 변경 분류: **B — 러너의 기존 진행 소유 결함**. Framework runtime 변경은 없다.
- 분리 thread의 native completion wake 지연은 내부 원인 미판정이다. 정책이 분리
  thread도 허용하므로, 보존한 public API 재현 사본으로 하위 계층을 별도 조사할지
  감독자 판단이 필요하다. timeout을 줄이는 보정이나 하위 계층 수정은 하지 않았다.
- 다른 셀은 미측정이다. ROUTER_ROUTER_REQREP도 같은 RunRequestLoop를 사용하지만
  사용자 허용 범위에 따라 성능 실행하지 않았다.
