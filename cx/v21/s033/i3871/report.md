# #3871 Java request-serial 왕복 지연 진단

## 측정

`flock -o /root/jobs/fx/cx/measure.lock` 안에서 같은 실행 세션으로 1024/4096 바이트 각각 Binding·Framework를 3회 교차 측정했다. 셀마다 warmup 30초, active 5초. condition B affinity는 source 0–7, target 8–15이며, 매 셀 completed=received였다. p50 수집을 위해 벤치 클라이언트에 임시 출력 필드를 넣어 빌드했고 측정 뒤 원본을 복원했다. 활성 구간의 `/proc/<pid>/task/<tid>/schedstat`·`status` 차이는 before/after TID union으로 합산했다. 스냅샷 간격은 5.096–5.129초였다.

| payload | 구현 | msg/s | RTT p50 / p99 (µs) | RTT mean (µs) | target stamp→handler mean (µs) | RTT 잔여 mean (µs) |
|---:|---|---:|---:|---:|---:|---:|
| 1024 | Binding | 14,792 | 65.34 / 100.17 | 67.48 | 31.99 | 35.49 |
| 1024 | Framework | 8,668 | 110.63 / 184.92 | 115.20 | 54.97 | 59.95 |
| 4096 | Binding | 13,700 | 71.06 / 105.89 | 72.87 | 34.66 | 38.21 |
| 4096 | Framework | 8,428 | 113.90 / 187.18 | 118.49 | 57.01 | 61.48 |

각 값은 세 round의 중앙값이다. RTT는 Binding 대비 Framework에서 1024 기준 mean +47.72µs, p50 +45.29µs; 4096 기준 mean +45.62µs, p50 +42.84µs다. Target의 payload stamp→handler 구간은 각각 +22.97µs, +22.35µs 길다. RTT에서 이 target 구간을 뺀 잔여는 각각 +24.46µs, +23.27µs 길다. 잔여는 한 단계가 아니라 source 준비·송신, reply 경로, Framework completion을 함께 포함한다.

요청별 프로세스 CPU와 context switch는 TID union 합계의 round 중앙값이다. 스레드별 CPU는 `summary.jsonl`에 TID, 이름, affinity별로 모두 남겼고, 표에는 주요 활성 스레드를 적었다.

| payload | 구현 | source CPU µs/req; context switch/req | target CPU µs/req; context switch/req | 주요 thread CPU µs/req (source / target) |
|---:|---|---|---|---|
| 1024 | Binding | 31.03; 2.007 | 39.76; 3.008 | source `ZLINKbg/IO/2` 13.22, `/0` 13.15 / target `ZLINKbg/IO/1` 13.33, `/0` 13.14, `bench-raw-reque` 9.94 |
| 1024 | Framework | 71.33; 5.040 | 69.24; 4.034 | source `ZLINKbg/IO/2` 20.89, `/0` 15.79, `zlink-jvm-raw-m` 14.97, `ForkJoinPool.co` 13.10 / target `ZLINKbg/IO/1` 15.64, `/0` 14.57, `zlink-jvm-raw-m` 14.38 |
| 4096 | Binding | 32.02; 2.011 | 41.77; 3.010 | source `ZLINKbg/IO/0` 13.89, `/2` 13.54 / target `ZLINKbg/IO/1` 13.84, `/0` 13.23, `bench-raw-reque` 11.09 |
| 4096 | Framework | 72.50; 5.052 | 71.45; 4.041 | source `ZLINKbg/IO/2` 20.87, `/0` 16.59, `zlink-jvm-raw-m` 14.52, `ForkJoinPool.co` 13.64 / target `ZLINKbg/IO/1` 16.15, `/0` 14.82, `zlink-jvm-raw-m` 14.70 |

## RTT 단계 증거와 한계

- **caller submit→payload stamp→wire→target handler 진입:** 소스 요청 RTT 샘플과 target `BenchServerMetrics`를 함께 사용했다. `BenchMetricHeader.stamp()`가 source에서 `System.nanoTime()`을 기록하고([BenchMetricHeader.java:51](/root/worktree/zlink-v21-s033/i3871/framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/BenchMetricHeader.java:51)), target은 그 시각부터 `record()`까지 잰다([BenchServerMetrics.java:45](/root/worktree/zlink-v21-s033/i3871/framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/BenchServerMetrics.java:45)). Framework 측 경계는 handler 진입 직후([BenchEchoHandler.java:34](/root/worktree/zlink-v21-s033/i3871/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchEchoHandler.java:34)), Binding은 raw receive 후 header 처리 지점([ZLinkRawBenchServer.java:109](/root/worktree/zlink-v21-s033/i3871/framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:109))이다. 이 경계는 wire와 Framework decode/admission을 따로 나누지 않는다.
- **handler→reply 및 source reply→completion→caller resume:** 기존 RTT에서 target 구간을 뺀 잔여만 계산할 수 있다. Framework source의 `reply_received`는 operation receiver에서 기록되며([ZLinkChannelRouteCalls.java:492](/root/worktree/zlink-v21-s033/i3871/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRouteCalls.java:492)), public future 완료는 뒤이어 `Operation.receiveValue()`에서 이뤄진다([ZLinkServiceOperationRegistry.java:846](/root/worktree/zlink-v21-s033/i3871/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:846)). 따라서 `reply_received`는 caller resume 시각과 같지 않다. Binding에는 같은 flow 경계가 없다.
- 기존 Framework flow를 1024에서 별도 켜서 `sent → received → admitted → dispatched → replied → reply_received` 순서를 확인했다. 하지만 log timestamp 해상도는 1ms이고, 203,990개 flow에서 대부분 단계 p50은 0ms, p99은 약 1ms였다. trace 실행은 30초 warmup까지 로깅해 414MB 로그를 만들고 측정 자체도 크게 교란했다. 따라서 이것은 순서 확인만 가능하며 microsecond 단계 비용 근거로 쓰지 않았다. 이전 JFR 조사도 serial RTT를 CPU sample로 단계 분리할 수 없었고 `ThreadPark` 수집은 부하를 교란한다고 판정했다([jprof/report.md](/root/jobs/fx/cx/v21/s033/jprof/report.md)).

즉, Binding과 Framework 양쪽에서 신뢰할 수 있는 단계 수치는 `payload stamp→target handler` 및 전체 RTT다. 그 밖의 wire, handler/reply, completion 각각은 현재 계측으로 분리되지 않는다. 단일 후속 단계의 비용을 단정할 수는 없다.

## 변경 제안과 소유 결정

가장 큰 **측정 가능한 세부 코드 비용 후보**는 source completion dispatch다. Framework source에서 공통 pool worker가 13.10–13.64µs CPU/요청을 쓰며 source context switch가 Binding보다 약 3회/요청 더 많다. `ZLinkServiceCompletionDispatcher.post()`가 모든 completion을 `ForkJoinPool.commonPool().execute(item)`으로 보낸다([ZLinkServiceCompletionDispatcher.java:51](/root/worktree/zlink-v21-s033/i3871/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51)); terminal claim 뒤 등록된 `Entry`를 post하는 호출 경로는 [ZLinkServiceOperationRegistry.java:407](/root/worktree/zlink-v21-s033/i3871/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407), typed reply decode와 future complete는 [ZLinkServiceOperationRegistry.java:959](/root/worktree/zlink-v21-s033/i3871/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:959)다. CPU/전환 자료는 이 부분이 추가 비용임을 보이지만, 그 wall-time이 잔여 +23–24µs 중 얼마인지는 증명하지 않는다.

| 결정 | 현재 판정 위치 | 소유 spec / 허용 범위 |
|---|---|---|
| terminal claim 후 completion을 어느 실행 자원에 새 turn으로 게시할지 | `ZLinkServiceOperationRegistry.java:407-411`; 실제 게시 `ZLinkServiceCompletionDispatcher.java:51-54` | Submit과 완료 §11은 completion을 process-shared dispatcher로 보내고 잠금 밖의 새 turn에서 실행하도록 한다([§11:492-519](/root/worktree/zlink-v21-s033/i3871/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:492)). 새 turn은 다른 thread일 필요가 없다([§11:508-512](/root/worktree/zlink-v21-s033/i3871/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:508)). |
| completion turn을 실행할 자원 선택 | 현재 common-pool 게시 `ZLinkServiceCompletionDispatcher.java:51-54` | Messaging hot path §4.4는 수신 역할이 없는 기존 실행 자원이 turn을 수행할 수 있고, 새 실행 자원·상한 증설은 금지한다고 정한다([§4.4:302-326](/root/worktree/zlink-v21-s033/i3871/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:302)). |

**제안:** 공통 pool에 매 completion을 게시하는 경로를 대상으로, 이미 존재하는 비수신 실행 자원의 completion turn 경로를 재사용해 게시/worker 깨우기 비용을 줄이는 안을 먼저 검토한다. dispatcher의 process 공유, terminal 확정 뒤 실행, 새 turn 보장, 수신 역할 분리 및 worker 수 제한은 그대로 둔다. 새 thread·queue를 추가하거나 receive callback에서 inline으로 완료하지 않는다. 별도 계측 없이 이 안이 23–24µs 잔여를 전부 줄인다고 단정하지 않으며, 구현 전 경로별 wall-time 계측이 필요하다.

## 원본 및 상태

- 최종 셀 원본·결과: `/root/jobs/fx/cx/v21/s033/i3871/raw/final/` (`measurement.log`, 셀별 `results.json`, `before.json`, `after.json`, `target-stats.json`)
- TID union 스레드 합계: `/root/jobs/fx/cx/v21/s033/i3871/raw/final/summary.jsonl`
- flow 진단 원본: `/root/jobs/fx/cx/v21/s033/i3871/raw/flow/round1/zlink-framework-java-request-serial-1024/`
- 임시 p50 출력 패치는 원복했고 생성한 Java `build/` 디렉터리를 제거했다. `git status --short`는 깨끗하며 테스트는 실행하지 않았다.
