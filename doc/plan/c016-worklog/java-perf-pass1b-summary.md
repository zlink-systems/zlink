# Java backpressure owner 구조 개선 — pass 1b

- **구현·기능 gate 완료, 공식 성능 승인 BLOCKED.** Context당 native completion poller·control pair·wait thread 하나를 lazy 생성하고 Context close까지 유지한다. Socket별 pending registry, submit/drain lock과 bounded completion lane은 기존 것을 사용한다.
- 최종 비계측 진단에서 **DD 4096B 305,913.6 msg/s, DD 64KiB 20,706.0 msg/s**. Pass 1 before 회복 기준 227,142.4 / 14,012.0보다 각각 **34.7% / 47.8%** 높다. Pass 1 after 130,893.0 / 8,484.8 대비 각각 **133.7% / 144.0%** 개선이다.
- 지정 공식 shell은 **Core freshness guard에서 EXIT 1**. 따라서 아래 20/20 결과는 공식 report의 대체 승인 자료가 아닌 별도 진단이다. 전체 성능 판정을 닫지 않는다.
- detached 작업 트리와 pass 1 미커밋 변경 보존. stash/checkout/reset/branch 변경/commit/push 없음. Core configure/build/clean 없음. Core build symlink와 binary 유지.

## 계약 근거와 언어별 경로 대조

소유권 근거는 Java spec에서 참조하는 공통 async 계약이다. 사용자가 지목한 Java 175–185는 Native Wait Boundary, 987–994는 Pull completion 도입부이며 idle 종료나 socket별 worker 생성을 요구하지 않는다.

- `bindings/doc/spec/async-execution-model.ko.md:69–80` §4: **“Socket을 public poller에 `PollCompletion`으로 등록하지 않았으면 binding runtime이 owner다.”** 등록/제거 시 원자적 이전, **“두 owner가 같은 queue를 동시에 비우지 않는다.”** Public owner에서는 그 `wait()`에 의존한다. 따라서 Java SEND/REQUEST의 runtime 진행 자체를 없애는 후보는 제외한다.
- `bindings/doc/spec/java/README.ko.md:564`, `:646–651`, `:993–1003`: registry/drain owner는 binding 소유이며, late completion 정리와 정확히 한 번의 완료, public wait가 완료 처리를 마친 progress event를 보장한다. Context pump는 native drain만 맡고 실제 stage settlement는 기존 socket lane으로 넘긴다.
- `core/doc/spec/core/05-polling.ko.md:123–136`: socket의 completion registration은 최대 하나. poller의 add/modify/remove/wait는 caller가 직렬화한다. 새 pump의 command queue는 이 native poller lifecycle 직렬화만 담당한다.
- `core/doc/spec/core/socket/README.ko.md:1157`, `:1265`: NO_DATA까지 단일 consumer drain; admitted API 실행 중 close는 EBUSY. Java private wait를 해제한 뒤 native socket을 닫는 순서는 .NET `PrepareClose()`와 같은 책임 경계다.

| 구분 | Java pass 1 | C++ 현재 checkout | .NET 현재 checkout | Java pass 1b |
|---|---|---|---|---|
| public POLLCOMPLETION owner | NativePoller wait → CompletionOwner drain → 기존 socket worker lane settlement | public poller → completion_owner drain | public poller → DrainCore, `_submitSync` 직렬화 | pass 1 경로 그대로 |
| public owner 없는 SEND WRITABLE | socket별 runtime worker/poller가 drain하고 재제출 | `register_send_entry` map 등록, 현재 코드의 SEND retry는 public poller 진행에 의존; REQUEST fallback도 중단 | runtime pump가 drain, token 이후 등록 | Context pump가 해당 socket의 기존 drain/retry 호출 |
| public owner 없는 REQUEST | socket별 runtime worker/poller | 내부 REQUEST fallback thread/poller | runtime pump | 같은 Context pump |
| owner 이전 | runtime 종료/join 후 public add; remove 뒤 필요 시 재생성 | stop_runtime_owner 후 public owner, generation으로 기존 turn 종료 | StopRuntimePump, 기존 drain turn 종료 | public flag 게시 → Context pump에서 native remove 완료 확인 → public add; 반환 때 필요하면 같은 pump에 재등록 |
| control wake | backpressure episode마다 PAIR 생성; idle 때 close | 별도 control socket 없음; REQUEST fallback은 기존 25ms wait | 별도 control socket 없음; 기존 25ms wait | Context당 PAIR 1쌍; 등록/해제/Context close 때만 signal |
| idle | pending empty이면 worker/poller/pair 종료 | REQUEST owner는 idle pending 여부로 poller를 반복 생성하지 않음 | idle에 pump task 종료, poller는 유지 | 하나의 platform wait thread가 native wait에서 block; spin/timer 없음 |
| 완료 lane | Context의 bounded pool + socket별 FIFO lane | async_operation_state의 resume/scheduler 계약 | RunContinuationsAsynchronously | Java 기존 lane 그대로 |

C++ 근거: `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:28–79`, `completion_owner.cpp:562–608,688–805`, `async_operation_state.hpp:70` 이후. 특히 현재 checkout의 SEND는 `completion_owner.cpp:593`의 public-poller 전용 설명과 `_send_entry_count` 분기가 있다. 이를 Java의 runtime SEND 진행 계약에 그대로 복사하지 않았다. .NET 근거: `CompletionOwner.cs:19,39–99,373–415,595–705,790`.

## 설계 비교와 선택

| 후보 | 리소스와 제어점 | 확인 결과 / 판정 |
|---|---|---|
| control pair만 유지 | socket별 worker/poller lifecycle 그대로 | pass 1 계측 −20.1%, 기각된 후보 유지하지 않음 |
| socket별 worker/poller까지 유지, virtual native idle | 생성은 줄지만 idle native FFM wait가 carrier를 점유 | 100 clients에서 owner 18개 이후 정지, 790 submit, 15초 외부 진단 종료. 기각 |
| virtual owner + 기존 ownerLock idle 조건 대기 | socket별 100 owner, native 리소스 100개, virtual resume 필요 | 3초 계측 DD64K 4,485.7 msg/s, 기각 |
| socket별 platform owner + native idle | timer/idle 조건은 삭제 가능하지만 native wait thread 100개 | 비계측 DD4096 53,888.0 / DD64K 3,108.4 msg/s, 기각 |
| **Context 공유 owner** | **poller·PAIR·native wait thread 각 1개**, socket별 등록만 보관 | **채택**. 100 socket의 64K native 계측 poller 1/1, PAIR 1쌍. 최종 비계측 DD 두 회복 기준 충족 |
| Core completion poller wake 사용 | control socket 제거 가능성 | 현재 공개 Core header에 별도 poller wake/interrupt API 없음. 새 API/spec/Core 변경이 필요해 제외 |
| public owner가 있으면 runtime 생략 | public owner만 drain | 이미 pass 1이 보장하던 경로이므로 그대로 유지. public owner 없는 DD의 해결책은 아님 |

작은 삭제 후보 세 개가 정지 또는 성능 하락을 보였으므로 Context 공유 native wait 책임을 `CompletionPump`로 옮겼다. `CompletionOwner`에서는 pass 1 대비 381줄 삭제/40줄 추가, `ControlWake` 구현도 복사본 없이 pump로 이동했다. Pump의 registration map은 native poller 소유권만 보관하며 operation token·재제출 상태는 기존 socket pending map 한 곳에 남는다. Command queue는 Core가 요구하는 add/remove/wait 직렬화를 위해 필요하다. 새 timeout, spin, thread pool, in-flight cap, retry budget, fairness 설정은 없다.

## 변경 파일 — 이 pass에서 추가한 범위

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`: socket-local token/retry/settlement 유지; per-socket runtime worker/poller/wake lifecycle 제거. Public handover와 native close는 공유 pump의 registration 제거를 기다린다.
- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionPump.java` (신규): Context의 native wait 리소스와 등록/해제 직렬화 소유. Socket drain은 기존 CompletionOwner로 위임하며 drain 오류는 해당 socket의 pending을 실패로 정리한다.
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/CompletionDispatcher.java`: 기존 Context owner에 pump 한 개 연결; completion worker pool과 FIFO lane 실행은 변경하지 않음.
- `bindings/java/src/main/java/systems/zlink/runtime/core/NativeContext.java`: ctxTerm 전에 binding-owned wait 리소스를 회수하여 Context 종료를 막지 않도록 함.
- `bindings/java/src/test/java/systems/zlink/runtime/sockets/CompletionOwnerLifecycleTest.java` (신규): backpressure 8회 idle 재사용, socket close의 registration 회수, Context close의 thread/wake 회수, carrier 수보다 많은 socket 공유, public owner 전환 중 runtime 소비 금지.

Pass 1의 NativePoller/InternalAccess/NativeSocketBase/CompletionBatchContractTest와 `run_benchmarks.sh:851` 변경은 그대로 보존했다. 이 pass는 러너 source·scheduler·drain·fairness, public contracts, spec/doc, Core, 다른 binding을 수정하지 않았다.

## 최종 native lifecycle 계측

동일한 실제 Java DD runner, tcp/100 clients/64KiB/3초. NativeSymbols downcall 앞의 별도 진단 counter이며 최종 정규 JAR/src에 hook 없음. Setup/teardown 포함, throughput 판정용 계측이 아니다.

| 구분 | submit | owner poller 생성/파괴 | control pair | binding allocation/submit |
|---|---:|---:|---:|---:|
| pass 1 after 계측 | 28,888 | 4,753 / 4,753 | 4,753쌍 | 564.64 B |
| pass 1b 최종 계측 | 66,425 | **1 / 1** | **1쌍** | 368.91 B |

최종 main `zlink_socket` 102회 = application 100개 + control PAIR 2개. Native poller add 101개는 control reader 1개 + application 100개다. Backpressure 횟수에 비례하는 poller/control 생성은 없으며 **socket당 O(1)보다 강한 Context당 O(1)**이다. Native registration은 socket별로 존재하고 public handover 때 remove/add된다. Runtime owner field·membership flag를 socket마다 중복 보관하지 않는다.

## 제출 미니벤치

같은 Core, public API inproc DD, body+empty tail, 각 size 5,000 warmup + 20,000 측정, before/after를 번갈아 3개 JVM씩 실행한 중앙값이다. `ThreadMXBean` allocation과 submit 범위 nanoTime. Body 생성·recv를 submit 값에 포함하지 않는다. 이 inline-admitted mini에는 backpressure가 없으며 생성 lifecycle 개선의 처리량 추정치로 사용하지 않는다.

| DD body | submit ns/건 before→after | submit B/건 before→after | 전체 main B/건 before→after |
|---|---:|---:|---:|
| 64 | 1834.2→1664.2 | 287.0→290.9 | 453.9→456.4 |
| 4096 | 1117.5→1060.8 | 272.0→272.0 | 432.3→432.3 |
| 65536 | 1176.5→1165.4 | 272.0→272.0 | 432.3→432.3 |

64B의 JIT/초기 allocation 차이와 ns 변동을 구조 개선으로 단정하지 않는다. 4096/65536의 inline submit allocation은 272B/건으로 유지된다. 실제 backpressure 경로의 lifecycle 결과는 위 별도 계측으로 판단한다.

## 20-case after — 공식 shell과 분리한 진단

- 공식 요청 명령을 최종 JAR build 후 재실행했으나 `core runtime is older than core source`로 종료 코드 1. 첫 시도도 같은 guard 결과였다. 메인 `core/src/runtime/sockets/common/socket_base_dispatch.cpp`가 작업 트리와 달라 pass 1의 “내용 동일 + 원래 소스보다 binary가 새로움” 예외에 해당하지 않는다. Guard를 완화하지 않았다.
- 진단은 동일 최종 Java PerfMain을 직접 실행했다. DD/PUBSUB의 START 게이트, REQREP의 즉시 실행, 원래 wire stop-token과 각 role의 RESULT 의미를 보존했다. io threads=4, parts=2, auto HWM balanced, timeout 200ms, monitor HWM 4,096,000, connect concurrency 128, pattern transition 3초. 러너 source 수정 없음. Shell report 생성/guard와 분리되어 있으므로 **공식 측정과 동급으로 주장하지 않는다**.
- 2026-09-05 **13:24:51 KST**, 1분 load **2.2246 ≤3**에서 시작, **13:27:07**까지 20/20 complete, 모든 client/server exit 0. 측정 중 Java gate/mini/counter를 병렬 실행하지 않았다.
- Core SHA-256 `93ad7f1161156d34aecf5550f25d1d8630d18cb657eee24eecc135e578050918`. final.jar embedded Core와 지정 shared binary hash 동일. 실제 client `/proc/<pid>/maps`에서도 동일 hash의 native cache 경로를 확인했다.
- 아래 before는 **pass 1의 공식 after**, C는 사용자가 제공한 paired C report다. 비율 평균은 size별 비율의 산술평균. One-way와 REQREP throughput/latency 측정 의미 및 raw latency ms 단위 유지.

| Pattern | Before/C 평균 | After/C 평균 |
|---|---:|---:|
| DEALER_DEALER | 54.66% | 65.01% |
| DEALER_ROUTER_REQREP | 52.63% | 54.26% |
| ROUTER_ROUTER_REQREP | 72.48% | 72.66% |
| PUBSUB | 88.43% | 78.43% |

| Pattern | B | C | Before: pass 1 after | Pass 1b 진단 after | 변화 | After/C | latency before→after ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,043,822.0 | 859,018.4 | 780,924.2 | -9.1% | 74.8% | 1.294→0.468 |
| DEALER_DEALER | 256 | 1,007,628.4 | 774,895.6 | 642,171.8 | -17.1% | 63.7% | 0.414→0.294 |
| DEALER_DEALER | 1024 | 903,580.6 | 608,627.6 | 687,909.8 | +13.0% | 76.1% | 0.469→0.882 |
| DEALER_DEALER | 4096 | 365,304.0 | 130,893.0 | 305,913.6 | +133.7% | 83.7% | 0.425→1.026 |
| DEALER_DEALER | 65536 | 77,680.2 | 8,484.8 | 20,706.0 | +144.0% | 26.7% | 0.589→5.251 |
| DEALER_ROUTER_REQREP | 64 | 192,764.0 | 83,562.2 | 82,986.8 | -0.7% | 43.1% | 0.500→0.505 |
| DEALER_ROUTER_REQREP | 256 | 168,158.8 | 75,020.2 | 66,562.6 | -11.3% | 39.6% | 0.556→0.627 |
| DEALER_ROUTER_REQREP | 1024 | 174,445.2 | 61,521.4 | 61,388.8 | -0.2% | 35.2% | 0.676→0.678 |
| DEALER_ROUTER_REQREP | 4096 | 141,283.0 | 57,121.6 | 58,781.0 | +2.9% | 41.6% | 0.727→0.708 |
| DEALER_ROUTER_REQREP | 65536 | 22,592.4 | 22,477.4 | 25,278.0 | +12.5% | 111.9% | 1.873→1.724 |
| ROUTER_ROUTER_REQREP | 64 | 141,509.0 | 80,535.0 | 76,126.2 | -5.5% | 53.8% | 0.524→0.550 |
| ROUTER_ROUTER_REQREP | 256 | 132,159.6 | 67,803.6 | 62,437.4 | -7.9% | 47.2% | 0.618→0.662 |
| ROUTER_ROUTER_REQREP | 1024 | 123,511.2 | 63,698.6 | 53,849.2 | -15.5% | 43.6% | 0.656→0.770 |
| ROUTER_ROUTER_REQREP | 4096 | 109,366.2 | 53,725.4 | 57,765.0 | +7.5% | 52.8% | 0.779→0.721 |
| ROUTER_ROUTER_REQREP | 65536 | 13,471.0 | 20,677.6 | 22,343.6 | +8.1% | 165.9% | 2.105→1.955 |
| PUBSUB | 64 | 618,285.4 | 565,977.6 | 541,395.0 | -4.3% | 87.6% | 1555.019→1556.529 |
| PUBSUB | 256 | 749,648.4 | 560,947.4 | 302,283.6 | -46.1% | 40.3% | 1831.994→2185.459 |
| PUBSUB | 1024 | 792,925.2 | 620,054.8 | 483,445.0 | -22.0% | 61.0% | 1244.798→1444.768 |
| PUBSUB | 4096 | 681,708.6 | 536,044.8 | 466,403.4 | -13.0% | 68.4% | 430.823→482.084 |
| PUBSUB | 65536 | 62,915.8 | 74,846.4 | 84,859.8 | +13.4% | 134.9% | 121.206→136.467 |

회복 기준 두 개는 진단에서 충족하지만 DD64/256은 pass 1 after보다 하락했다. DD64KiB latency도 **0.589→5.251ms**로 증가했으므로 throughput 개선만 보고 latency 개선으로 표현하지 않는다. 앞선 후보 진단의 DD4096은 197,625.4 msg/s였고 최종 한 번의 측정은 305,913.6이다. 두 값을 모두 보존하며 단일 run의 변동성을 숨기지 않는다. 이 pass는 전체 pattern의 C 대비 성능 목표 달성을 주장하지 않는다.

## Gate와 API

- `bindings/java/tests/run_tests.sh` **PASS**: :test 105개(104 PASS, 기존 disabled 1), :integrationTest 25 PASS, Netty 3 PASS, Kotlin 4 PASS. 합계 **136 PASS / disabled 1 / failure 0 / error 0**.
- 샘플 **7/7 PASS**: RequestReplyAsync, PairRecv, PubSubRecv, DealerRouterRecv, StreamRecv, StreamPacketCallback, MonitorRecv.
- `CompletionBatchContractTest` 3 + `CompletionOwnerLifecycleTest` 2 + `DontWaitBackpressureContractTest` 4 = **9개 ×5회 PASS**. 각 :test/:integrationTest에 `--rerun` 적용. 최종 lifecycle assertion 정리도 이 반복에 포함했다.
- 공개 contract/module classfile **108개 byte-for-byte 동일**. Message ownership, Core DONTWAIT 결과 및 오류 타입 mapping, token 등록/재제출, cancellation과 late completion 정리, 기존 public poller settlement 경로는 유지했다. 새 CompletionPump는 non-exported runtime class다.
- `git diff --check` **PASS**. 임시 printf/stacktrace 계측 없음. 최종 정규 jar에 ProfileCounter class 없음.
- 초기 검증의 실패는 보존: 첫 Gradle 필터가 bare `test`라 Kotlin 하위 test에도 적용돼 `No tests found`(기능 실패 아님); socket별 persistent 후보의 native close 실패 및 virtual carrier 정지. 최종 전체 gate에는 이 실패가 남지 않았다.

## 소유권·분류·spec gap

- 소유 계층: **Java binding**. Socket별 CompletionOwner는 operation state/retry/settlement, Context CompletionPump는 native wait/registration lifecycle. Framework/Core 책임을 복제하지 않았다.
- Spec 조항: 공통 async execution §4, Java Pull completion, Core polling §4–5 및 socket close의 admitted-call EBUSY 계약.
- 교차언어 결과: .NET의 submit/drain 직렬화와 close 전 pump 정지, C++의 token 이후 등록/단일 drain을 참고했다. Java의 native FFM virtual carrier와 socket별 private lifecycle 비용 때문에 Context 공유 wait를 적용하며 기존 Java bounded settlement lane을 보존했다.
- 변경 분류: **B 기존 binding 성능/lifecycle 결함**. 공개 contract 적응이나 Framework 우회가 아니다.
- **새 spec gap 없음**. Spec은 runtime drain owner가 socket별 독립 thread/poller여야 한다거나 idle에 종료해야 한다고 요구하지 않는다. 기존 D-B102의 disabled monitor test는 그대로. 공통/Java spec의 provisional 등록 서술과 기존 0.17 token-after-submit 구현 간 drift는 pass 1에서 이미 기록한 사항이며 이 pass에서 변경하지 않았다.

## BLOCKERS

1. **공식 benchmark 승인 불가:** 메인 Core source와 고정 binary의 freshness/내용 관계를 이 작업 범위에서 복구할 권한이 없다. Core build 금지와 기존 guard 유지 지시를 지켰다. 공식 shell after report는 생성되지 않았으며, 진단 after report만 보존했다.
2. **성능의 한계:** DD64/256의 하락, DD64KiB latency 증가, C 대비 남은 격차 및 1-run 변동성. DD4096/64KiB 회복은 진단 결과로만 확인했다. 더 높은 수치로 대체하기 위한 재측정은 하지 않았다.
3. 기능 gate blocker는 없다. 구현은 미커밋 상태로 남기며 공식 성능 승인을 주장하지 않는다.

## 실행 환경·증거

- JDK `/home/hep7hep7/.jdks/jdk-22.0.2+9`, `GRADLE_OPTS=-Dorg.gradle.workers.max=2`. 실행 전 `ZLINK_CORE_SOURCE=local` 및 `bindings/tools/local_core_runtime.sh` source. 마지막 `./gradlew --stop` PASS (`No Gradle daemons are running.`); `gradle-stop.log`에 보존.
- 요약: `/home/hep7hep7/project/zlink-work/c016/java-perf-pass1b-summary.md`.
- **after report 사본**: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass1b-after-diagnostic.txt`.
- 진행: `/home/hep7hep7/project/zlink-work/c016/java-perf-pass1b-progress.md`.
- 원본: `bindings/java/build/pass1b/`: `full-gate.log`, `gate-counts.json`, `repeat-{1..5}.log`, `public-api-check.txt`, `final-official-attempt.log`, `after-diagnostic.log`, `after-diagnostic-logs/`, `runtime-counter-logs/`, `final-counter.log`, `mini-final-{before,after}-{1..3}.txt`, `pass1b-owner.diff`. 후보별 로그도 별도 이름으로 유지.

- 최종 JAR SHA-256: `b7c2c718d7c231b1f64afb977543eba32ce48f4b12523563537f44565df04fcc`. 최종 JAR의 공개 contract/module 108개 동일 및 ProfileCounter 부재를 종료 시 재확인했다.
- 별도 단발 REQUEST public close probe는 초기 idle 후보와 최종 구현 모두 `CLOSE OK`였다. 앞서 기록한 idle close 실패는 backpressure 반복 fixture에서 관측한 것이며 모든 close가 실패한다는 뜻이 아니다. `close-repro-{before,after}.txt` 보존.
- 증거 archive: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass1b-evidence.zip`.
- 종료 상태: **EXIT:1 — 공식 benchmark freshness blocker**. 구현·기능 gate·진단 20/20은 완료했지만 공식 성능 승인은 미완료다.
