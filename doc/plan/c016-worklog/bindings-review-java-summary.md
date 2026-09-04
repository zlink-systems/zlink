# zlink Java 바인딩 Contract B 리뷰 요약

## 결론

- Core 0.17.0 Contract B의 DONTWAIT SEND/WRITABLE 계약을 Java 바인딩의 공개 API 형태 변경 없이 충족한다.
- 표준 DEALER_ROUTER tcp 1024B 성능은 `984.667 msg/s`에서 `789,010.333 msg/s`로 회복됐다.
- 전체 Java 테스트와 샘플, single perf 6/6, multi perf 24/24가 통과했다. 0 throughput, 정지, 미회수 대기자는 없었다.

## 계약 항목 판정

| 항목 | 판정 | 근거 |
|---|---|---|
| (a) DONTWAIT 단일 시도와 즉시 성공 | 적합(수정) | `CompletionOwner.java:82`에서 한 번만 submit하고 `OK/ID 0`이면 정적 완료 stage를 즉시 반환한다. Pending/Future/map/runtime owner를 만들지 않는다. |
| (b) payload 보유와 정확한 WRITABLE 재시도 | 적합(수정) | `CompletionOwner.java:102-123`에서 BACKPRESSURED+token일 때만 `zlink_msg_copy` 기반 snapshot을 보유한다. `CompletionOwner.java:1322-1343`에서 kind·completion ID·context token·RID가 모두 같은 WRITABLE만 처리하고, 재거절은 새 completion ID로 다시 arm한다. |
| (c) WRITABLE TERMINAL 전달 | 적합(수정) | `CompletionOwner.java:1328`, `NativeSubmitErrors.java:35`, `ZlinkException.java:132`에서 ENOENT는 NOT_FOUND, ECANCELED/ESHUTDOWN/ETERM은 TERMINATED typed 예외로 완료한다. |
| (d) ROUTER/STREAM route 없음 | 적합 | 초기 submit의 NOT_CONNECTED/EHOSTUNREACH를 즉시 예외로 전달하며 token을 등록하지 않는다. `DontWaitBackpressureContractTest.java:217`에서 메시지 보존까지 검증한다. |
| (e) completion queue 소유권과 level readiness | 적합(수정) | `CompletionOwner.java:550-596`에서 한 public/runtime owner가 REQUEST와 WRITABLE을 context별 O(1) dispatch하며 NO_DATA/BUSY까지 drain한다. runtime owner는 `CompletionOwner.java:749`에서 POLLCOMPLETION만 기다려 level POLLOUT spin을 피한다. |
| (f) close/term, 자원, 스레드 안전성 | 적합(수정) | `CompletionOwner.java:360-390`에서 close를 drain/native gate와 직렬화한다. `CompletionOwner.java:1416`에서 SEND/REQUEST 대기자를 typed 종료하고 retained message, completion record, poller/control socket을 한 번씩 해제한다. close/send stress와 HWM close 테스트가 통과했다. |
| (g) 오류 매핑·예외 타입 | 적합(수정) | NOT_FOUND/NOT_CONNECTED/BACKPRESSURED/TERMINATED 및 native errno를 `ZlinkSubmitException`/`ZlinkRequestException`에 보존한다. |
| (h) REQUEST/blocking/PUB 회귀 | 적합 | blocking·REQUEST·reply는 기존 의미를 유지한 native header staging 경로를 사용하고 PUB 경로는 변경하지 않았다. 전체 unit/integration/netty/kotlin/sample 및 perf smoke가 통과했다. |

## 발견 버그와 수정

| 파일:행 | 증상 | 수정 |
|---|---|---|
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:82` | 즉시 성공 SEND도 Pending, CompletableFuture, map entry와 payload snapshot을 만들고 runtime owner를 깨웠다. | 최초 DONTWAIT를 원본 메시지의 shared native header로 직접 시도하고 `OK/ID 0`은 정적 완료 stage로 반환했다. BACKPRESSURED일 때만 Pending과 snapshot을 만든다. |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:290` | 매 part마다 shared-copy `Message`와 Arena를 만들고 즉시 닫아 FFM scope close가 hot path를 지배했다. | 한 attempt Arena에 native `zlink_msg` header를 연속 배치하고 `zlink_msg_copy`로 payload를 공유했다. 64B 초과 payload는 refcount만 증가하며 byte copy하지 않는다. |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:749` | runtime owner가 level-true POLLOUT도 구독해 completion이 없을 때 poll/drain busy loop를 만들었다. | 내부 owner는 POLLCOMPLETION만 구독하고, 종료/소유권 전환은 control socket으로 깨운다. |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1328`; `NativeSubmitErrors.java:35`; `ZlinkException.java:132` | TERMINAL WRITABLE의 lifecycle errno가 NOT_ADMITTED로 뭉개져 typed 종료 의미를 잃었다. | ENOENT→NOT_FOUND, ECANCELED/ESHUTDOWN/ETERM→TERMINATED로 매핑해 future를 예외 완료한다. |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:360`, `:1416` | close와 token 등록 경계가 분리돼 대기 상태가 남을 수 있었고 close는 일반 IllegalStateException으로 완료했다. | drainLock/nativeCallGate 순서로 close/register를 직렬화하고 SEND/REQUEST별 TERMINATED+ESHUTDOWN 예외로 모든 대기자를 settle한다. |
| `bindings/java/perf/single/run_benchmarks.sh:279`; `bindings/java/perf/multi/run_benchmarks.sh:502` | report가 100개를 넘으면 `find|sort|head`의 SIGPIPE가 pipefail에 잡혀 성공 benchmark가 exit 141로 끝났다. | `head`를 입력을 끝까지 소비하는 `sed -n`으로 바꿨다. |
| `bindings/java/perf/multi/run_benchmarks.sh:62-111` | 필수 smoke 명령의 `CCU/DUR/SIZES/PATTERNS/TIMEOUT` 환경변수를 무시해 기본 대형 suite가 실행됐다. | 기존 PERF_* 우선순위를 유지하면서 요청된 호환 환경변수를 fallback으로 지원했다. |

## 성능 리뷰와 측정

동일 조건: DEALER_ROUTER, tcp, 1024B, duration 3, runs 1, part-count 2, JDK 22, local `core/build`.

| 상태 | Throughput (msg/s) | Bandwidth (MB/s) | Mean (ms) | P95 (ms) | P99 (ms) |
|---|---:|---:|---:|---:|---:|
| 포팅 후 수정 전(main) | 984.667 | 1.008 | 1.740 | 4.842 | 8.231 |
| 포팅 전 `70a9998998` + 동일 current Core | 314,733.333 | 322.287 | 0.146 | 0.342 | 1.882 |
| 수정 후 | 789,010.333 | 807.947 | 1.142 | 2.740 | 13.217 |

포팅 전 worktree wrapper는 공유한 Core 산출물의 source timestamp gate 때문에 실행을 거부했으므로, wrapper가 빌드한 동일 runner를 같은 인자와 환경으로 직접 실행했다. Core는 재빌드하거나 clean하지 않았다.

| 항목 | 판정 | 수정 | 측정 |
|---|---|---|---|
| 성공 hot path 할당/복사 | 결함 수정 | per-part Message/Arena, Pending/Future/map을 제거했다. attempt당 native header block 하나만 만들고 payload는 `zlink_msg_copy` refcount로 공유한다. | 984.667→789,010.333 msg/s(약 801.3배). |
| 거절 payload snapshot | 적합 | 최초 BACKPRESSURED가 확인된 시점에만 retained shared copy를 만든다. | 1024B 성공 경로 payload byte copy 없음. |
| poll/drain spin·sleep | 결함 수정 | POLLOUT 구독을 제거하고 POLLCOMPLETION/control wake의 무기한 poll을 사용한다. 고정 sleep 없음. | single/multi 모두 정지 없이 완료. |
| 완료 대기 자료구조 | 적합 | `ConcurrentHashMap<Long, Pending<?>>`의 context token 조회·삭제 O(1), Pending 내부 상태 전이 O(1). | contract/stress 5회 연속 green. |

## 스모크 결과

### 테스트와 샘플

| 실행 | 결과 |
|---|---|
| `bash bindings/java/tests/run_tests.sh` (JDK 22, local Core) | unit, integration, netty extension, Kotlin contract 모두 green |
| 위 스크립트의 samples | RequestReplyAsync, PairRecv, PubSubRecv, DealerRouterRecv, StreamRecv, StreamPacketCallback, MonitorRecv 7/7 green |
| Contract B + close/send concurrency 관련 테스트 | 5회 연속 green, sleep 없는 public API HWM 테스트 포함 |
| `:perf-multi:test` | green |
| `bash -n` single/multi runner, `git diff --check` | 통과 |
| mirror cmp | 적용 대상 mirror 없음 |

### perf single

1024B, duration 2, runs 1. 값은 throughput msg/s / mean latency ms.

| 패턴 | tcp | inproc |
|---|---:|---:|
| PAIR | 728,193.0 / 1.746 | 789,724.5 / 0.848 |
| DEALER_ROUTER | 737,749.5 / 1.718 | 657,896.5 / 0.110 |
| PUBSUB | 648,569.0 / 1.050 | 638,742.5 / 0.103 |

결과: `bindings/java/perf/results/single/report/perf_java_single_linux_20260904_214225_java-review-smoke-r2.txt`

### perf multi

CCU 8, duration 2, sizes 1024/65536, runs 1. 표 값은 throughput msg/s이며 24/24 모두 nonzero로 통과했다.

| 패턴·크기 | tcp | tls | ws | wss |
|---|---:|---:|---:|---:|
| DEALER_DEALER 1024 | 567,142.5 | 546,181.0 | 601,223.5 | 484,032.5 |
| DEALER_DEALER 65536 | 36,024.5 | 24,163.0 | 32,593.5 | 18,531.5 |
| DEALER_ROUTER_SENDSEND 1024 | 101,762.0 | 136,480.0 | 126,030.0 | 151,552.0 |
| DEALER_ROUTER_SENDSEND 65536 | 35,855.0 | 12,736.0 | 26,354.0 | 9,008.0 |
| PUBSUB 1024 | 833,880.5 | 762,751.0 | 666,369.5 | 690,307.0 |
| PUBSUB 65536 | 91,920.0 | 45,758.0 | 81,726.0 | 36,025.0 |

결과: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260904_214321_java-review-smoke.txt`

## 변경 파일

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeSubmitErrors.java`
- `bindings/java/src/main/java/systems/zlink/contracts/errors/Errors/ZlinkException.java`
- `bindings/java/src/test/java/systems/zlink/integration/contract/DontWaitBackpressureContractTest.java`
- `bindings/java/perf/single/run_benchmarks.sh`
- `bindings/java/perf/multi/run_benchmarks.sh`

## BLOCKERS

없음. JDK 22를 명시해 sample class-version 환경 문제도 발생하지 않았다.
