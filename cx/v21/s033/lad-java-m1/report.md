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
