# Framework messaging 성능 개선 계획 — send/request 병목 (2026-09-10)

> 사용자 요청(2026-09-10 01:20): "framework의 send·request 전체 성능 병목 원인을 분석하고 개선한다.
> binding 라이브러리와 10% 이상 차이 나면 문제다."
>
> 목표: 같은 언어 안에서 `zlink-framework-<lang> / zlink-<lang>` ≥ **0.90** (throughput, 3-run 중앙값,
> 규격 `framework/bench/grpc/README.ko.md` §7.2 formula 2의 합격선 0.80보다 높은 사용자 기준).
> 측정 도구는 2차 bench(server-driven, `framework/bench/grpc/`)를 그대로 쓴다. 조건을 완화하지 않는다.

## 1. 출발점 — 2차 bench 3-run 중앙값 (2026-09-09, `framework/bench/grpc/doc/comparison.ko.md`)

| 언어 | request-serial (fw / raw) | request-window @100 | send-saturation | 비고 |
|---|---:|---:|---:|---|
| .NET | 1.56 / 7.52 KOPS = **0.21** (latency 0.64 vs 0.13 ms) | 12.3 / 98.9 = **0.12** | 107 / 706 KMSG/s = **0.15** (drain 9.7 s) | backpressure는 admission 거절 오류 2,511건(FB-042·FB-047) |
| Java | 0.47 / 6.61 = **0.07** (2.1 vs 0.15 ms) | 2.3 / — (raw는 FB-050 유실) | 10.5 / 545 = **0.02** (drain 2.3 s) | Kotlin도 동일(2.26) |
| C++ | 0.50 / 7.67 = **0.065** (2.0 vs 0.13 ms) | 1.06 / 176 = **0.006** (92 ms latency) | 0 (FB-054 peer 소실) | FB-052·FB-053 |
| Node | unsupported (codec protobuf bytes 미지원) | — | — | codec 결함 수정이 선행 |

공통 관찰: framework 행의 request-serial 지연이 raw의 **5~15배**(0.6~2.1 ms)이고, window 100에서 처리량이
serial의 2~8배에 그친다(raw는 13~23배). 즉 요청당 **ms 단위의 고정 대기**가 경로 어딘가에 있고 동시성이
그 대기를 겹치지 못한다. 후보(감독자 사전 grep):

- Java `ZLinkJavaRawMeshNode.startPump()`(`:4243-4275`): 서비스 펌프가 `port.receive()`가 비면
  `LockSupport.parkNanos(1 ms)`로 **폴링**한다 — 요청·응답 각 hop마다 최대 1 ms 지연.
- C++ `mesh_node_host_service.cpp:2429·:2474-2476`(dispatch activity 대기 1 ms 단위), `raw_mesh_node_owner_t::wait_for_activity`(`port->poll(timeout)`의 timeout 값), `route_mesh_runtime_service.cpp:482`(10 ms sleep 펌프).
- .NET `ZLinkManagedMeshNode`(`PollInterval` 100 ms, `ReceiveBatchSize` 64, poller 등록 flags), `ZLinkChannelReceiveLoop`, `ZLinkChannelApplicationDispatchQueue` — FB-047의 admission 경로.
- 공통: typed codec(protobuf) 인코딩/디코딩 경로의 복사 횟수, 요청당 task/continuation·map 등록·timer 등록 비용, application job queue의 hop 수.

## 2. 진행 방식

| 단계 | 내용 | 담당 |
|---|---|---|
| P1 진단 | 언어별로 request-serial·request-window·send-saturation 경로를 프로파일(perf/JFR/dotnet-trace)해 ms 단위 대기의 **정확한 위치**와 hop별 비용 지도를 만든다. 수정 후보와 예상 효과, 소유 계층(framework runtime / binding / Core)을 보고한다 | codex: C++ astra, .NET sol, Java sol |
| P2 수정 | 진단이 지목한 framework runtime 수정(B 분류)을 감독자가 승인한 범위에서 구현. 공개 계약·timeout·HWM 불변. 회귀 테스트 추가 | codex |
| P3 재측정 | 2차 bench 3-run(티켓 큐, 조용한 창), 집계기로 formula 2 판정. 0.90 미만이면 P1로 | 감독자 |
| Node | framework codec protobuf `bytes` 종류 추가(unsupported 해제) → P1~P3 | codex sol |

규칙: 측정·프로파일은 `scripts/perf/perf-ticket.sh`로만. 벤치 조건·규격은 바꾸지 않는다. 각 job은 보고서를
`.artifacts/codex/fwperf-<lang>/summary.md`에 남기고, 감독자가 검증 뒤 이 문서 §3에 결과를 적는다.
결정 기록은 `doc/plan/fw-bench-worklog/decisions.ko.md` FB-056부터 이어 쓴다.

## 3. 진행 기록

| 날짜 | 언어 | 단계 | 결과 |
|---|---|---|---|
| 2026-09-10 | C++·.NET·Java | P1 시작 | job `fwperf-cpp`(astra), `fwperf-dotnet`(sol), `fwperf-java`(sol) |
| 2026-09-10 | C++ | P1 완료 | FB-056: 1 ms sleep 아님. record당 끝나는 dispatch 회차(873 µs 관리 작업)와 요청당 동기 state-lane 왕복이 병목; FB-054는 probe가 application FIFO 뒤에 놓여 만료. P2 승인 → job `fwperf-cpp-p2` (Issue #7·#8) |
| 2026-09-10 | .NET | P1 완료 | FB-057: 단일 receive loop 1건 읽기(spec 64)·pump 1 claim·요청별 cold Task+DI scope·codec 전체 복사. 100 ms poll은 원인 아님. P2 승인 → job `fwperf-dotnet-p2` (Issue #5·#19) |
| 2026-09-10 | Java | P1 완료 | FB-058: 1 ms park 폴링(+49% 단독)·요청당 state-lane park 5회·permit 전 receive/3회 복사/1건 claim·executor hop. P2 승인 → job `fwperf-java-p2` (Issue #6) |
