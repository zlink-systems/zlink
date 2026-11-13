# #4162 — C perf latency histogram

## 결과

단일 inproc A/A에서 진단 당시의 고 latency 모드(평균 ≥0.9ms)는 0/20회였고, 기존 low/high 분리는 관찰되지 않았다. PM 기준은 **실패**했다: 평균 중앙값 0.0081585ms, 최솟값 0.002932ms, 최댓값 0.028651ms, max/min 9.77배, 20회 중 2회만 중앙값의 ±5% 이내여서 전체 기준을 충족하지 못했다. 한 번의 고점(p95 0.122368ms, 11회차)이 포함되어 있다.

| 회 | 평균 ms | p95 ms | 처리량 msg/s | 큐 근사 건수 |
|---:|---:|---:|---:|---:|
| 1 | 0.012202 | 0.004320 | 1729863 | 5.27 |
| 2 | 0.009845 | 0.006656 | 1658605 | 17.74 |
| 3 | 0.008239 | 0.004672 | 1586672 | 18.59 |
| 4 | 0.017831 | 0.005760 | 1612632 | 5.95 |
| 5 | 0.003915 | 0.003744 | 1725100 | 12.84 |
| 6 | 0.014605 | 0.005792 | 1680469 | 8.50 |
| 7 | 0.005964 | 0.004224 | 1702797 | 12.63 |
| 8 | 0.016200 | 0.005728 | 1711793 | 5.53 |
| 9 | 0.009906 | 0.004608 | 1692257 | 5.20 |
| 10 | 0.003925 | 0.003888 | 1701738 | 7.54 |
| 11 | 0.028651 | 0.122368 | 1653808 | 33.49 |
| 12 | 0.003663 | 0.003808 | 1697689 | 6.13 |
| 13 | 0.002932 | 0.002640 | 1622408 | 4.10 |
| 14 | 0.011891 | 0.004384 | 1706445 | 10.99 |
| 15 | 0.006460 | 0.003856 | 1755440 | 6.04 |
| 16 | 0.009760 | 0.004000 | 1707081 | 9.21 |
| 17 | 0.006078 | 0.004256 | 1710263 | 16.93 |
| 18 | 0.008078 | 0.004352 | 1679271 | 6.99 |
| 19 | 0.005667 | 0.004224 | 1723273 | 4.96 |
| 20 | 0.004274 | 0.003984 | 1739715 | 5.41 |

큐 근사치는 외부 관측의 송신 sequence와 수신 count 차이며 실제 pipe depth가 아니다(관측 횟수 45–47). 전체 run별 JSON/TSV와 원로그는 `raw/aa-analysis.json`, `raw/aa-runs.tsv`, `raw/histogram-aa-01`…`20`이다.

multi before 바이너리는 진단 커밋 `76a4d7850664c2cb32e8d35837dac45d9d2a4057`에서 같은 Core 후보로 재빌드했다. 제공된 `i4162/bin`에는 single 바이너리만 있어, 기존 multi 바이너리와의 직접 비교는 아니다. 각 셀 3회 모두 정상 완료했다.

| 패턴/전송/크기 | 버전 | 평균 ms (3회) | p95 ms (3회) | 처리량 msg/s (3회) | 관찰 |
|---|---|---|---|---|---|
| PUBSUB/tcp/256B | before | 440.192, 384.822, 3.233 | 787.004, 785.240, 7.876 | 1193097, 1208890, 1186586 | 3회 중 1회 저 latency |
| PUBSUB/tcp/256B | new | 956.935, 391.719, 115.509 | 1216.348, 729.809, 358.613 | 1021481, 1062845, 1112816 | 신규도 고 latency 범위 |
| DEALER_DEALER/tls/64B | before | 0.772, 0.619, 2.605 | 7.736, 4.108, 16.493 | 1671009, 2006871, 1682695 | 3회 표본 |
| DEALER_DEALER/tls/64B | new | 0.559, 0.240, 0.107 | 3.588, 1.221, 0.109 | 2049211, 1891533, 1419350 | 3회 표본 |

PUBSUB 신규 3회에서는 기존 저 latency 1회 수준이 나타나지 않아 bimodality 해소를 입증하지 못했다. 작은 표본이며 A/A PM 기준도 통과하지 않았다.

## 판정 위치

- percentile 저장·교체: 기존 single `bindings/c/perf/single/common/perf_single_latency.hpp:14`, multi `bindings/c/perf/multi/common/perf_multi_metrics.hpp:31`의 두 sampler에서 각각 판정(**2곳**); 현재 공통 bucket index·percentile 판정은 `bindings/c/perf/common/perf_latency_histogram.hpp:29`, `:80` 한 곳(**1곳**).
- 수신 건수: 기존 single `bindings/c/perf/single/common/perf_single_one_way.hpp:310` active count와 latency sampler count의 중복(**2곳**); 현재 `latency.count()`만 사용(**1곳**, `bindings/c/perf/single/common/perf_single_one_way.hpp:308`, `:348`; count 소유는 `perf_single_latency.hpp:18`).

## 바꾼 것

- `bindings/c/perf/common/perf_latency_histogram.hpp:9-128`: 7-bit sub-bucket의 고정 histogram, 전체 count/sum/min/max, merge, report-time percentile 대표값을 추가.
- `bindings/c/perf/single/common/perf_single_latency.hpp:14-33`, `bindings/c/perf/multi/common/perf_multi_metrics.hpp:31-67`: 같은 helper 사용. single 별도 수신 count와 single/multi reservoir, RNG, sample cap 제거.
- `bindings/c/perf/multi/src/perf_multi_router_router_matched_client.cpp:55-62`, `:460-483`: matched-client IPC를 bounded sample 배열에서 histogram bucket 집계로 변경. `perf_multi_weighted_latency.hpp` 및 사용하지 않는 echo-policy helper 제거.
- `doc/perf/PERF_POLICY.md:106-107`에 C perf histogram 정책 추가. 별도 영문 정책 쌍은 없다. single/multi 정책 문서의 cap 설명도 갱신. JSON field 이름·단위 유지; percentile은 reservoir subset 대신 모든 유효 수신 메시지 기준, mean은 전체 count/sum 기준. sample-cap 환경 변수는 `bindings/c/perf`에서 제거했으며 다른 구현에서 쓰이므로 문서 표에 남겼다.

## 검증

- 빌드: `/root/jobs/fx/cx/cpp-build-slot.sh` 경유, worktree 안 `.build-4162`에서 `make -B -j3` (`raw/build-attempt-02.log`), Core 후보 `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24`; single ROUTER_ROUTER, multi PUBSUB·DEALER_DEALER 및 matched-client와 self-test 빌드 성공. Core 로딩 경로 `raw/ldd-new-single.txt`. 첫 실패는 제거된 cap 인자 잔여 사용으로, `raw/build-attempt-01.log` 보존 후 수정했다.
- histogram correctness/self-test: `LD_LIBRARY_PATH=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib bin-new/perf_multi_metrics_test` 통과(`raw/perf-multi-metrics-test.log`). 1…10,000 값을 넣어 sorted exact p50/p99와 ≤1/128 상한, merge, count/sum/min/max, 0.5ns 확인.
- A/A: `flock -o /root/jobs/fx/cx/measure.lock taskset -c 0 python3 /root/jobs/fx/cx/v21/s033/f4162/observe.py --runs 20 --label histogram-aa`; 로그 `raw/aa-driver.log`, 집계 `raw/aa-analysis.json`. 20/20 정상 종료.
- Multi: 같은 measurement lock과 `taskset -c 8-15` 아래 `run-multi-aa.sh`; 로그 `raw/multi-aa-driver-02.log`, 실행별 `raw/multi-{PUBSUB,DEALER_DEALER}-{before,new}-{1..3}.log`. 각 셀 3회 완료.
- `git diff --check` 통과. 관련 self-test 및 요청한 측정만 실행했으며 전체 suite는 실행하지 않았다.

## 리팩토링 점검

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 2건 발견. 고친 항목: 가변 reservoir 비용·중복 수신 count, obsolete weighted-reservoir helper·미사용 echo-policy helper. 넘긴 항목: 없음.

## 남은 문제

A/A는 진단 당시 고 latency 모드를 재현하지 않았지만 PM의 5% median 안정성 기준은 실패했다. PUBSUB 신규 3회에서 bimodality 해소를 판정할 수 없다. 추가 설계 변경 없이 이 결과를 감독자 판정 대상으로 남긴다.
