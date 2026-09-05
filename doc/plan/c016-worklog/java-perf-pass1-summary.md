# Java 바인딩 hot-path pass 1 결과

- 작업 트리: `/home/hep7hep7/project/zlink-wt-java-perf`, detached `1961c950eb32b8fd4f071cc81344a7dc2b23917a` 유지. commit/push/reset/checkout/stash 없음.
- 수정은 `bindings/java/**`의 runtime 4개 파일, 러너 shell 1개 파일, 새 contract test 1개 파일이다. Core·다른 binding·spec·repository doc 변경과 Core configure/build/clean 없음. 기존 Core build symlink 두 개 유지.
- 공식 after: multi/tcp, 100 clients, 64/256/1024/4096/65536B, 5초, 1 run, **20/20 complete**. 공식 after는 한 번 실행했다. 계측 run과 미니벤치 수치는 공식 처리량 판정에 합산하지 않는다.
- REQREP의 socket별 worker 대기를 poller 한 번의 준비 이벤트 처리 뒤로 옮겼다. DD 작은 메시지와 REQREP가 개선됐으나 **DD 4096B −42.4%, 65536B −39.4% 하락**이 남는다. DD transport는 최소 70%에도 미달하며 이 pass로 성능 판정을 닫지 않는다.
- 공개 contract와 module의 classfile 108개는 before 설치 JAR와 byte-for-byte 동일하다. public API signature·message ownership·typed error·completion lane·러너 scheduler/drain/fairness는 유지했다.

## 원인과 가이드 판정

아래 line은 before 원인을 먼저 적었다. `CompletionOwner`의 before 소스는 HEAD 및 프로파일의 `CompletionOwner.before`로 재확인할 수 있다.

| 비용 위치 | 확인 근거 | §2 판정·변경 |
|---|---|---|
| Binding public poller의 socket별 settlement 대기 | `runtime/eventing/NativePoller.java` before 260–262 → `CompletionOwner.java` before 564–575,1542–1552. DR64 JFR에서 `awaitSettlement` 80,755회, 총 2.763초. 5초 active run의 약 55%에 해당한다 | §2.3: 모든 ready socket을 먼저 drain하고 기존 worker settlement를 기다린다. 현재 `NativePoller.java:247`, `CompletionOwner.java:596`. runtime/public drain owner, NO_DATA까지 drain, worker lane FIFO, wait 반환 전 stage 완료 의미 유지 |
| Submit와 drain마다 confined Arena·native header 할당 | `CompletionOwner.java` before 334–345,618–620. JFR에서 `NativeMemorySegmentImpl`, `SegmentFactories$1`, `ArenaImpl`, `ConfinedSession` 할당이 반복된다. uninstrumented mini의 DD 제출 528–556B/건 | §2.1·§2.4: thread-local native scratch로 target/id/part header/completion storage 재사용. 현재 `CompletionOwner.java:58–83,357,639`. public Message/Future/CompletionStage pool 추가 없음 |
| 성공 시에도 validation 문자열 생성 | `CompletionOwner.java` before 455. DD64KiB allocation sample에서 이 위치의 byte array 약 3.24MB | §2.1: null을 발견했을 때만 동일한 `parts[i]` 문자열을 만든다. 예외 type/message 유지; 현재 479–488 |
| 즉시 SEND의 pending/Future 등록 | before `submitSend`는 이미 OK/ID0에 shared completed stage를 반환하고 토큰이 나온 뒤에만 pending 등록 | §2.1: 정상. .NET pass1의 eager registration 결함은 Java에서 재현되지 않아 변경하지 않음 |
| 거절 SEND의 snapshot·registry·재제출 | 기존 drainLock이 submit와 drain의 등록 창을 보호하고 context token map으로 조회한다. source는 native `zlink_msg_copy`로 보존 | §2.2: 기존 책임 유지. retry 수·timeout·budget·POLLOUT 정책 변경 없음. Core가 실패 part도 소비하므로 제출 전 공유 header snapshot을 제거하지 않음 |
| DD64KiB 큰 본문 | `PerfMultiDealerDealer.java:228`은 native Message를 할당하고 header만 쓴다. binding staging은 `zlink_msg_copy`로 payload를 공유한다. 64B/65536B mini에서 FFM 호출 수가 동일하다 | managed↔native 대형 payload 왕복 복사 가설은 해당 DD 경로에서 기각. DD64KiB의 낮은 처리량은 이번 scratch 개선으로 해소되지 않음 |
| DD64KiB backpressure 뒤 owner 생성·wake | JFR에서 `startRuntimeOwner`, `runtimeEventLoop`, `ControlWake.create` 할당이 반복된다. after sample에는 VirtualThread 약 1.55MB, ControlWake endpoint String 약 1.31MB가 관측됨 | §2.3 잔여 후보. 후속 실제 러너 native 계수·중첩 CPU 계측으로 아래와 같이 binding의 private control socket 생성 비용을 확인했다. 재사용 후보는 처리량 하락으로 기각했다 |
| REQREP server reply | 받은 native Message를 그대로 reply하며 payload byte array 왕복이 없다. client template의 `Message.from`은 공개 deep-copy 계약대로 S bytes를 복사한다 | §2.4: direct reply 유지. template copy를 shared copy로 바꾸거나 source mutation 수명을 바꾸지 않음 |

Binding/러너 구분: **DR64의 고정 지연은 binding settlement 경계로 확정**했다. DD64는 binding native staging의 반복 할당을 확인했다. DD64KiB는 payload-copy가 아니라 **binding control socket 생성이 제출 CPU를 지배**했다. 다만 이 비용만 제거한 후보는 다른 대기 증가와 함께 처리량이 낮아져 기각했다. 공식 DD regression의 인과는 아직 확정하지 않는다.

## 변경과 대안

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`: native scratch, 성공 validation 문자열 제거, per-socket `DrainBatch` 제거 및 wait 작업 전달.
- `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativePoller.java`: 준비된 socket 전체를 drain한 뒤 settlement 수행. completion 이벤트가 없으면 collection도 만들지 않는다. 오류 경로에서도 이미 drain한 settlement는 finally에서 처리한다.
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/InternalAccess.java`, `runtime/sockets/NativeSocketBase.java`: non-exported bridge의 drain 인자를 내부 settlement list로 연결. exported contract 변경 없음.
- `bindings/java/src/test/java/systems/zlink/runtime/sockets/CompletionBatchContractTest.java`: 한 socket의 busy lane을 다른 socket 완료가 해제하는 회귀, part count 1/5/2/9/1 및 request→SEND 전환의 scratch/ID/ownership 검증. sleep 없는 테스트다.
- **러너 별도 버그** `bindings/java/perf/multi/run_benchmarks.sh:851`: fresh worktree의 소스 mtime 때문에 지정된 shared build를 거부했다. CMake build의 원래 source tree와 현재 include/src가 동일하고 원래 source가 binary보다 오래된 경우에만 통과시킨다. 내용이 다른 소스와 오래된 build의 거부는 유지한다. 처리량·측정 loop·scheduler/drain/fairness 변경은 없다.

대안: public poller에서 future를 직접 complete하면 worker 왕복을 더 줄일 수 있지만 continuation 실행 thread 경계를 바꾸므로 사용하지 않았다. 기존 bounded context lane을 그대로 두고 대기 위치만 옮겼다. 각 socket에서 만든 `DrainBatch`를 제거하고 poller의 한 list로 모아 새 operation 상태·timer·retry 정책 없이 수정했다.

Native scratch는 호출마다 Arena를 여닫는 대안과 비교했다. 기존 SocketCore send scratch는 option/payload 경로도 사용하고 concurrent blocking request와 completion drain의 독립 수명을 보장하지 않으므로 공유하지 않았다. 현재 scratch는 Java thread 하나에 속한 내부 native header만 보관하고 automatic arena로 회수한다. Core에 넘긴 header는 소비되며 pending payload는 이 scratch를 참조하지 않는다. 새 public wrapper pool, Future pool, payload pool, 설정값, in-flight cap은 없다.

## 프로파일과 비용

JDK 22.0.2+9 JFR를 사용했다. 기본 profile의 sampling은 native blocking poller에 `EINTR=4`를 일으켜 typed `INTERNAL_ERROR`로 반환됐다. 이 실패 run은 폐기했으며 public error contract를 바꾸거나 재시도를 넣지 않았다. 실제 multi run은 `jdk.ExecutionSample`/`jdk.NativeMethodSample`을 끄고 allocation·monitor wait·park·GC를 수집해 4/4 complete를 얻었다. CPU hot method는 동일 public API의 inproc mini를 기본 JFR profile로 실행해 수집했다. `perf`, `strace`, async-profiler는 사용하지 않았다.

CPU mini의 sample은 native poller wait 5, plain recv 3, requestPart 3, routerRecvPart 3, replyPart 2 등이었다. 표본이 작고 실제 multi CPU 점유율 자료가 아니므로 이 개수로 비용 비율을 추정하지 않는다. 실제 multi의 wait/GC/alloc 자료와 코드 경계를 함께 사용했다.

### 정확한 managed allocation과 FFM 호출 계수

같은 Core로 inproc, 2 parts(body + empty tail), 5,000회 warmup 뒤 20,000회를 처리했다. DD는 async submit→recv, DR은 request→direct reply→public completion poller다. `ThreadMXBean`으로 main 및 전체 Java thread의 할당을 읽었다. 별도 계수 run에서 각 NativeSymbols downcall MethodHandle 앞에 thread-local counter를 합성했다. 계수 code는 build 아래 별도 classpath에만 두었고 최종 runtime에 남기지 않았다. 아래 allocation은 counter를 넣지 않은 run이다. submitB는 public builder 생성부터 native submit 반환까지이며 body/tail 생성·수신·worker 실행은 제외한다.

| Mini 경로 | main B/건 before→after | 전체 thread B/건 before→after | submit B/건 before→after | FFM/건 before→after | GC before→after |
|---|---:|---:|---:|---:|---:|
| DD 64B | 719.5→444.0 | 719.5→444.0 | 555.7→281.5 | 20→20 | 1→1 |
| DD 65536B | 688.3→432.3 | 688.3→432.3 | 528.0→272.0 | 20→20 | 1→0 |
| DR 64B | 2435.4→1780.2 | 2467.5→1812.3 | 680.0→424.0 | 39→39 | 1→1 |
| DR 65536B | 2392.3→1712.2 | 2424.3→1744.3 | 680.0→424.0 | 39→39 | 1→1 |
| C DD 64/65536B | managed 0 | managed 0 | managed 0 | native C API 13 | Java GC 없음 |
| C DR 64/65536B | managed 0 | managed 0 | managed 0 | native C API 17 | Java GC 없음 |

C mini는 Java build 디렉터리에서 공개 `zlink.h`와 지정 `.so`로만 컴파일했다. C의 managed allocation 0은 native malloc 0을 뜻하지 않는다. Java DR 39회에는 completion queue의 NO_DATA 확인 1회와 public poller wait 1회가 포함된다. C mini는 reply가 준비된 상태에서 completion을 직접 받으므로 이 2회가 없다. 해당 2회를 제외해도 Java 37회/C 17회로 차이가 남지만 native 호출 수를 CPU 비율로 해석하지 않는다. native heap allocation byte는 계측하지 않았다.

Payload copy(S=body size, transport 내부 복사 제외): 실제 Java DD는 0B/메시지의 body copy와 metric header write만 수행한다. 실제 C DD runner는 retained vector→native Message로 S bytes를 memcpy한다(`bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:168–170`). Java DR client는 공개 template deep copy로 S bytes를 native→native 복사한다. Java binding의 send/request staging은 2개의 64-byte native header를 공유 copy하고, 64B/64KiB body를 다시 복사하지 않는다. 두 mini는 template copy 없이 새 native body를 할당하므로 C/Java 모두 body copy 0이다. C mini에는 29B header store가 있고 Java mini에는 metric header write가 없다; 실제 러너의 header 작업과 혼동하지 않는다.

### 실제 multi JFR

아래 sample B/완료는 client 프로세스의 allocation sample weight 합계를 해당 run의 `throughput × 5s`로 나눈 **추정치**다. startup/shutdown 및 backpressure 재제출을 포함하고 native malloc을 제외한다. 따라서 mini의 정확한 B/건과 동일한 경계가 아니다. GC도 프로세스 전체 값이며 처리 건수가 달라 그대로 효율 비율로 읽으면 안 된다.

| 실제 경로 | sample B/완료 before→after(약) | client GC before→after | Java monitor settlement wait before→after | 계층 판정 |
|---|---:|---:|---:|---|
| DR 64B | 2177→1137 | 4→11 | 80,755회/2.763s→17,258회/1.029s; 완료당 34.0→2.3µs | binding worker 대기 병목 확인 |
| DD 64B | 605→356 | 28→25 | monitor wait 0→0 | binding staging 반복 할당 확인 |
| DD 65536B | 1705→1414 | 3→3 | monitor wait 0→0 | 후속 계측에서 binding control socket 생성 CPU 확인 |

JFR의 JDK worker park 합계는 여러 idle worker의 시간이 겹친 값이다(DD64KiB before 약 88.7s, after 약 93.3s). 이를 main thread의 lock 대기나 wall-clock 지연으로 제시하지 않는다. 10ms 이상 Java monitor contention은 DR에서 0회였으며, `drainLock` 경쟁이 이 REQREP 러너를 직렬화했다는 증거는 없다. 요청 제출은 DONTWAIT이며 public poller 소유이므로 이 경로에 runtime owner thread 생성도 없다.

### DD64KiB 실제 러너의 native lifecycle 계측과 기각 후보

JFR만으로 남은 비용을 단정하지 않고, 별도 진단 JAR의 native downcall을 main/owner/completion thread별로 계수했다. 100 clients, 3초, 같은 DD Java 러너의 코드를 사용했고 native 함수·반환값·scheduler·drain·fairness는 바꾸지 않았다. 이런 계측은 처리량 판정용이 아니며 공식 after는 그대로다.

| DD client 경로 | 제출 수 | binding B/submit | main FFM/submit | owner FFM/submit | owner poller 생성/파괴 | control pair 생성 |
|---|---:|---:|---:|---:|---:|---:|
| 64B | 2,277,437 | 176.61 | 11.001 | 0 | 0 | 0 |
| 65536B | 28,888 | 564.64 | 13.034 | 3.455 | 4,753/4,753 | 4,753쌍(9,506 socket) |

setup/teardown 포함 계수이며 64KiB는 약 **6.08번 제출마다 private poller/control pair lifecycle**이 한 번 발생했다. 별도 C 직접 send 경로는 body init/data, tail init, part send 2회, header close 2회로 정상 admission 7회이고 runtime owner/control pair 생성이 없다. 앞의 C mini 13회는 수신도 포함한 값이므로 혼동하지 않는다.

추가 중첩 `ThreadMXBean` CPU·`nanoTime` 계측(64KiB, 27,122 submits)에서 러너 `sendOneActive`의 CPU 합은 2.036s, 그 안의 binding `submitSend`는 1.913s(**94.0%**), 그 안의 `ControlWake.create`는 1.548s(binding CPU의 **80.9%**, 러너 제출 CPU의 **76.1%**)였다. wall 합은 각각 2.945s / 2.816s / 2.433s다. 중첩 scope이므로 합산하면 안 된다. body/tail 생성과 metric header write를 포함한 러너 비용보다 binding의 내부 control socket 생성이 지배한다. 측정 hook의 시간도 일부 포함되므로 비계측 ns/op로 제시하지 않는다.

이 근거로 `finishRuntimeOwner()`의 idle `closeControlWake()` 2줄을 제거하는 후보를 독립 시험했다. worker/poller는 기존처럼 idle에 종료하고 control pair만 socket close까지 재사용하는 대안이었다. Control pair 생성은 4,445→100쌍, create CPU는 1.548s→0.098s로 줄었으나 같은 계측의 DD64KiB 처리량은 **9,040.7→7,227.7 msg/s(−20.1%)**로 낮아졌다. owner poller 생성은 6,901회로 늘었으며, 다른 진행 대기를 해소하지 못했다. 단독 후보는 가이드에 따라 **기각하고 코드에서 제거**했다. 새로운 timeout/worker 설정이나 public API를 넣어 보상하지 않았다.

새 `repeatedBackpressureSurvivesIdleAndPublicOwnerHandover` 테스트는 runtime backpressure→public owner 전환→해제를 세 번 반복한 뒤 socket/context를 닫는 public lifecycle 검증으로 남겼다. 후보를 되돌린 최종 runtime은 공식 12:13 after의 구현과 같다. 새 lifecycle 테스트까지 포함해 최종 전체 gate와 관련 contract 5회를 다시 실행했다.

증거: `runtime-counter-logs/DD-{64,65536}-{client,server}.log`, `runtime-timing-logs/DD-65536-client.log`, `wake-timing-logs/DD-65536-{client,server}.log`, `runtime-counter-summary.txt`(모두 pass1-profile 아래). instrumentation은 별도 build 디렉터리 JAR에만 존재하고 최종 src/main 및 정규 perf JAR에는 없다.

## 공식 before/after

| Pattern | Before/C 평균 | After/C 평균 | 목표 | Latency before/C 평균 | After/C 평균 | 판정 |
|---|---:|---:|---:|---:|---:|---|
| DEALER_DEALER | 50.77% | 54.66% | 90% | 0.30x | 0.93x | 목표 미달 |
| DEALER_ROUTER_REQREP | 15.12% | 52.63% | 70% | 2.32x | 0.58x | 목표 미달 |
| ROUTER_ROUTER_REQREP | 24.02% | 72.48% | 70% | 2.09x | 0.57x | 목표 충족 후보; pass 2 검토 전 |
| PUBSUB | 81.05% | 88.43% | 90% | 1.04x | 0.92x | 목표 미달 |

| Pattern | B | C msg/s·ops/s | Java before | Java after | 변화 | After/C | Latency before ms | After ms | After/C |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,043,822.0 | 583,561.8 | 859,018.4 | +47.2% | 82.3% | 0.295 | 1.294 | 4.069x |
| DEALER_DEALER | 256 | 1,007,628.4 | 561,919.0 | 774,895.6 | +37.9% | 76.9% | 0.203 | 0.414 | 0.218x |
| DEALER_DEALER | 1024 | 903,580.6 | 559,639.6 | 608,627.6 | +8.8% | 67.4% | 0.615 | 0.469 | 0.343x |
| DEALER_DEALER | 4096 | 365,304.0 | 227,142.4 | 130,893.0 | -42.4% | 35.8% | 0.996 | 0.425 | 0.001x |
| DEALER_DEALER | 65536 | 77,680.2 | 14,012.0 | 8,484.8 | -39.4% | 10.9% | 0.697 | 0.589 | 0.029x |
| DEALER_ROUTER_REQREP | 64 | 192,764.0 | 9,589.8 | 83,562.2 | +771.4% | 43.3% | 3.699 | 0.500 | 0.492x |
| DEALER_ROUTER_REQREP | 256 | 168,158.8 | 14,794.6 | 75,020.2 | +407.1% | 44.6% | 2.438 | 0.556 | 0.609x |
| DEALER_ROUTER_REQREP | 1024 | 174,445.2 | 15,007.0 | 61,521.4 | +310.0% | 35.3% | 2.447 | 0.676 | 0.642x |
| DEALER_ROUTER_REQREP | 4096 | 141,283.0 | 14,916.0 | 57,121.6 | +283.0% | 40.4% | 2.447 | 0.727 | 0.388x |
| DEALER_ROUTER_REQREP | 65536 | 22,592.4 | 9,637.4 | 22,477.4 | +133.2% | 99.5% | 4.007 | 1.873 | 0.765x |
| ROUTER_ROUTER_REQREP | 64 | 141,509.0 | 14,116.0 | 80,535.0 | +470.5% | 56.9% | 2.575 | 0.524 | 0.545x |
| ROUTER_ROUTER_REQREP | 256 | 132,159.6 | 14,304.6 | 67,803.6 | +374.0% | 51.3% | 2.493 | 0.618 | 0.651x |
| ROUTER_ROUTER_REQREP | 1024 | 123,511.2 | 14,119.6 | 63,698.6 | +351.1% | 51.6% | 2.541 | 0.656 | 0.680x |
| ROUTER_ROUTER_REQREP | 4096 | 109,366.2 | 14,520.8 | 53,725.4 | +270.0% | 49.1% | 2.524 | 0.779 | 0.520x |
| ROUTER_ROUTER_REQREP | 65536 | 13,471.0 | 10,047.2 | 20,677.6 | +105.8% | 153.5% | 3.809 | 2.105 | 0.454x |
| PUBSUB | 64 | 618,285.4 | 458,085.6 | 565,977.6 | +23.6% | 91.5% | 1414.136 | 1555.019 | 1.074x |
| PUBSUB | 256 | 749,648.4 | 520,609.8 | 560,947.4 | +7.7% | 74.8% | 1984.653 | 1831.994 | 0.984x |
| PUBSUB | 1024 | 792,925.2 | 591,762.0 | 620,054.8 | +4.8% | 78.2% | 1349.404 | 1244.798 | 1.013x |
| PUBSUB | 4096 | 681,708.6 | 509,765.8 | 536,044.8 | +5.2% | 78.6% | 538.376 | 430.823 | 0.983x |
| PUBSUB | 65536 | 62,915.8 | 70,655.0 | 74,846.4 | +5.9% | 119.0% | 192.860 | 121.206 | 0.521x |

비율은 size별 ratio의 산술평균이다. raw RESULT latency는 이미 ms이며 단위 변환 없이 사용했다. DD64B latency는 C의 4.069배인 개별 outlier다. one-way queue latency와 REQREP latency는 구분하여 기록한다. DD4096B·65536B의 공식 하락은 지우거나 좋은 재측정값으로 대체하지 않았다. JFR 진단의 DD64KiB는 12.73k→12.55k msg/s였지만 공식 after는 8.48k이므로 하락의 인과를 scratch 변경 하나로 확정하지 않는다.

### Artifact와 실행 조건

- Core: `core/build/lib/libzlink.so.0.17.0`, SHA-256 `93ad7f1161156d34aecf5550f25d1d8630d18cb657eee24eecc135e578050918`. 작업 종료 전에도 동일했다.
- Java before 메인 트리 설치 JAR의 embedded `native/linux-x86_64/libzlink.so` hash가 위 값과 일치했다. 초기 비교 시 main/Core와 worktree/Core의 include/src 내용도 동일했다. 12:33 이후에는 main의 `socket_base_dispatch.cpp`가 외부 작업으로 바뀌어 추가 진단 shell의 mtime/content guard가 정상적으로 거부했다. 지정 `.so` hash는 그대로였고 추가 native 계측만 기존 Java 실행 파일과 같은 START/stop-token 제어로 직접 수행했다. 추가 진단을 위해 Core guard를 완화하거나 Core 파일을 바꾸지 않았다. 사용자 제공 C paired baseline을 사용했고 C 전체 benchmark나 Core rebuild는 하지 않았다.
- `JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9`, `GRADLE_OPTS=-Dorg.gradle.workers.max=2`, 매 실행 전 `export ZLINK_CORE_SOURCE=local; source bindings/tools/local_core_runtime.sh`. 기본 공식 JVM 옵션 유지, JFR·counter·Xmx override 없음.
- 공식 after 시작: 2026-09-05 12:13:48 KST, load 0.23 / 0.72 / 1.80. .NET pass2의 AFTER_END 11:55:52 및 EXIT:0 11:57:40과 dotnet process 없음 확인 뒤 실행했다. 1분 load ≤3 충족.
- 명령: `JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9 bash bindings/java/perf/multi/run_benchmarks.sh --pattern DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB --transports tcp --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1`.

## Gate

- `bindings/java/tests/run_tests.sh`: **PASS**. JUnit 135개 중 134 PASS, 기존 disabled 1, failure/error 0. 구성: :test 103(1 disabled), :integrationTest 25, Netty 3, Kotlin 4.
- 샘플 **7/7 PASS**: RequestReplyAsync, PairRecv, PubSubRecv, DealerRouterRecv, StreamRecv, StreamPacketCallback, MonitorRecv.
- 관련 contract **29개 × 5회 PASS**: CompletionBatch 3, CompletionOwnership 3, PullCompletion 3, RoutedMultipartAdmission 2, ContextCompletionDispatcher 6, DontWaitBackpressure 4, RequestWaitToken 8. `--rerun`으로 각 test task를 실제 재실행했다.
- 신규 batch 회귀의 old-code 검증: HEAD의 runtime 4개 class를 별도 classpath로 컴파일한 before는 TimeoutException, 현재 코드는 PASS. 두 실행에서 같은 테스트와 assertion을 사용했다.
- public contract/module classfile 108개 before/after 동일. `git diff --check` PASS.
- Gradle 종료: `./gradlew --stop` PASS (`No Gradle daemons are running.`)..

## 소유권·spec·교차언어·분류

- 소유 계층: Java binding의 CompletionOwner 및 public Poller. Core routing/retry/errno 결정과 Framework runtime은 변경하지 않았다.
- Spec: `core/doc/spec/core/socket/README.ko.md:921` 입력 소비와 보존, `:1074` reply admission/token, `:1105` Completion pull, `:1157` NO_DATA drain·single consumer. `bindings/doc/spec/java/README.ko.md:1001` public poller가 live stage/detached state 처리를 마친 progress event라는 계약 유지.
- 교차언어: .NET은 `TaskCompletionSource(RunContinuationsAsynchronously)`의 결과를 drain에서 정하고 worker continuation은 따로 실행하므로 Java의 socket별 `awaitSettlement`와 같은 왕복을 하지 않는다. C는 completion record를 직접 처리한다. Java의 worker lane을 보존해야 하는 구조 차이 때문에 batch settlement만 Java에서 바꿨다. .NET/C++ pass1·2의 native header staging 개선과 같은 비용 종류를 Java에서도 확인했다.
- 변경 분류: **B 기존 binding 성능 결함**. 기존 contract 안에서 storage·대기 위치만 변경. 러너 mtime 오탐 수정도 B이며 성능 개선과 별도 항목이다.
- 새 spec gap: **없음**. 기존 D-B102(inproc peer close CLOSED 부재) 때문에 `MonitorConnectionIdentityContractTest.java:47`의 1개 disabled test는 그대로다. Java spec의 기존 0.16.0/provisional 등록 서술과 0.17.0 구현 간 문서 drift는 이 pass에서 수정하지 않았다.

## BLOCKERS와 다음 검토 범위

- 기능 gate blocker 없음. 성능은 **DD 54.66%(<최소70%, 목표90%), DR 52.63%(<70%), PUBSUB 88.43%(<90%)**로 미달이다. RR 72.48%는 평균 목표 충족 후보이며 4096B의 개별 비율은 49.1%다. pass2 검토 전이다.
- DD4096B −42.4%, DD65536B −39.4%의 공식 regression이 남아 있다. 이 변경을 모든 pattern의 성능 승인·commit 가능 상태로 표시하지 않는다. 원인별 기여율을 추가로 분리하기 전에는 통과 판정 불가다.
- DD64KiB 제출 CPU는 binding control socket 생성이 지배함을 계측했으나, 이를 단독 제거한 후보도 처리량이 20% 하락했다. native owner/poller/wake 전체의 진행 구조와 backpressure를 함께 검토해야 하며 그 설계는 이번 pass에서 확정하지 않았다. idle timeout 증가, polling spin, 별도 completion consumer, public pool 또는 fairness 변경으로 우회하지 않았다.
- 운영상 잔여 조건: 현재 main Core source가 고정 binary 이후 외부 작업으로 바뀌어 추가 shell benchmark는 freshness guard에서 거부된다. 공식 after와 gate는 위 고정 hash로 완료했으며 Core 재빌드는 이 작업 범위 밖이다.
- 제약: 실제 multi의 CPU sampling은 EINTR 때문에 사용하지 못했고 allocation/lock JFR와 CPU mini로 대체했다. native malloc bytes·C lock profiler 값은 미측정이며 0으로 주장하지 않는다.

## Report와 증거 경로

- 공식 after 원본: `/home/hep7hep7/project/zlink-wt-java-perf/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260905_121358.txt`
- **공식 after 복사본**: `/home/hep7hep7/project/zlink-work/c016/reports/perf_java_multi_linux_20260905_121358.txt`
- JFR before/after report: `reports/perf_java_multi_linux_20260905_115917.txt`, `reports/perf_java_multi_linux_20260905_121028.txt`(모두 위 c016 아래; 둘 다 4/4 complete이며 공식 처리량과 분리).
- 사용자 제공 Java before: `perf_java_multi_linux_20260905_{114527,114633,114742,114848}.txt`; C baseline: `perf_c_multi_linux_20260905_{114449,114559,114705,114814}_p1java.txt`. 모두 c016/reports에 사본 보존.
- JFR/mini/계수/회귀/gate 원본: `/home/hep7hep7/project/zlink-wt-java-perf/bindings/java/build/pass1-profile/`. 주요 파일은 `jfr-before-analysis.txt`, `jfr-before-server-analysis.txt`, `jfr-after-analysis.txt`, `mini-cpu-before.jfr`, `mini-before.txt`, `mini-after.txt`, `count-before.txt`, `count-after.txt`, `c-mini.txt`, `regression-before.txt`, `regression-after.txt`, `final-gate.log`, `final-gate-counts.json`, `final-repeat-{1..5}.log`, `public-api-check.txt`.
- 진행 로그: `/home/hep7hep7/project/zlink-work/c016/java-perf-pass1-progress.md`.

- 계측 증거 archive: `/home/hep7hep7/project/zlink-work/c016/reports/java-perf-pass1-evidence.zip`(JFR, gate/계수 log, 미니벤치·진단 source; 정규 배포 JAR 제외).
