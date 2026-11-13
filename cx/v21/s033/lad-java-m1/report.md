# Java ladder L1–L3 측정

기준 binding은 2026-11-13 UTC의 `zlink-java` 3회 측정 중앙값이며, 각 셀의 원자료는 [binding-daily/java-2026-11-13.json](/root/jobs/fx/cx/v21/s033/binding-daily/java-2026-11-13.json)에 기록했다. 이 실행의 fresh build와 단일 lock hold 측정 결과를 아래에 추가한다.

| 셀 | binding (msg/s) | raw 중앙값 |
|---|---:|---|
| send-saturation 1K | 505,353.6 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-send-saturation-1024/results.json) |
| send-saturation 4K | 198,901.6 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-send-saturation-4096/results.json) |
| request-serial 1K | 11,554.0 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-request-serial-1024/results.json) |
| request-serial 4K | 11,123.4 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-request-serial-4096/results.json) |
| request-backpressure 1K | 154,763.4 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-request-backpressure-1024/results.json) |
| request-backpressure 4K | 106,317.2 | [run1–3 results.json](/root/jobs/fx/cx/v21/s033/lad-java-par/raw/fresh/L0/run1/zlink-java-request-backpressure-4096/results.json) |

## Ladder steps

<!-- append per-step tables immediately after the measurement hold -->

## 부분 hold 1 (무효)

| 단계 | 셀 | 관측 msg/s | errors / submitted=completed=received | 판정 | raw |
|---|---|---:|---|---|---|
| L0 | send-saturation 1K | 525,476.6 | 0 / 2,627,383=2,627,383=2,627,383 | 무효: SCHED_IDLE 우선순위 구간 포함. 재측정 예정 | [results.json](/root/jobs/fx/cx/v21/s033/lad-java-m1/raw/session/L0/run1/zlink-framework-java-send-saturation-1024/results.json) |

이 hold는 run-group Python 검증 heredoc 오류로 첫 셀 뒤 중단됐다. 이 raw를 보존하고 본 비교에서는 제외한다.

## 부분 hold 2 (무효)

| 단계 | 셀 | 관측 msg/s | errors / submitted=completed=received | 판정 | raw |
|---|---|---:|---|---|---|
| L0 | send-saturation 1K | 534,511.4 | 0 / 2,672,557=2,672,557=2,672,557 | 무효: 결과 JSON이 완성되기 전에 progress watcher가 읽어 hold가 조기 종료됨. ladder 비교에 사용하지 않음 | [results.json](/root/jobs/fx/cx/v21/s033/lad-java-m1/raw/session2/L0/run1/zlink-framework-java-send-saturation-1024/results.json) |

남은 background runner는 종료했다. 원자료는 보존하며, watcher가 `target_stats` 완성을 기다리도록 수정하고 `session3` 경로에 새 전체 hold를 실행한다.
