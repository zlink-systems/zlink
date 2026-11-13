# #4190 Java/Kotlin 작업 보고

**결과**
- Java와 Kotlin 보조 클라이언트의 `request-backpressure`·`send-saturation`은 stream마다 전용 platform submit thread를 실행한다. `request-serial`은 1개 stream과 in-flight 1을 유지한다.
- stream별 sequence, counters, in-flight와 latency sample을 보유하고 phase 종료 때 집계한다. raw binding은 전용 completion-poller 한 개가 binding poller를 구동하며 submit thread는 이를 호출하지 않는다.
- Trigger와 metadata 필드를 `submitConcurrency`로 변경했다.

**판정 위치 (전 → 후)**
- 제출 thread 수: 단일 backpressure loop와 send/raw 경로의 분산 판단(`BenchDrivers.java` HEAD: `runPattern`·`runRaw`·`runBackpressure`) → pattern별 stream 수를 `runPattern`에서 정하고(`BenchDrivers.java:178-215`), raw/framework/gRPC 각 driver에 전달(`232`, `341`, `472`, `559`). 결정 위치 3 → 1.
- sequence: driver별 local/shared allocator(`BenchDrivers.java` HEAD `234`, `317`, `515`, `552`) → stream prefix와 local ordinal을 만드는 `sequence`(`BenchDrivers.java:336`), 위치 4 → 1. reply 검증은 기존 sequence 기반을 유지한다.
- 집계: 공용 `SourceMetrics` 직접 갱신(HEAD `618`) → stream metric 생성 `createStreams`(`BenchDrivers.java:682`)과 phase 종료 merge `freezeTerminal`(`813-847`), merge 위치 1 → 1.
- 소유 범위는 bench driver이며 근거는 승인 스펙 §10.3이다. Java/Kotlin은 공용 driver를 사용해 동일하게 맞췄다. 구현 분류: A(승인된 스펙 이행). 제품 runtime은 수정하지 않았다.

**바꾼 것**
- `BenchDrivers.java`, `RawStack.java`: thread 구성, per-stream metrics/sequence와 raw completion-poller 소유권.
- `BenchOptions.java`, `BenchHttpApplication.java`, `BenchResultWriter.java`, `BenchClient.java`, Kotlin `BenchKotlinClient.kt`, 두 Java runner와 관련 테스트: `submitConcurrency` 전환 및 stream metadata.
- 공용 `runner_common.sh`는 지정된 두 줄만 변경했다.
- 커밋된 실수 출력 4개를 삭제했다: `framework/bench/grpc/java/framework/bench/grpc/log/java/smoke300/`의 `with_grpc_java_20260913_004105.txt`, `report.txt`, `results.json`, `target-stats.json`.

**검증**
- `./gradlew --no-daemon --max-workers=1 :client:check :shared:check :kotlin-client:compileKotlin` — 성공. Java bench focused checks, shared checks, Kotlin compile 통과.
- `./build.sh` — 성공. 마지막 생성 산출물은 Gradle `clean`으로 제거했다.
- `flock -o /root/jobs/fx/cx/measure.lock ... ./run_local.sh`, `WARMUP_SECONDS=5`, `DURATION_SECONDS=5`, 1024 bytes — binding/Framework의 serial, backpressure, send 여섯 셀 모두 completed > 0, errors 0. 결과 JSON은 backpressure/send `streams.count=8`, serial `streams.count=1`, 전체 `submitConcurrency=8`을 기록했다. 원본은 `/root/jobs/fx/cx/v21/s033/s4190j/smoke/`에 보관했다.
- active backpressure 중 `/proc`에서 `bench-stream-0`부터 `bench-stream-7`까지 확인했다. 목록: `/root/jobs/fx/cx/v21/s033/s4190j/smoke-thread-list.txt`.
- `git diff --check` — 성공.

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 없음, 넘긴 항목 없음.

**커밋**
- `fd586e515e` (`bench(java): submit each logical stream on dedicated thread`). Push하지 않았다.

**남은 문제 · 다음 할 일**
- 없음.
