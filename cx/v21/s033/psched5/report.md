# #1639 C++ scheduler 진단 — 구현 조건 확인 대기

요청한 작업은 **미완료**다. 기존 branch에서 지연과 worker 비용 증가를 재현하고, Spot gate의 제출·실행 구간을 계측했다. `blocked`의 의미를 확인하는 질문은 아직 답변을 받지 못했다. Runtime 변경과 base/narrowed 3회 비교는 실행하지 않았으며 머지 가능하다고 판정하지 않는다.

## 작업 범위와 변경

- 원본 `/root/worktree/zlink-v21-s033/psched`, `perf/1639-cpp-sched-drain`은 수정하지 않았다.
- 별도 worktree: `/root/worktree/zlink-v21-s033/psched5`; 새 branch: `perf/1639-cpp-worker-wake`; HEAD: `37f78cba87`.
- 새 worktree에는 기존 측정과 같게 bench pacer `4b40d0f10f`, `8cab98383e`의 diff만 적용했다. Commit·push·PR·GitHub·spec 변경은 없다.
- 변경 파일은 `framework/bench/grpc/{README.en.md,README.ko.md,cpp/client/bench_cpp_client.cpp,cpp/run_local.sh}`다. Scheduler와 correctness test의 source는 변경하지 않았다.
- 작업용 harness는 이 JOB 안에 있다. Shell의 background 실행을 Python `Popen` supervisor로 옮겼으며 정리는 SIGTERM만 사용한다. `/proc/<pid>/task` 조회 중 thread가 종료될 때 발생하는 ESRCH를 수집기에서 처리했다. 측정 부하·pacer·CPU·warmup·duration은 유지했다.
- Wake 판정 소유 위치는 변경 전/후 **1 → 1**: `runtime/dispatch/offload_executor.cpp:179`의 `reserve_workers_locked`. 그 안의 worker 생성(`:206`)과 idle worker 예약(`:216`)도 변경하지 않았다. 새 runtime flag·counter·queue·lock은 없다.

## 진단

아래 C++ 경로는 `/root/worktree/zlink-v21-s033/psched5/framework/languages/cpp/framework/src/` 기준이며, 코드가 같은 원본 `37f78cba87`에도 동일한 줄 번호가 적용된다.

### Worker 비용 증가

`runtime/dispatch/offload_executor.cpp:182–196`은 `_queue`의 owner 수 대신 각 drain source의 `pending()` record 수를 worker demand에 더한다. `:199–204`는 idle/reserved worker만 가용 수에 더하며 running worker의 drain 진행은 demand에서 빼지 않는다. 이어 `:206–223`에서 추가 worker를 만들거나 여러 idle worker를 예약한다. `:478–479`에서는 worker가 다음 작업을 고르기 전에도 이 판정을 다시 실행한다.

Mesh application pool은 `runtime/mesh/mesh_node_host_service.cpp:844–850`에서 registration 수 + CPU 수까지 늘어난다. 이번 CPU 8–15 조건에서는 한 receive role과 최대 8 application worker가 같은 pool에 참여할 수 있다. Node record를 이어 처리하는 기존 경로는 `:3975–4002`다. 이 drain 경로가 진행 중이어도 새 record 수가 다른 worker 예약 수요로 계산된다.

22k 진단 결과는 다음과 같다. **perf 기록을 함께 실행한 진단값이며, merge 비교용 무계측 값이 아니다.** 기존 보고서의 app-worker 59.771 µs/msg와 아래 77.502를 직접 비교해 추가 회귀로 판정하지 않는다.

| 항목 | 기존 branch 진단값 |
|---|---:|
| 실제 window 처리량 | 30,633.6 msg/s |
| app-worker CPU | 77.502 µs/msg |
| receive CPU | 19.603 µs/msg |
| target voluntary context switch | 2.552/msg |
| target I/O sendto | 0.523/msg |
| target I/O 유효 주파수 | 2.589 GHz |

원자료: `diagnosis/22k/1/22k/branch-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture/`의 `A-perfstat.csv`, `A-*-before.tsv`, `A-*-after.tsv`, `cpu.data`, `cpu.txt`, `sched.data`. 집계: `diagnosis/22k/1/22k/summary-22k.json`.

app-worker CPU sample 1,052개 중 312개(29.7%)의 stack에 futex 경로가 있었고, 146개(13.9%)에는 condition-variable 경로, 84개(8.0%)에는 `__lll_lock_wait`가 있었다. 이 비율은 서로 겹친다. Worker loop의 mutex 해제·재획득과 `try_submit_drain`의 notify 경로가 확인된다. 원자료는 `diagnosis/cpu-counts.json`과 해당 `cpu.txt`다.

따라서 관측된 증가는 scheduler의 사용자 공간 busy-spin보다 **반복 notify와 mutex/futex 경합**에 부합한다. `offload_executor.cpp:484–487`의 idle wait는 condition-variable wait다. Mesh idle timeout은 100 ms이므로 worker 9개의 순수 idle timeout만으로는 초당 약 90회 수준이며, 30k msg/s에서 메시지당 1회 이상 늘어난 context switch를 설명하지 못한다. CPU 비용 증가 전부를 각 함수에 정량 배분한 것은 아니다.

### Spot 요청 대기

Spot handler stack은 `serial_execution_queue_t::run_shared_gate → execute_item → invoke_erased → pm_spot_t::probe`였다. Spot pool은 `runtime/spots/spot_runtime.cpp:948–960`, gate 연결은 `:987–1000`에서 생성·연결된다. Mesh Node drain pool과 **별도 pool**이므로 같은 executor queue에서 Node drain 뒤에 쌓인다고 설명할 수 없다.

확인한 구간은 다음과 같다. `diagnosis/pm-queue/`에서 완전히 관측된 요청 239개를 같은 HTTP coroutine frame과 Spot handler 진입으로 연결했다. 시작/종료 경계의 불완전한 요청은 단계 집계에 넣지 않았다. 전체 HTTP probe는 250개다.

| 구간 | p50 µs | p99 µs | 코드 위치 |
|---|---:|---:|---|
| HTTP handler 진입 → Spot gate 제출 | 1,192 | 5,489 | gate 제출: `runtime/execution/serial_execution_queue.cpp:2795–2811` |
| gate 제출 → worker의 gate 실행 | 33 | 2,710 | 위 제출 → 같은 파일 `:2952` |
| gate 실행 → Spot handler 진입 | 40 | 67 | 같은 파일 `:2984–2992`, `:3590–3612`; `runtime/spots/spot_runtime.cpp:4622` |
| Spot handler 진입 → HTTP coroutine 재개 | 134 | 3,503 | request/reply와 HTTP continuation을 합한 구간 |

원자료: `diagnosis/pm-queue/run1/zlink-framework-cpp-send-saturation-1024/capture/{stages.data,stages.txt,events.json,request-stages.json,pm-probes.json,pm-latencies.json}`. Handler stack 예시는 `diagnosis/pm-stacks/run1/zlink-framework-cpp-send-saturation-1024/capture/spot-example-stack.txt`다.

**확인된 worker-wait 위치는 gate 제출(`serial_execution_queue.cpp:2809`)과 실행(`:2952`) 사이**다. 하지만 중앙값에서 더 큰 대기는 gate 제출 전이다. 이 구간의 route 준비·infrastructure 진행·admission 중 어느 세부 단계가 주원인인지는 아직 분리하지 못했다. Reply submit만의 지연도 마지막 구간에서 분리하지 못했다. 그러므로 “Spot 지연 원인은 Node drain 뒤 queue 대기 하나” 또는 “capacity admission 하나”라는 결론은 내리지 않는다.

`diagnosis/pm-all/`의 system-wide sched 기록에서는 Spot worker가 450회 실행되었고 합계 run time 22.169 ms, runnable delay 47.142 ms였다. 원자료: `diagnosis/pm-all/{spot-timehist.txt,spot-timehist-detail.txt}` 및 `run1/zlink-framework-cpp-send-saturation-1024/capture/sched.data`. 처음 PID 부착 방식은 뒤에 생성된 thread를 충분히 관측하지 못해 CPU 8–15 전체 기록으로 보완했다.

PM 지연은 기존 branch에서 재현됐다. `diagnosis/pm-queue/`의 전체 250개 probe에서 p50 **1,684.509 µs**, p99 **7,331.983 µs**였다. Setup은 기존 `run-B.sh`처럼 `/pm/request` 성공을 기다렸다. 모든 측정은 `flock -o /root/jobs/fx/cx/measure.lock` 아래 source 0–7 / target 8–15로 실행했다. 실패한 `diagnosis/pm-stages/`와 초기 불충분한 자료도 삭제하지 않았다.

## 구현을 막는 조건

현재 `runtime/dispatch/offload_executor.hpp:150–160`의 worker phase는 `idle/reserved/running`뿐이다. `running`은 callback 실행 중임을 뜻하며 CPU 계산, mutex 대기, `shared_future.wait()`를 구별하지 않는다. Callback이 대기를 시작할 때 executor로 알리는 경로도 없다.

기존 correctness test에는 한 번의 publication 뒤 **추가 publication 없이** 다음을 요구하는 사례가 있다.

- `tests/Zlink.Framework.UnitTests/test_cpp_framework_scheduler_drain.cpp:184–225`: 최초 handler 네 개가 모두 진입한 뒤에야 release한다.
- 같은 파일 `:279–316`: A handler가 `shared_future.wait()`에서 대기해도 B는 A terminal 전에 다른 worker에서 실행되어야 한다.

따라서 “실제 handler 대기일 때만 한 worker를 더 깨움, proactive fan-out 없음”을 현재 phase만으로 적용하면 첫 handler의 대기 이후 깨움 결정을 실행할 계기가 없다. 한 worker만 깨우도록 축소하는 것만으로는 이 두 진행 조건을 유지할 수 없다. Wake bound를 줄이는 수정으로 해결되는 문제가 아니다.

선택지는 다음과 같이 다르다.

| 해석 | 필요한 조치 | 현재 제약과 관계 |
|---|---|---|
| blocked = 기존 running worker가 작업을 맡아 사용할 수 없음 | worker당 한 번씩 추가 예약하는 조건을 구체화 | 실제 대기를 감지하지 않으며 CPU 계산 중에도 추가 worker를 시작할 수 있음 |
| blocked = 실제 handler 대기 | 대기 시작을 executor가 관측할 방법 필요 | 기존 phase만으로 표현할 수 없고, 단순 wake rule 축소를 넘는 설계가 필요 |

질문은 첫 번째 진단 실행 중 전송했으며 아직 답변이 없다. 이 의미가 정해지기 전에 임의의 감지 상태·timer를 추가하거나 test의 진행 조건을 완화하지 않았다.

### 결정 소유와 교차언어 대조

| 결정 | spec 소유 조항 | 현재 구현 위치 | 유지할 소유자 |
|---|---|---|---|
| Node/Channel handler의 독립 실행 | S02 §2 | C++ `runtime/mesh/service_mailbox.cpp:239`, 위 scheduler test `:279`; .NET `Runtime/Execution/ZLinkProcessExecutionPool.cs:438`; Java/Kotlin 공용 `execution/ZLinkWorkerPool.java:59`; Node `runtime/backend/mesh-dispatch-pump.ts:146` | 각 언어의 application scheduler; public 진행 계약 유지 |
| 실행 자원·worker 깨움 | S02 §10 | C++ `runtime/dispatch/offload_executor.cpp:179`; .NET `Runtime/Execution/ZLinkProcessExecutionPool.cs:307`; Java/Kotlin 공용 `execution/ZLinkWorkerPool.java:59`; Node `runtime/backend/mesh-dispatch-pump.ts:146` | 기존 scheduler 한 곳 |
| Node worker의 연속 drain 예산 | S02 §9, Framework API §11 | C++ `runtime/mesh/mesh_node_host_service.cpp:3956–4002`; .NET pool의 ApplicationSource, Node pump `:636`; Java/Kotlin은 JVM 실행 자원 사용 | 기존 owner drain, wake 코드에 복제하지 않음 |

다른 언어의 실제 실행 자원은 S02 §10에 따라 .NET thread pool, Java/Kotlin virtual thread, Node event loop로 다르다. 이번에는 public 동작 변경과 교차언어 contract 변경을 하지 않았다. Wake 수정 후보는 Framework 내부의 기존 결함(B)을 대상으로 하지만, 구현이 없으므로 A/B 승인을 받은 완료 변경으로 보고하지 않는다.

## 시험

원본과 같은 runtime source로 별도 Release 시험 build를 진행 중이다. 결과는 종료 후 갱신한다.

- Configure: `configure-tests.sh` (`Release`, `-O3 -DNDEBUG`, test probes ON, install OFF, 원 측정과 같은 Core/binding/vcpkg prefix).
- Build: `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build /root/worktree/zlink-v21-s033/psched5/.psched5-tests --parallel 3 --target test_cpp_framework_scheduler_drain test_cpp_framework_mailbox_owner_dispatch`.
- Test: `ctest --test-dir .../.psched5-tests -R '^test_cpp_framework_(scheduler_drain|mailbox_owner_dispatch)' --output-on-failure`.
- Bash 문법 검사와 Python compile 검사는 통과했다.

## Base / narrowed 및 머지 조건

아래 base 값은 **기존 psmeas의 3회 측정**이며 이번에 새로 잰 값이 아니다. Narrowed 구현·측정이 없으므로 빈 값을 통과로 취급하지 않는다.

| 항목 | 기존 base 중앙값 | 기존 base 범위 | Narrowed | 판정 |
|---|---:|---:|---:|---|
| Target I/O sendto/msg | 0.811 | 0.800–0.820 | 미측정 | 판정 불가 |
| Spot p99 µs | 603.360 | 560.561–680.402 | 미측정 | 판정 불가 |
| Send 1 KiB msg/s | 183,257.7 | 179,445.8–187,554.3 | 미측정 | 판정 불가 |
| Request 1 KiB msg/s | 53,768.4 | 53,746.6–54,145.1 | 미측정 | 판정 불가 |
| App-worker µs/msg | 22.221 | 22.077–22.542 | 미측정 | 판정 불가 |
| Target voluntary wakes/msg | 1.385 | 1.346–1.389 | 미측정 | 판정 불가 |

원자료는 `/root/jobs/fx/cx/v21/s033/psmeas/measure/{1,2,3}/` 아래 다음 경로다.

- Send: `send-saturation/base/run1/zlink-framework-cpp-send-saturation-1024/results.json`.
- Request: `request-backpressure/base/run1/zlink-framework-cpp-request-backpressure-1024/results.json`.
- Spot: `pm/base/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json`.
- 22k: `22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture/{A-perfstat.csv,A-target-before.tsv,A-target-after.tsv,windows.json}`.

적용할 조건은 고정한다: target sendto/msg ≤ base, Spot p99의 악화가 **base 자체 범위** 이내, send/request 처리량은 base 자체 범위 이내 또는 개선, app-worker µs/msg와 wakes/msg ≤ base. 기존 summary의 양쪽 variant 범위 중 큰 값을 사용하는 판정은 새 결과에 적용하지 않는다.

`measure.sh`와 `summarize.py`의 JOB 사본은 준비했지만 실행하지 않았다. Base/narrowed 3회 결과와 `measurement-summary.md`는 없다.

## 남은 작업과 정리

- `blocked`의 정확한 조건 확인, wake rule 구현, 수정 후 두 correctness test와 negative controls 실행, Release bench build, base/narrowed 3회 측정 및 조건별 판정이 남았다.
- Spot의 gate 제출 전 지연과 reply submit의 세부 구간 분리가 남았다.
- Runtime 점검: 과도한 worker 예약 문제 1건 확인, runtime 수정 0건, 미해결 1건. 별도 이슈로 넘기지 않았다.
- 원자료는 이 JOB 아래 보존한다. Branch와 worktree도 유지한다. 시험 build 정리 결과는 시험 종료 뒤 갱신한다. 기존 psmeas build는 사용자가 제공한 자산이므로 그대로 둔다.
