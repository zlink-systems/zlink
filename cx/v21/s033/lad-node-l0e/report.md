# #4350 Node Ladder L0 재측정

## 결과 요약

L0 서버를 worker thread에서 Node 주 이벤트 루프로 옮겨 raw binding 서버와 같은 `setImmediate` 수신 펌프와 `BenchServerMetrics` 일반 카운터를 사용하게 했다. 여섯 셀 중 다섯 셀은 `L0 / binding >= 0.95`를 만족했다. request-serial 4K는 0.916으로 미달했으며, 아래 CPU 프로파일은 남은 Framework ROUTER 수신·응답 경로 비용을 보여준다.

| 패턴 | 크기 | L0 중앙값 msg/s | binding 중앙값 msg/s | 비율 | 기준 | G5 | 원자료 |
|---|---:|---:|---:|---:|---|---|---|
| send-saturation | 1K | 119,421.0 | 114,926.4 | 1.039 | 통과 | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-send-saturation-1024/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-send-saturation-1024/results.json), run 1–3 |
| send-saturation | 4K | 79,862.0 | 81,483.2 | 0.980 | 통과 | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-send-saturation-4096/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-send-saturation-4096/results.json), run 1–3 |
| request-serial | 1K | 9,573.0 | 9,314.6 | 1.028 | 통과 | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-request-serial-1024/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-request-serial-1024/results.json), run 1–3 |
| request-serial | 4K | 8,009.6 | 8,748.6 | **0.916** | **미달** | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-request-serial-4096/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-request-serial-4096/results.json), run 1–3 |
| request-backpressure | 1K | 30,409.2 | 30,940.4 | 0.983 | 통과 | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-request-backpressure-1024/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-request-backpressure-1024/results.json), run 1–3 |
| request-backpressure | 4K | 22,180.8 | 22,871.6 | 0.970 | 통과 | 통과 | [L0](raw/six-cells/bench/node/run1/zlink-framework-node-request-backpressure-4096/results.json) [binding](raw/six-cells/bench/node/run1/zlink-node-request-backpressure-4096/results.json), run 1–3 |

표의 원자료 링크는 run 1이며, 전체 run 1–3은 [`raw/six-cells/bench/node/`](raw/six-cells/bench/node/)에 있다. 집계표는 [`raw/aggregates/six-cells.md`](raw/aggregates/six-cells.md), JSON은 [`raw/aggregates/six-cells.json`](raw/aggregates/six-cells.json)이다. 모든 셀에서 submitted/completed/target received 수가 일치했고 오류는 0이었다. 12개 구현·셀 조합 모두 G5 통과(최대 편차 7.64%)로 추가 반복 기준에 해당하지 않았다.

## 수정

- `framework/bench/grpc/node/zlink-framework-server/main.js`: L0 서버를 같은 프로세스의 직접 ROUTER 서버로 전환했다. worker thread와 `poller.wait()`/wake 경로를 없애고 `zlink-raw-server/main.js:121-142`의 bounded `setImmediate(pump)` idle 정책을 맞췄다. 최대 256개를 drain한 뒤 다시 예약한다.
- L0는 `shared/bench-server-metrics.js:33-46`의 일반 JavaScript 카운터를 공유한다. 메시지당 두 번 하던 `Atomics.add`와 별도 shared counters를 제거했다.
- `framework/bench/grpc/node/zlink-framework-server/l0-worker.js`: 제거했다. 별도 worker를 유지해야 할 구조적 이유가 없었다. socket과 stats는 주 이벤트 루프에서 함께 진행하고 같은 `setImmediate` 펌프가 처리한다.
- 커밋: `8cbbf6387b perf(node): align ladder L0 receive path`, `48655c21ca perf(node): match L0 idle pump policy`. branch `exp/4350-node-ladder`; 측정은 두 번째 커밋 기준으로 수행했다.

## 미달 셀 원인 프로파일

request-serial 4K L0는 binding보다 8.45% 낮았다. `raw/profile/request-serial-4k/`에 진단 CPU 프로파일을 보존했다. 세 L0 서버 프로파일에서 Core의 ROUTER `recv` (`framework/languages/node/node_modules/@zlink-systems/zlink/dist/zlink/runtime/sockets/socket_operations.js:156`)가 약 13–17%의 CPU 샘플, `routedReceivedPrefersManagedBuffer` (`.../messaging/message_materializer.js:26`)가 약 2.7–3.8%, `replyDirect` (`.../sockets/router_socket.js:56`)가 약 2.2–2.7%를 차지했다. L0의 `pump`와 `schedulePump` 샘플 합은 약 1.6–2.2%였고 `EchoHandler.handleL0` (`zlink-framework-server/bench-handlers.js:17-19`)은 약 0.05–0.07%, `BenchServerMetrics.record` (`shared/bench-server-metrics.js:33-40`)은 약 0.01–0.03%였다. 대표 프로파일은 [`CPU.20261113.165750.763157.0.001.cpuprofile`](raw/profile/request-serial-4k/CPU.20261113.165750.763157.0.001.cpuprofile)이다.

이에 대응하는 L0 호출부는 `framework/bench/grpc/node/zlink-framework-server/main.js:64-83`이다. 각 응답에서 Framework ROUTER의 `Received.reply()`를 통해 새 response operation을 만들고 두 wire part를 submit한다. 프로파일은 L0 서버 isolate의 CPU를 보여주며, raw target 서버에는 프로파일이 생성되지 않아 이 자료만으로 두 서버 간 CPU 샘플 차이를 정량화할 수는 없다. 따라서 Core 수신·라우터 직접 응답/materialization은 L0에서 관측된 주요 비용 경로로 귀속하고, 두 구현의 차이 전체를 그 경로 하나의 인과 효과로 단정하지 않는다. L0 자체 JS 펌프, handler, 계측은 프로파일에서 작고, 이번 수정은 per-request blocking wait와 atomic 계측을 제거했다. L0가 실행하지 않는 Framework dispatch/service path 전체 비용도 이 측정에 포함되지 않는다.

## 측정 절차 및 검증

- kairos 측정 서버에서 exclusive measurement lock을 사용하고 source CPU 0–7, target CPU 8–15, warmup 30초, active 5초, 3회 실행했다. L0와 binding 6개 셀은 한 lock session에서 수행했다.
- 먼저 `bash framework/bench/grpc/node/build.sh`로 Framework와 Nest dist를 갱신했고, 측정 전 worktree-local dist/native binding/Core artifact와 해시를 기록했다. 최종 측정 commit은 `48655c21ca`, dirty=false였다.
- 셀별 raw `results.json`에 errors=0, server errors=0, submitted=completed=target received임을 확인했다. 모든 36개 결과와 driver 기록은 `raw/six-cells/`에 있다.
- 로컬 확인: `node --check`, `git diff --check`, Node build. branch는 clean이며 push/PR은 하지 않았다.

## L0 클라이언트 교차 시험 — send-saturation 1K

| 서버 / 클라이언트 | 중앙값 msg/s | G5 spread | 판정 | 원자료 |
|---|---:|---:|---|---|
| raw / raw | 118,762.0 (3회) | 8.38% | 통과 | [run 1](raw/cross-l0-client/raw-raw/bench/node/run1/zlink-node-send-saturation-1024/results.json) [run 2](raw/cross-l0-client/raw-raw/bench/node/run2/zlink-node-send-saturation-1024/results.json) [run 3](raw/cross-l0-client/raw-raw/bench/node/run3/zlink-node-send-saturation-1024/results.json) |
| Framework L0 / raw | 118,945.8 (5회) | 15.19% | 5회 후에도 spread 초과 | [run 1–3](raw/cross-l0-client/l0-raw/bench/node/) [run 4–5](raw/cross-l0-client/l0-raw/repeats-4-5/bench/node/) |
| Framework L0 / Framework L0 | 113,787.0 (3회) | 6.35% | 통과 | [run 1](raw/cross-l0-client/l0-client/bench/node/run1/zlink-framework-node-send-saturation-1024/results.json) [run 2](raw/cross-l0-client/l0-client/bench/node/run2/zlink-framework-node-send-saturation-1024/results.json) [run 3](raw/cross-l0-client/l0-client/bench/node/run3/zlink-framework-node-send-saturation-1024/results.json) |

L0 클라이언트 시험은 bench `zlink-framework-node`를 선택하고 `ZLINK_LADDER_L0_CLIENT=1`을 설정해 binding transport를 쓰는 L0 client 경로로 실행했다. raw/raw 대비 중앙값 비율은 0.958이다. 각 결과의 errors와 server errors는 0이며 submitted=completed=target received다. L0 서버/raw client는 첫 세 번의 spread가 17.85%여서 G5 규칙에 따라 두 번 더 측정했다. 5회 중앙값은 118,945.8 msg/s지만 spread가 15.19%로 여전히 10%를 넘으므로 그 조합은 재현성 판정에 실패한다. L0 client와 raw/raw 집계는 [`raw/aggregates/cross-l0-client-pair.md`](raw/aggregates/cross-l0-client-pair.md), L0/raw의 5회 G5 집계는 [`raw/aggregates/cross-l0-raw-five.md`](raw/aggregates/cross-l0-raw-five.md)다. 전체 cross raw는 [`raw/cross-l0-client/`](raw/cross-l0-client/)에 있다.
