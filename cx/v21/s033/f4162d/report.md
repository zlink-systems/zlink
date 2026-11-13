# #4162 PUBSUB catch-up 수정 및 재측정

**원인** — client는 `PHASE_DRAINED` 전에 단일 epoch를 고정해 가장 늦게 paced 수신을 시작한 subscriber의 시작 지연까지 in-flight로 계산했다 (수정 전 `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:271-275`). 수신은 목표율에 도달했지만 일정과 수신 수 차이가 264개로 남는 현상을 진단 로그에서 확인했다 (`raw/client-pubsub-diag-05.txt:1`).

**바꾼 것** — active와 paced 수신 카운터를 분리하고, warmup/latency phase를 paced 수신으로 센다. 모든 subscriber가 paced 메시지를 한 번씩 받은 순간 공통 anchor를 잡고 카운터를 0부터 시작한다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:17-33`, `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:150-155,221-240,264-269`). Catch-up 규칙은 정책의 1 ms in-flight bound와 0.5초 연속 조건 그대로다 (`doc/perf/PERF_POLICY.md:84-93`).

**수정 전 실패를 모델링한 시험** — 한 subscriber가 250 ms 늦게 시작하는 shared-epoch 사례는 backlog가 남아 catch-up하지 않고, all-subscriber anchor 후 목표율로 수신하는 사례는 catch-up한다 (`bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp:198-247`). `perf_multi_metrics_test` 통과.

**PUBSUB tcp 256 B, 각 5초** — 측정 원시 로그는 `raw/final-pubsub-01.log`–`raw/final-pubsub-05.log`; 각 로그 54-55행과 RESULT 행(104-110행)을 참조.

| 회 | catch-up 초 | 유효 | 평균 / p50 / p95 ms | 처리량 msg/s | catch-up in-flight | paced 수신율 msg/s | 증거 |
|---:|---:|:---:|---:|---:|---:|---:|---|
| 1 | 미도달 | invalid | invalid / invalid / invalid | 1,423,345.6 | — | 0 | `raw/final-pubsub-01.log:55,107-110` |
| 2 | 미도달 | invalid | invalid / invalid / invalid | 1,342,964.2 | — | 0 | `raw/final-pubsub-02.log:55,107-110` |
| 3 | 미도달 | invalid | invalid / invalid / invalid | 1,442,468.0 | — | 0 | `raw/final-pubsub-03.log:55,107-110` |
| 4 | 0.7645 | valid | 145.443845 / 144.703488 / 255.852544 | 1,486,353.8 | ≤11* | 11,861.4 | `raw/final-pubsub-04.log:54-55,104-107` |
| 5 | 미도달 | invalid | invalid / invalid / invalid | 1,466,689.2 | — | 0 | `raw/final-pubsub-05.log:55,107-110` |

`invalid`는 `receiver_not_caught_up_by_deadline` (`raw/final-pubsub-01.log:107-110`, `02:107-110`, `03:107-110`, `05:107-110`). 표의 `paced_received_rate`는 `PHASE_COMPLETE`의 latency-phase 수신만 집계하므로 catch-up 미도달 실행에서는 0이며, tracker가 센 warmup paced 수와는 다른 진단값이다 (`bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:305-314`). *4회 in-flight 정확값은 runner가 client의 `PUBSUB_CATCHUP_DIAG`를 raw report에 전달하지 않아 보존되지 않았다. 정책 bound는 target 11,861.28 msg/s에서 정수 in-flight 최대 11이다 (`raw/final-pubsub-04.log:55`).

**측정 조건** — `flock /root/jobs/fx/cx/measure.lock` exclusive 안에서 5회만 실행했다. cpu0-15 governor는 performance로 설정하고 trap에서 powersave로 복원했으며 현재도 powersave다. run별 before/after에 uptime/load, `free -b`, cpu0-15 `scaling_cur_freq`를 저장했다 (`raw/final-conditions-01-before.txt`–`05-after.txt`). 측정 전후 load 평균 범위는 8.24–9.67 (1분), 4.73–5.66 (5분), 4.53–4.88 (15분)이며 available memory는 약 24.5–25.2 GB였다.

**리팩토링 점검** — C multi PUBSUB 경로에서 성능·POSDDD·불필요 코드 발견 0건; 카운터 소유를 active/paced로 분리했고 catch-up 판정 위치는 1곳에서 1곳이다.

**남은 문제** — 최종 5회 중 4회는 deadline 안에 catch-up하지 못해 latency가 invalid다. 이 재측정 조건에서 해당 실행의 in-flight estimate는 계속 정책 bound보다 컸으며 threshold는 바꾸지 않았다. 정확한 in-flight 추이 및 최종 raw는 `raw/`에 보존했다.
