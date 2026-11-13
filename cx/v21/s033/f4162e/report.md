# #4162 PUBSUB catch-up schedule anchor

**원인** — 4cfb084fa8의 수신기는 마지막 subscriber가 첫 paced 메시지를 받은 시각을 epoch로 삼고 `paced_received`를 0으로 초기화했다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:231-238` at base). 따라서 이미 지난 송신 일정이 backlog에서 빠졌다. 정책은 송신 일정상 전송 수를 기준으로 한다 (`doc/perf/PERF_POLICY.md:84-93`).

**바꾼 것** — 첫 paced header의 기존 `sent_ts_ns`를 sender schedule epoch로 사용하고, 누적 paced 수신 카운터를 유지한다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:229-234,257-262`; timestamp 기록은 `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:64-67`). 새 protocol field는 없다. catch-up bound와 0.5초 연속 조건은 유지했다 (`bindings/c/perf/multi/common/perf_multi_measurement.hpp:31-53`; 정책 `doc/perf/PERF_POLICY.md:87-93`).

**수정 전 실패를 모델링한 단위 사례** — target 100 msg/s, sender epoch 이후 150 ms lag는 15개 backlog를 계속 남겨 catch-up하지 않아야 하고, 무지연 입력은 catch-up한다 (`bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp:210-232`). 이전 receive-time epoch 및 counter reset을 적용하면 매 tick scheduled와 received가 같아져 0.5초 후 잘못 catch-up하므로 delayed assertion과 모순된다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:231-238` at base; tracker 조건 `bindings/c/perf/multi/common/perf_multi_measurement.hpp:35-50`). `perf_multi_metrics_test` 통과.

**PUBSUB tcp 256 B, 각 5초 — 5회** — 모든 실행이 10초 catch-up 관찰 기한까지 catch-up하지 못해 latency는 invalid다. `paced_received_rate=0`은 latency-phase 결과 진단값이며, 수신 누적은 각 progress 행에 기록했다 (`bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:305-313`).

| 회 | catch-up s | 유효성 | 평균 / p50 / p95 ms | 처리량 msg/s | catch-up in-flight | paced_received_rate msg/s | progress in-flight (기저/최대) | 근거 |
|---:|---:|:---:|---|---:|---:|---:|---:|---|
| 1 | 미도달 (>10) | invalid | invalid / invalid / invalid | 1,346,537.2 | — | 0 | 19 / 265 | `raw/final-pubsub-01.log:55-66,118-121` |
| 2 | 미도달 (>10) | invalid | invalid / invalid / invalid | 1,495,621.0 | — | 0 | 22 / 729 | `raw/final-pubsub-02.log:55-66,118-121` |
| 3 | 미도달 (>10) | invalid | invalid / invalid / invalid | 1,478,282.4 | — | 0 | 21 / 141 | `raw/final-pubsub-03.log:55-66,118-121` |
| 4 | 미도달 (>10) | invalid | invalid / invalid / invalid | 1,503,491.4 | — | 0 | 22 / 490 | `raw/final-pubsub-04.log:55-66,118-121` |
| 5 | 미도달 (>10) | invalid | invalid / invalid / invalid | 1,493,818.6 | — | 0 | 21 / 164 | `raw/final-pubsub-05.log:55-66,118-121` |

**일관성 점검** — 유효 실행이 0회라 catch-up in-flight / target_rate와 post-catch-up p50 비교 대상은 없다. progress의 기저 in-flight는 target_rate 10,753–11,968 msg/s 기준 약 1.6–1.9 ms이고 정책의 1 ms bound보다 높아, 다섯 실행 모두 invalid 판정과 일치한다 (`raw/final-pubsub-01.log:55-66`–`05.log:55-66`; 정책 `doc/perf/PERF_POLICY.md:87-93`).

**runner 로그 전달** — client의 `PUBSUB_CATCHUP_DIAG`와 `PUBSUB_CATCHUP_PROGRESS` prefix를 출력 allowlist에 추가했다 (`bindings/c/perf/run_comparison.py:902-906`). 다섯 raw log에 progress가 보존됐고 catch-up이 없어서 DIAG 행은 발생하지 않았다 (`raw/final-pubsub-01.log:55-66`–`05.log:55-66`).

**측정 조건** — `run-pubsub-five.sh`가 runs 구간에만 exclusive `measure.lock`을 잡고 cpu0–15 performance 설정 및 EXIT 시 powersave 복원을 수행했다 (`run-pubsub-five.sh:23,45-64`). run별 uptime/load, `free -b`, cpu0–15 `scaling_cur_freq` before/after는 `raw/final-conditions-01-before.txt`–`05-after.txt`에 있다. 10개 snapshot의 load 평균 범위는 1/5/15분 각각 4.95–6.36 / 5.63–5.87 / 5.60–5.68, available memory 23.89–24.41 GB, scaling frequency 400,000–4,420,032 kHz였다 (`raw/final-conditions-01-before.txt`–`05-after.txt`).

**리팩토링 점검** — C multi PUBSUB 경로에서 성능·POSDDD·불필요 코드 추가 발견 0건; catch-up 판정 위치는 1곳에서 1곳이며, 사용처가 사라진 receive-start helper를 제거했다 (`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:257-275`, `bindings/c/perf/multi/common/perf_multi_measurement.hpp:16-20`).

**남은 문제** — 5회 모두 정책 bound를 0.5초 유지하지 못해 latency는 invalid이고 throughput만 유효하다 (`raw/final-pubsub-01.log:55-66,118-121`–`05.log:55-66,118-121`). 기준을 완화하지 않았다 (`doc/perf/PERF_POLICY.md:87-93`).
