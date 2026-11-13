# #4162 PUBSUB catch-up 후속

## 결정 소유 위치

| 항목 | 변경 전 | 변경 후 |
|---|---|---|
| catch-up 판정 | `catch_up_tracker_t::observe` 한 곳에서 0.5초 수신율 ≥95%와 backlog 비증가를 판정했다 (`7a65e1f013:bindings/c/perf/multi/common/perf_multi_measurement.hpp:36-47`). | 같은 tracker 한 곳에서 in-flight ≤ `max(1, target_rate × 1 ms)` 상태가 연속 0.5초인지 판정한다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:24-50`; 정책 `doc/perf/PERF_POLICY.md:84-93`). 판정 위치는 1곳에서 1곳으로 유지했다. |
| latency 시작 | client가 tracker의 `caught_up()` 이후 표본을 누적하고 server가 `PHASE_CATCHUP`을 따른다 (`7a65e1f013:bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:237-254`; `7a65e1f013:bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:263-298`). | 동작 경로를 유지했다. catch-up 순간 초와 최대 subscriber in-flight를 출력한다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:246-256`). |

기존 판정은 목표율로 계속 수신하면서 backlog가 남아 있어도 통과했다 (`7a65e1f013:bindings/c/perf/multi/common/perf_multi_measurement.hpp:36-47`). 수정 분류: **B 기존 벤치 판정 결함**. 판정 소유자는 C multi PUBSUB tracker이며, 다른 binding runtime 변경은 없다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:15-58`).

## 검증 및 PUBSUB tcp 256B · 5회

| 회 | catch-up 초 | 유효성 | 평균 / p50 / p95 ms | 처리량 msg/s | catch-up 시 최대 in-flight |
|---:|---:|:---:|---:|---:|---:|
| 1 | 미도달 | invalid | invalid / invalid / invalid | 1,145,284.2 | 해당 없음 (`raw/pubsub-runs.tsv:2`) |
| 2 | 미도달 | invalid | invalid / invalid / invalid | 1,207,084.4 | 해당 없음 (`raw/pubsub-runs.tsv:3`) |
| 3 | 미도달 | invalid | invalid / invalid / invalid | 1,199,610.0 | 해당 없음 (`raw/pubsub-runs.tsv:4`) |
| 4 | 미도달 | invalid | invalid / invalid / invalid | 1,224,100.6 | 해당 없음 (`raw/pubsub-runs.tsv:5`) |
| 5 | 미도달 | invalid | invalid / invalid / invalid | 1,217,260.0 | 해당 없음 (`raw/pubsub-runs.tsv:6`) |

다섯 회 모두 deadline까지 drain 조건을 만족하지 못해 `receiver_not_caught_up_by_deadline`로 latency가 invalid였으며 throughput은 기록됐다 (`raw/pubsub-01.log:54-58`; `raw/pubsub-02.log:54-58`; `raw/pubsub-03.log:54-58`; `raw/pubsub-04.log:54-58`; `raw/pubsub-05.log:54-58`). 모든 원시 로그는 `raw/pubsub-01.log`–`raw/pubsub-05.log`에 보존했다 (`raw/verification-summary.txt:6`).

- 빌드 통과, metrics self-test 종료 코드 0, aggregator 46 tests 통과, `git diff --check` 통과 (`raw/verification-summary.txt:2-7`).
- 단위 사례는 drained, 일정한 큰 backlog로 목표율을 따라가는 경우, never와 invalid RESULT를 확인한다 (`bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp:168-204`).
- 리팩토링 점검: 성능·POSDDD·불필요 코드 발견 0건, 추가 수정 0건. 기존 window/rate 이력 필드를 drain 시작과 catch-up in-flight 기록으로 대체했다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:18-57`).
- 로컬 commit `e068171c77`로 저장했으며 push하지 않았다 (`raw/verification-summary.txt:9`).
