# Java binding hot-path pass 2 결과

**기능 gate 완료, 공식 성능 판정 BLOCKED — EXIT:1.** Blocking ROUTER receive가 임시 `Received`를 만들고 다시 채택하던 경로를 기존 caller-storage 경로로 통합했다. 64B blocking 수신 구간의 3-JVM 중앙값은 3,325.6→3,105.3ns(시간 −6.6%), 해당 구간 managed allocation은 496→304B/건(−38.7%)이다. 공식 multi 러너는 `DONT_WAIT`를 사용하므로 이 수신 구간의 이득을 공식 REQREP 처리량 개선으로 합산하지 않는다.

- 작업 트리: `/home/hep7hep7/project/zlink-wt-java-perf2`, detached `cb2a4c8a5d` 유지. 수정은 Java runtime 1개와 test 1개다. Commit/push/reset/checkout/stash와 Core build/clean 없음.
- 지정 공식 after 명령은 load 조건을 만족한 뒤 한 번 실행했으나 Core freshness guard에서 EXIT 1. 공식 after report는 생성되지 않았다.
- 별도 진단: 동일한 고정 Core와 최종 Java JAR, tcp/100 clients/5초/1 run, 지정 20-case 전부 완료, server/client exit 전부 0. 각 case 시작 전 load≤3 확인. 진단 report를 공식 report로 간주하지 않는다.
- 공개 contract·qualified framework export·module classfile 110개는 before와 byte-for-byte 동일하다. Source 수정은 `NativeRouterSocket.java`와 `RouterReceiveStorageContractTest.java`뿐이다.

## 후보 판정

Production 소스를 수정하기 전에 별도 build classpath에서 후보를 비교했다. 아래 시간은 미니벤치의 경계를 표시한 값이다. 할당 감소를 처리량 개선율로 환산하지 않았다. §4의 public wrapper/Future pool, spin, 인위적인 in-flight 제한과 러너 변경은 시험하지 않았다.

| 후보·before 위치 | 실측·예상 효과의 근거 | 계약 보존 방법 | 가이드 §4 | 판정 |
|---|---|---|---|---|
| **Blocking ROUTER caller-storage 통합** — `NativeRouterSocket.java:60–71` | receive 496→304B/건; 64B receive 3,325.6→3,105.3ns. 전체 RPC 시간은 아래 별도 표처럼 혼재 | 기존 `routerRecvInto`에 같은 flags 전달. NO_DATA 예외/false, 이전 저장소 보존, multipart·RID·reply token을 기존 owner에서 처리 | 해당 없음 | **적용**. 수신 경계의 5% 기준 충족; 공식 DONT_WAIT 처리량 이득은 주장하지 않음 |
| native scratch의 part slice 재사용 — `CompletionOwner.java:366–375` | DD submit 272→192B/건; DR submit 424→344B/건. 비교 JVM의 DD64 3,112→3,243ns, DR64 51,563→50,596ns; 안정적인 5% 처리 시간 개선 없음 | 기존 ThreadLocal scratch가 header view까지 소유. public Message/Future 수명은 그대로 | 해당 없음 | **no-go**. 할당 효과만 확인 |
| builder와 MessagePartsBuffer 저장소 통합 — `MessageOperations.java:83,134` | 2-part SEND 80→56B, REPLY 56→40B, PUBLISH 64→40B; REQUEST 64→64B. 전체 mini에서 5% 개선 없음 | 기존 buffer 한 곳으로 2-part/overflow 분기 통합; one-shot·part identity 유지 | 해당 없음 | **no-go**. 실제 Received.reply builder는 별도 계약 값 내부 구현이며 이 후보의 적용 대상이 아님 |
| 위 두 후보 결합 | DD 전체 main 432→328B, DR 전체 약 160B 감소. 100-client 축소 진단에서는 DD·DR 하락, RR256 중앙값 +3.7% | native 호출·러너 코드 동일, 진단 classpath만 변경 | 해당 없음 | **no-go**. 다른 job과 겹친 축소 측정만으로 인과를 단정하지 않으며 채택 근거도 없음 |
| 대기자가 없는 ownerLock notify 제거 — `CompletionOwner.java:877` | `ownerLock.wait()`는 없으나 notify가 남아 있음. DR64/64K mini 처리율 중앙값 +1.1/+2.2% | 기존 registry/ownership lock은 유지하고 불필요한 알림만 제거 | 해당 없음 | **no-go**. 5% 미달 |
| Future.get으로 settlement 대기 통합 — `CompletionOwner.java:1196–1230` | notify 제거에 추가한 후보. DR64 처리율 −9.9%, DR64K +8.5%; 작은 요청 회귀. Signaller 할당 추가 | stage 실패/cancel은 stage에 남기고 interrupt 의미와 worker lane 유지 | 해당 없음 | **no-go**. 작은 요청 회귀; 이 변경은 production에 없음 |
| 기존 DONTWAIT critical downcall 자동 선택 — `Native.java:584,646` | DD64/64K 처리율 −3.2/−1.5%. 변경되지 않은 DR 경로에도 변동이 있어 그 상승을 효과로 사용하지 않음 | 동일 native 함수·flags·typed error; blocking에는 일반 downcall 유지 | 해당 없음 | **no-go** |
| DD 큰 메시지의 runtime wake lifecycle | 4096/64K 모두 Context poller 생성/파괴 1/1, PAIR 1쌍. 64K WRITABLE 9,911건에 native poller wait 203회 | Core completion에 따라 기존 단일 pump/drain/retry 유지 | 과거 control-pair-only 재사용 후보는 재시험하지 않음 | 반복 생성 병목 **재현 안 됨**. 타이머·wake·fairness 조정 없음 |
| FFM batch/native vector 직접 채택·metadata 조회 생략 | 39회 구성과 C 17회 차이를 아래에서 확인. staging rollback, vector lifetime 및 알 수 없는 payload size 처리가 남음 | 새 native bridge 또는 새로운 공동 vector 수명 규칙 없이 제거할 기존 API 경로를 찾지 못함 | §4의 타 언어 기각 구현을 이식하지 않음 | **미구현 no-go**. 효과를 추정하거나 검증됐다고 주장하지 않음 |
| public poller 우회, public Future/Message pool | public/runtime은 이미 같은 native drain과 retry를 사용. Public progress와 고유 stage 수명을 바꾸는 최적화는 제외 | 공통 async §4의 owner 이전·단일 consumer 유지 | Future/wrapper pool·spin은 명시적 기각 대상 | **제외**, 재시험 없음 |

## 변경과 검증 경계

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterSocket.java:56`: `NONE → fresh Received → adoptFrom` 분기를 없애고 기존 `routerRecvInto`를 모든 flags에 사용한다. NativeRouterReceiveSupport가 이미 소유한 no-data/하드 오류·multipart 수신 규칙을 사용한다.
- `bindings/java/src/test/java/systems/zlink/runtime/sockets/RouterReceiveStorageContractTest.java:18`: NONE/DONT_WAIT 각각 1/2/3/9 parts, RID/reply token, 받은 Message identity·내용, NO_DATA 시 이전 저장소 보존 및 직접 reply를 검증한다. NONE의 NO_DATA는 `recvTimeout(Duration.ZERO)`로 발생시키며 sleep/retry나 expectation 완화가 없다.
- **수정 전/후 규칙 수: 수신 저장소 선택 2개(NONE fresh/adopt, DONT_WAIT direct) → 1개(기존 caller-storage 경로 + 전달된 flags).** 새 상태·pool·timer·옵션·scheduler 없음.

두 대안은 기존 direct 경로로 통합하는 방법과 별도 blocking 수신을 유지하며 임시 객체를 재사용하는 방법이다. 후자는 재사용 상태와 수명 규칙을 추가하므로 선택하지 않았다. Prefix 실패와 3-part 이상 fallback은 기존 소유 모듈이 처리한다.

## 미니벤치와 할당

JDK 22.0.2+9, 고정 Core, inproc body+empty tail, 각 case 10,000 warmup + 30,000 측정, before/후보를 번갈아 3개 JVM씩 실행했다. `ThreadMXBean`으로 managed allocation, `nanoTime`으로 범위 시간을 측정했다. 아래 수신 구간은 blocking `router.recv(Received, NONE)` 호출 전체이며, RPC 시간에는 submit·reply·public completion poller·worker settlement가 포함된다. Public poller는 zero-timeout spin 대신 2초 bounded wait를 사용한다. 공식 러너 소스와 측정 loop는 수정하지 않았다.

첫 조사 구간은 다른 컴파일 job 때문에 load가 100을 넘었다. 그 시간 값은 성능 판정에서 제외하고, 이후 비교(`mini-quiet-*`, `mini-control-*`, `mini-receive-*`)를 별도로 보존했다. 일부 후속 비교도 다른 job과 겹쳤으므로 원자료의 load와 모든 run을 함께 보존한다.

| Blocking DR body | receive B/건 before→after | receive ns/건 before→after | RPC ns/건 before→after | submit B/건 before→after |
|---|---:|---:|---:|---:|
| 64 | 496.0→304.0 | 3,325.6→3,105.3 | 55,893.0→58,009.6 | 424.0→424.0 |
| 65536 | 496.0→304.0 | 11,694.1→8,021.9 | 164,553.1→117,819.9 | 424.0→424.0 |

64B 전체 RPC 중앙값은 수신 구간과 달리 악화했다. 64KiB 수신·RPC 시간은 실행 간 편차가 컸다. 따라서 **64B 수신 경계의 국소 효과와 전체 RPC 처리량은 분리**하며, 이 수정으로 공식 목표를 달성했다고 표현하지 않는다.

JFR는 allocation·monitor/park만 켰고 `ExecutionSample`/`NativeMethodSample`은 껐다. Sample weight 합은 220,780,264B, GC 5회다. `NativeMemorySegmentImpl` 36.87MB, `Pending` 5.63MB, `CompletableFuture` 2.62MB와 socket/settlement lambda가 관측됐다. 이는 startup·warmup·모든 mini 경로를 합친 **샘플 가중치**이며 요청당 정확한 byte 수가 아니다. `alloc-analysis.txt`, `alloc-classes.txt`에 class/stack별 근거가 있다.

- Scratch slice가 가장 큰 단일 binding allocation stack이었다(24.45MB sample weight, `submitPartsAttemptLocked:375`). 줄여도 이 mini에서 5% 시간 개선을 얻지 못했다.
- REQUEST는 고유 Pending/Future를 요청마다 하나 만든다. Completion 및 builder lambda도 남는다. Late completion·cancellation과 caller 관찰 가능성을 보존하기 위해 pool을 사용하지 않았다.
- 이 async submit/drain은 pass 1의 ThreadLocal scratch를 사용하며 Arena 생성은 초기화/성장 경계다. 별도 `trackNoWaitSend`의 confined Arena는 이 2-part async mini 경로가 아니다.
- Blocking ROUTER mini의 임시 Received allocation은 `Received$1.create`와 constructor/list stack으로 확인했다. 실제 multi server는 DONT_WAIT direct 경로이므로 이 임시 Received 비용을 실제 server 비용으로 간주하지 않는다.

## FFM 39회와 C 17회

최종 JAR의 counter에서도 **DD 20회, DR 39회로 동일**하다. Counter는 별도 classpath에만 있으며 최종 JAR에 hook이 없다. C 공개 API probe도 같은 고정 Core로 재실행해 DD 13회, DR 17회를 확인했다.

| C API 함수 | Java DR / 요청 | C DR / 요청 | 차이의 책임 |
|---|---:|---:|---|
| request_part / reply_part / router_recv_part | 2 / 2 / 2 | 2 / 2 / 2 | 2-part 공개 Core API; 언어 간 동일 |
| completion_recv / completion_close | 2 / 1 | 1 / 1 | Java는 NO_DATA 확인까지 drain, C probe는 준비된 completion 한 건 직접 수신 |
| poller_wait | 1 | 0 | Java public progress/settlement 경계; C probe에는 poller가 없음 |
| msg_init_size / msg_init | 2 / 8 | 1 / 3 | Java의 empty tail init_size, staging header 4개, reply target header 2개 |
| msg_copy / msg_move | 4 / 2 | 0 / 0 | 원본 실패 ownership 보존용 staging, 독립 reply Message로 이동 |
| msg_close | 6 | 4 | 반환된 독립 reply Message의 close 포함 |
| msg_data / msg_size | 3 / 4 | 1 / 0 | Java receive/reply payload cache 초기화; getter가 반복 downcall하는 것은 아님 |
| **합계** | **39** | **17** | poller/NO_DATA 2회를 제외해도 37 대 17 |

`has_more`는 recv의 out-parameter이므로 추가 downcall이 아니다. Part별 native 호출을 순수 FFM 비용이나 CPU 비율로 환산하지 않았다. `zlink_msg_copy`는 큰 payload의 refcount를 공유한다. C managed allocation 0은 native malloc 0을 의미하지 않는다.

## DD wake와 public/runtime 경로

실제 DD runner의 별도 native counter(100 clients, tcp, 3초)는 처리량 판정용이 아니다. Setup/teardown을 포함한다.

| DD body | submit | binding B/submit | runtime poller new/destroy | control PAIR | WRITABLE close | runtime poller wait |
|---|---:|---:|---:|---:|---:|---:|
| 4096 | 690,606 | 261.78 | 1 / 1 | 1쌍 | 5,187 | 1,392 |
| 65536 | 62,065 | 370.27 | 1 / 1 | 1쌍 | 9,911 | 203 |

64K wake당 평균 약 48.8건의 completion이 처리됐다. `completion_recv` 19,822회는 성공 9,911회와 drain 끝 확인을 포함한다. 추가 timeout-0 poll, control pair 재생성, POLLOUT 재시도나 fairness 변경으로 수치를 높이지 않았다. 남은 backpressure 및 wake 지연의 전체 인과를 이 counter만으로 확정하지 않는다.

- Public poller: `NativePoller.wait` → `InternalAccess.completionDrain:265` → `CompletionOwner.drain:593` → `drainWithNativeGate` → `drainLocked:636`.
- Public poller 미사용: `CompletionPump.run:170` → `CompletionOwner.drainFromRuntime:603` → 같은 `drainWithNativeGate/drainLocked`.
- 재제출은 같은 `retrySend/retryRequest → submitPartsAttempt`다. Public owner만 settlement 목록을 기다리며 native queue의 두 consumer를 만들지 않는다. Handover/idle/close는 기존 lifecycle contract를 5회 반복 검증했다.

## 공식 after와 별도 진단

지정 명령:

```sh
JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9 bash bindings/java/perf/multi/run_benchmarks.sh --pattern DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB --transports tcp --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1
```

실행 전 `ZLINK_CORE_SOURCE=local`, `source bindings/tools/local_core_runtime.sh`, Gradle workers 2를 적용했다.

- 공식 실행 load 기록: `2026-09-05T16:26:19.080162 OFFICIAL_AFTER gate load1=1.28; 16:26:19 up 22:42,  1 user,  load average: 1.28, 2.79, 16.99`. 결과 EXIT 1.
- Guard: `bindings/java/perf/multi/run_benchmarks.sh:851`. Shared build의 원본 `core/src/runtime/core/ctx_inproc_registry.cpp`가 checkout과 다르고 binary보다 새롭다. Core binary mtime 15:15:18, 원본 source mtime 16:01:54. Guard·Core·symlink를 수정하지 않았다.
- 진단은 최종 PerfMain을 직접 실행한 별도 실행이다. START gate, stop token, 100 clients, 5초 active duration, io threads=4, 원래 HWM·timeout·framing을 유지했다. 공식 shell의 freshness 승인과 report 생성은 통과하지 않았으므로 공식 결과의 대체 승인 자료가 아니다.
- 진단 case 시작 load: **0.83–3.00**, 20회 모두 ≤3. Case 사이에는 load 조건을 기다렸으며 active 측정 중 러너를 바꾸지 않았다.
- Before/C는 메인 트리의 15:23 paired 3-run report 8개다. 원본 사본은 `build/pass2/paired-before/`에 보존했다. 다음 After는 **공식 after가 아닌 진단 1-run**이다. Before와의 차이는 causal binding speedup으로 판정하지 않는다.

| Pattern | Before/C size 평균 | 진단 After/C size 평균 | 목표 | 공식 판정 |
|---|---:|---:|---:|---|
| DEALER_DEALER | 70.84% | 78.32% | 90% | BLOCKED |
| DEALER_ROUTER_REQREP | 54.70% | 63.83% | 70% | BLOCKED |
| ROUTER_ROUTER_REQREP | 58.53% | 67.36% | 70% | BLOCKED |
| PUBSUB | 102.86% | 125.97% | 90% | BLOCKED |

Ratio는 size별 비율의 산술평균이다. DD/PUBSUB는 msg/s, REQREP는 ops/s이며 latency 원자료의 ms 단위를 유지한다.

| Pattern | B | C 3-run | Java before 3-run | 진단 after 1-run | 변화 | After/C | latency before→after ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 998,014.2 | 757,008.2 | 774,173.2 | +2.3% | 77.6% | 0.507→2.413 |
| DEALER_DEALER | 256 | 947,243.0 | 690,986.0 | 764,432.4 | +10.6% | 80.7% | 0.563→0.583 |
| DEALER_DEALER | 1024 | 787,600.6 | 606,679.8 | 722,684.0 | +19.1% | 91.8% | 1.412→0.674 |
| DEALER_DEALER | 4096 | 300,354.6 | 198,630.8 | 309,650.8 | +55.9% | 103.1% | 59.037→0.989 |
| DEALER_DEALER | 65536 | 64,188.4 | 39,944.6 | 24,685.6 | -38.2% | 38.5% | 3.320→4.526 |
| DEALER_ROUTER_REQREP | 64 | 192,343.6 | 71,180.2 | 80,806.4 | +13.5% | 42.0% | 0.586→0.515 |
| DEALER_ROUTER_REQREP | 256 | 166,580.2 | 64,106.8 | 81,201.8 | +26.7% | 48.7% | 0.649→0.514 |
| DEALER_ROUTER_REQREP | 1024 | 167,976.2 | 75,220.8 | 70,213.8 | -6.7% | 41.8% | 0.552→0.588 |
| DEALER_ROUTER_REQREP | 4096 | 128,479.6 | 57,103.0 | 77,818.0 | +36.3% | 60.6% | 0.725→0.541 |
| DEALER_ROUTER_REQREP | 65536 | 22,497.6 | 24,477.4 | 28,353.4 | +15.8% | 126.0% | 1.765→1.479 |
| ROUTER_ROUTER_REQREP | 64 | 177,520.2 | 70,713.2 | 78,966.6 | +11.7% | 44.5% | 0.592→0.529 |
| ROUTER_ROUTER_REQREP | 256 | 128,647.2 | 40,294.8 | 75,908.0 | +88.4% | 59.0% | 1.054→0.550 |
| ROUTER_ROUTER_REQREP | 1024 | 125,093.4 | 72,841.6 | 69,272.2 | -4.9% | 55.4% | 0.574→0.602 |
| ROUTER_ROUTER_REQREP | 4096 | 104,437.8 | 71,327.0 | 56,043.6 | -21.4% | 53.7% | 0.589→0.744 |
| ROUTER_ROUTER_REQREP | 65536 | 20,786.0 | 19,741.0 | 25,831.8 | +30.9% | 124.3% | 2.195→1.675 |
| PUBSUB | 64 | 569,222.8 | 583,534.6 | 744,641.6 | +27.6% | 130.8% | 1555.341→1665.301 |
| PUBSUB | 256 | 659,390.2 | 668,045.6 | 800,308.2 | +19.8% | 121.4% | 1863.988→1987.251 |
| PUBSUB | 1024 | 728,521.8 | 709,529.0 | 850,009.2 | +19.8% | 116.7% | 1172.964→931.630 |
| PUBSUB | 4096 | 578,812.4 | 485,388.2 | 753,005.0 | +55.1% | 130.1% | 546.369→332.060 |
| PUBSUB | 65536 | 57,974.4 | 74,929.4 | 75,895.4 | +1.3% | 130.9% | 138.441→145.051 |

변경하지 않은 PUBSUB도 크게 달라졌다. 시각과 1-run/3-run 차이 및 호스트 실행 조건의 영향을 분리할 수 없으므로 이 표의 상승을 이번 수신 통합 효과로 인정하지 않는다. DD/DR/RR는 진단 평균조차 목표에 미달한다. 특히 DD64KiB는 39,944.6→24,685.6 msg/s(−38.2%), latency 3.320→4.526ms로 악화했다. 이 하락은 삭제하거나 좋은 재측정값으로 대체하지 않았으며, 변경된 blocking ROUTER 경로는 DD에서 사용되지 않는다. RR256의 불안정한 단일 값에 맞춰 러너나 callback 정책을 바꾸지 않았다.

## Gate

- `bindings/java/tests/run_tests.sh` 전체 **PASS**: unit 106개 중 105 PASS / 기존 disabled 1, integration 25 PASS, Netty 3 PASS, Kotlin 4 PASS. **137 PASS / disabled 1 / failure 0 / error 0**.
- 샘플 **7/7 PASS**: RequestReplyAsync, PairRecv, PubSubRecv, DealerRouterRecv, StreamRecv, StreamPacketCallback, MonitorRecv.
- 관련 contract **29개 ×5회 PASS**: unit 17 + integration 12. `:test`, `:integrationTest`에 `--rerun`과 명시적 필터 적용. 신규 수신 저장소, batch, ownership, lifecycle, routed multipart, pull completion, Received, backpressure, request wait token 포함.
- `git diff --check` **PASS**. 공개 contract/qualified export/module classfile **110개 byte-for-byte 동일**. 최종 JAR의 ProfileCounter 부재와 native binary 일치 확인.
- 기능 테스트에 남은 실패는 없다. 공식 benchmark guard EXIT 1만 남는다. 후보의 성능 회귀 자료는 삭제하지 않았다.

## 소유 계층·계약·교차언어·분류

- **소유 계층:** Java binding의 `NativeRouterSocket → NativeRouterReceiveSupport`. Core의 receive/admission/reconnect/wake 결정을 복제하지 않았다.
- **Spec:** `bindings/doc/spec/java/README.ko.md:676,690–705`의 caller-provided Received 및 no-wait false/typed failure/소유권. Completion 리뷰는 `bindings/doc/spec/async-execution-model.ko.md` §4의 단일 drain owner와 Java Pull completion을 기준으로 했다.
- **교차언어:** .NET `SocketKernel.ReceiveCore.cs:185–216`는 같은 ReceiveRouterParts에 flags를 전달한다. C++ `Runtime/Native/native_receive.hpp:51–57`도 같은 recv_router_part에 flags를 전달한다. Java만 blocking public recv에서 임시 Received를 만들고 다시 adopt하는 별도 진입 경로가 있었다.
- **변경 분류: B — 기존 binding allocation/중복 수신 경로 결함.** Framework runtime 변경이나 Core 보상 로직이 아니다.
- **새 spec gap 없음.** Provisional registry 서술과 기존 token-after-submit 코드의 drift는 이전 pass에서 기록한 상태 그대로이며 이 수정과 무관하다. Protected spec/doc 변경 없음.

## BLOCKERS

1. **공식 after 미완료:** 공유 Core source freshness guard. Core build/수정과 guard 완화가 금지되어 이 범위에서 복구하지 않았다. 공식 성능 승인을 요청하거나 통과로 표시하지 않는다.
2. **성능 목표 미달:** 실제 DONT_WAIT multi 경로의 Java/C 격차와 DD64KiB 진단 −38.2% 하락은 해결되지 않았다. 이 국소 blocking 수신 통합의 효과로 해당 변화들을 설명할 수 없다. 진단 1-run 수치는 표에 보존하되 개선의 인과와 공식 목표 달성을 주장하지 않는다.
3. 기능 gate blocker는 없다. Runtime 변경과 회귀 test는 미커밋 상태다.

## 실행 환경과 증거

- Core SHA-256: `543e1089430176bf861f9ef8b7974941e3d785dee8d93bb8d4a39d62e1d08538`. 조사 시작/끝 shared binary, 고정 probe/진단 binary 및 최종 JAR embedded Core 동일.
- Final JAR SHA-256: `1013df13d9b7bc5a145cab764f98d53e242e3374e4ca0e94bd058ae82973b4be`.
- JDK `/home/hep7hep7/.jdks/jdk-22.0.2+9`; `GRADLE_OPTS=-Dorg.gradle.workers.max=2`. 종료 `./gradlew --stop` PASS (`No Gradle daemons are running.`); `build/pass2/gradle-stop.log`에 보존.
- 요약: `/home/hep7hep7/project/zlink-work/c016/java-perf-pass2-summary.md`.
- 진행: `/home/hep7hep7/project/zlink-work/c016/java-perf-pass2-progress.md`.
- After 사본: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass2-after-diagnostic.txt` (**NON-OFFICIAL**).
- 공식 실패 사본: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass2-official-after-blocked.log`.
- 원자료: `bindings/java/build/pass2/`: `mini-*`, `builder-*`, `multi-mini-logs/`, `count-{before,final}.txt`, `c-count.txt`, `alloc-before.jfr`, `alloc-{analysis,classes}.txt`, `runtime-counter-logs/`, `full-gate.log`, `repeat-{1..5}.log`, `public-api-check.txt`, `after-diagnostic-logs/`, `freshness-preflight.json`.
- 증거 archive: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass2-evidence.zip`.
- **종료: EXIT:1 — 공식 after freshness blocker.**
