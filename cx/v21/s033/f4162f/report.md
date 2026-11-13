# #4162 PUBSUB catch-up 진단 및 재측정

**진단 — counting artifact가 아니라 실제 수신 지연(b)**

짧은 PUBSUB tcp 256 B 진단 run에서 subscriber 100개의 첫 paced `seq`가 모두 `1355473`이었다. 전역 첫 paced seq도 `1355473`이므로 최소 subscriber의 `(first_seq − global_first_seq)=0`; 약 19개를 설명할 시작 skew는 없다 (`raw/diagnostic-pubsub-01.log:54-165`).

같은 tick의 최소 수신 subscriber와 최신 latency 표본은 다음과 같다. target rate는 6,655.04 msg/s, 1 ms bound는 6.66개다.

| tick | 최소 subscriber | in-flight | first_seq 차이 | latency 표본 |
|---:|---:|---:|---:|---:|
| 1 s | 1 | 10 | 0 | 1.124 ms |
| 2 s | 13 | 10 | 0 | 1.023 ms |
| 3 s | 15 | 17 | 0 | 1.658 ms |
| 6 s | 7 | 18 | 0 | 1.653 ms |
| 9 s | 12 | 17 | 0 | 1.785 ms |

근거: 각 tick의 progress/sample 행 `raw/diagnostic-pubsub-01.log:166,168;267,281;368,384;671,679;974,987`. in-flight/target는 각각 약 1.50, 1.50, 2.55, 2.70, 2.55 ms로 latency 표본과 같은 크기다. 최초 0.5 ms tick의 작은 값은 측정 시작 구간이다 (`:65-76`).

**원인 위치** — 수신 누적은 subscriber별로 집계되고, 판정은 그 최솟값을 sender 첫 paced `sent_ts_ns` epoch의 scheduled 수와 비교한다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:229-233,257-275`). 따라서 이 run의 잔여 in-flight는 첫 수신 시점 차이로 계산된 값이 아니다. 실제 최소 subscriber latency가 약 1.0–1.8 ms여서 `max(1, target_rate × 1 ms)` 경계를 넘어간다. tracker의 1 ms bound와 연속 0.5초 조건은 그대로 유지했다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:35-52`; `doc/perf/PERF_POLICY.md:84-93`).

**바꾼 것** — catch-up/count 규칙 변경 없음(b). 진단용 per-subscriber 로그는 짧은 run 후 제거했다. 작업 트리에 코드 차이는 없다.

**수정 전 실패한 시험** — 해당 없음. 원인이 counting artifact가 아니므로 규칙이나 시험 기대를 바꾸지 않았다. 관련 metrics test 바이너리는 compile했으나 실행하지 않았다.

**PUBSUB tcp 256 B, latency duration 5 s × 5회** — 모든 run이 기한 내 catch-up하지 못해 latency mean/p50/p95는 invalid다. 처리량은 유효하다. `paced_received_rate=0`은 catch-up 후 latency window가 없어 기록된 값이다.

| 회 | catch-up s / 유효성 | mean / p50 / p95 ms | 처리량 msg/s | catch-up in-flight | paced_received_rate / target_rate msg/s | 최대 progress in-flight | late_sends_gt_1ms |
|---:|---|---|---:|---:|---:|---:|---:|
| 1 | 미도달 / invalid | invalid / invalid / invalid | 1,280,132.8 | — | 0 / 10,165.92 | 2,956 | 0 |
| 2 | 미도달 / invalid | invalid / invalid / invalid | 1,387,631.0 | — | 0 / 11,084.96 | 11,300 | 0 |
| 3 | 미도달 / invalid | invalid / invalid / invalid | 1,379,374.8 | — | 0 / 10,966.24 | 10,184 | 0 |
| 4 | 미도달 / invalid | invalid / invalid / invalid | 1,350,283.0 | — | 0 / 10,768.64 | 13,471 | 0 |
| 5 | 미도달 / invalid | invalid / invalid / invalid | 1,346,498.4 | — | 0 / 10,741.28 | 76,816 | 40,360 |

근거는 각 run의 `raw/final-pubsub-0N.log`: 진단·progress·결과는 01 `:65,117,120`, 02 `:66,118,121`, 03 `:66,118,121`, 04 `:66,118,121`, 05 `:71,123,126`. 다섯 run 모두 catch-up 시각과 catch-up in-flight가 없어 요청된 `in_flight_at_catch_up / target_rate` 대 post-catch-up p50 검사는 계산할 수 없다. run 5는 `late_sends_gt_1ms=40,360`; 나머지는 0이다. 이는 해당 run의 큰 progress backlog도 함께 설명할 수 있는 측정 조건 차이다.

**측정 조건** — `run-pubsub-five.sh`의 `flock -x /root/jobs/fx/cx/measure.lock` 안에서 cpu0–15 governor를 performance로 설정하고 EXIT 시 powersave로 복원했다 (`run-pubsub-five.sh:21-49`). 전후 snapshot 10개는 `raw/final-conditions-01-before.txt`–`05-after.txt`; 전체 범위는 uptime 27일 12:34–12:36, 1/5/15분 load 2.39–6.69 / 3.63–4.76 / 3.95–4.32, available memory 21.66–22.57 GiB, cpu0–15 scaling_cur_freq 400,000–3,906,784 kHz다. 종료 후 cpu0–15는 모두 powersave였다.

**리팩토링 점검** — 검사한 C multi PUBSUB 경로에서 성능·POSDDD·불필요 코드 발견 0건; catch-up 판정 위치 1곳 유지(1→1), 코드 변경 0건.

**남은 문제** — 다섯 재측정 모두 latency invalid다. 1 ms bound·0.5초 hold·기한은 변경하지 않았다. no-code 진단 결과라 tracked diff가 없으며, 요청한 빈 진단 기록 커밋은 `98b0d258f4`다.
