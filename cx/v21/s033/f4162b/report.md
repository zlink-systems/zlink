# #4162 후속 — bench gate rules

## 규칙 위치와 판정 소유

| 항목 | 변경 전 위치·판정 | 변경 후 위치·판정 |
|---|---|---|
| A/A ±5% | 저장소 정책에는 없고 이전 작업 보고서가 평균 기준을 사용했다(`/root/jobs/fx/cx/v21/s033/f4162/report.md:5`). `perf_regression_gate.py`의 5%는 셀별 성능 한계이며 A/A 기준이 아니다(`bindings/c/perf/perf_regression_gate.py:361-363`). | 공통 정책이 실행별 p50 중앙값 대비 ±5%로 정의한다(`doc/perf/PERF_POLICY.md:81-83`). 실제 판정 스크립트는 p50의 중앙값과 상대 편차를 비교한다(`/root/jobs/fx/cx/v21/s033/f4162b/raw/analyze-aa-p50.py:7-12`). |
| PUBSUB latency window | 기존 공통 helper는 송신 timestamp 기준 고정 0.5초 settle을 썼다(`pre-change commit b9faa64cba87:bindings/c/perf/multi/common/perf_multi_measurement.hpp:30-35`). PUBSUB client가 별도 sample 시작 시각을 정하고(`pre-change commit b9faa64cba87:bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:163,207-230`), server도 epoch + settle로 phase를 전환했다(`pre-change commit b9faa64cba87:bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:252-274`). 기존 문서와 실행 도움말도 active 동시 측정·settle을 기술했다(`pre-change commit b9faa64cba87:doc/perf/PERF_MULTI_TEST_POLICY.md:29-31,714`; `pre-change commit b9faa64cba87:bindings/c/perf/run_benchmarks_multi.sh:574-575`). | catch-up 판정은 `catch_up_tracker_t` 한 곳이 소유한다(`bindings/c/perf/multi/common/perf_multi_measurement.hpp:13-50`). 수신율은 0.5초 구간 기준 목표율의 95% 이상, backlog는 직전 구간보다 증가하지 않아야 하며 첫 구간은 기준값으로만 쓴다(`doc/perf/PERF_POLICY.md:84-93`). Client는 판정 후 `PHASE_CATCHUP`을 보내고 해당 시각 이후 표본만 누적하며 deadline 실패는 latency invalid로 출력한다(`bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:246-280,310-314`). Publisher는 catch-up 신호 뒤 latency 표본 phase로 바꾸고, deadline을 지키지 못하면 latency duration까지만 더 보낸다(`bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:252-298`). |

**Decision sites 전/후:** PUBSUB은 기존 client sample-start와 server timestamp phase 전환 두 곳(2)이 latency 시작을 따로 결정했다(`pre-change commit b9faa64cba87:bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:163,207-230`; `pre-change commit b9faa64cba87:bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:252-274`). 변경 후 catch-up 판정은 tracker 한 곳(1)이며 client는 이를 통지하고 server는 통지를 따른다(`bindings/c/perf/multi/common/perf_multi_measurement.hpp:16-47`; `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:246-254`; `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:263-298`). A/A 판정은 이전 평균 기반 외부 보고에서 p50 기반 단일 comparator로 바뀌었다(`/root/jobs/fx/cx/v21/s033/f4162/report.md:5`; `/root/jobs/fx/cx/v21/s033/f4162b/raw/analyze-aa-p50.py:7-12`).

## 변경 및 검증

- 정책은 한국어로 갱신했고, multi 정책·실행 도움말의 active/settle 설명을 PUBSUB catch-up 규칙에 맞췄다(`doc/perf/PERF_POLICY.md:79-93`; `doc/perf/PERF_MULTI_TEST_POLICY.md:29-32,604-605,710-739`; `bindings/c/perf/run_benchmarks_multi.sh:574-576`; `bindings/c/perf/README.md:172-190`).
- Invalid latency RESULT는 사유를 문자열로 보존하고 throughput/bandwidth는 수치로 출력한다(`bindings/c/perf/multi/common/perf_multi_metrics.hpp:144-160`; aggregator 처리 `bindings/c/perf/run_comparison.py:857-894,4264-4277`). Catch-up 후 추가 latency duration까지 포함하도록 runner timeout도 맞췄다(`bindings/c/perf/run_comparison.py:961-964`).
- Unit coverage는 catch-up 즉시/지연/미도달과 invalid RESULT를 포함한다(`bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp:168-202`). Aggregator test는 invalid 사유와 `PHASE_CATCHUP` 전달을 확인한다(`bindings/c/perf/single/tests/test_multi_run_comparison_policy.py:104-123,166-172`). p50 비교기는 ±5% 경계 통과와 초과 실패를 확인한다(`/root/jobs/fx/cx/v21/s033/f4162b/raw/test-aa-p50.py:10-19`).
- Build는 build-slot helper와 Core prefix `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24`로 완료했고 전체 대상이 100%까지 빌드됐다(`/root/jobs/fx/cx/v21/s033/f4162b/raw/build-retry-01.log:55-62`). C++ metrics self-test도 0으로 종료했다(`bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp:168-202,721-727`; 실행 로그 `/root/jobs/fx/cx/v21/s033/f4162b/raw/perf-multi-metrics-test-rerun.log`). Python aggregator 46 tests 통과(`/root/jobs/fx/cx/v21/s033/f4162b/raw/aggregator-test-final-rerun.log:61-65`); p50 comparator 2 tests 통과(`/root/jobs/fx/cx/v21/s033/f4162b/raw/aa-comparator-test-final.log:1-5`). `git diff --check`, shell 문법 검사, Python compile도 통과했다(변경 스크립트 `bindings/c/perf/run_comparison.py:857-894,961-964`; shell 도움말 `bindings/c/perf/run_benchmarks_multi.sh:568-580`).

### PUBSUB tcp 256B · 5회

실행별 원시 로그와 상세 통계는 `/root/jobs/fx/cx/v21/s033/f4162b/raw/pubsub-01.log`…`pubsub-05.log`; 집계 행은 아래 인용에 있다.

| 회 | catch-up 초 | 유효 | latency 평균 / p50 / p95 ms | throughput msg/s | 원시 집계 |
|---:|---:|:---:|---:|---:|---|
| 1 | 1.013677 | 예 | 333.913 / 352.322 / 708.837 | 1,152,569.6 | `raw/pubsub-runs.tsv:2` |
| 2 | 1.013118 | 예 | 404.850 / 434.110 / 796.918 | 1,212,346.4 | `raw/pubsub-runs.tsv:3` |
| 3 | 1.497055 | 예 | 2.533 / 2.441 / 3.883 | 1,217,256.6 | `raw/pubsub-runs.tsv:4` |
| 4 | 미도달 | 아니요: `receiver_not_caught_up_by_deadline` | invalid / invalid / invalid | 1,247,367.4 | `raw/pubsub-runs.tsv:5`; invalid RESULT와 throughput은 `raw/pubsub-04.log:107-110` |
| 5 | 1.009680 | 예 | 454.427 / 480.248 / 843.055 | 1,212,045.2 | `raw/pubsub-runs.tsv:6` |

4회는 유효, 1회는 latency invalid였고 throughput은 5회 모두 기록됐다(`raw/pubsub-runs.tsv:2-6`). 유효 latency 편차가 커서 이를 감추지 않고 원시 값 그대로 보고한다(`raw/pubsub-runs.tsv:2-6`).

### Single inproc A/A · 20회

판정은 p50 기준 **실패**: p50 중앙값 0.002776 ms이며 20개 중 7개만 ±5% 이내였다(`/root/jobs/fx/cx/v21/s033/f4162b/raw/aa-p50-analysis.log:1-6`; 전 회차 수치는 아래 집계 행).

| 회 | p50 ms | 평균 ms | p95 ms | 처리량 msg/s | 원시 집계 |
|---:|---:|---:|---:|---:|---|
| 1 | 0.002656 | 0.004456 | 0.003920 | 1,725,414.8 | `raw/aa-p50-runs.tsv:2` |
| 2 | 0.002960 | 0.005690 | 0.004192 | 1,687,418.8 | `raw/aa-p50-runs.tsv:3` |
| 3 | 0.002768 | 0.003544 | 0.003840 | 1,701,797.4 | `raw/aa-p50-runs.tsv:4` |
| 4 | 0.002768 | 0.004634 | 0.003824 | 1,747,178.4 | `raw/aa-p50-runs.tsv:5` |
| 5 | 0.003008 | 0.004098 | 0.003984 | 1,716,360.0 | `raw/aa-p50-runs.tsv:6` |
| 6 | 0.002608 | 0.005404 | 0.004192 | 1,702,682.2 | `raw/aa-p50-runs.tsv:7` |
| 7 | 0.002736 | 0.006644 | 0.003904 | 1,771,730.8 | `raw/aa-p50-runs.tsv:8` |
| 8 | 0.002784 | 0.005064 | 0.004000 | 1,767,151.0 | `raw/aa-p50-runs.tsv:9` |
| 9 | 0.002960 | 0.003961 | 0.003840 | 1,683,010.4 | `raw/aa-p50-runs.tsv:10` |
| 10 | 0.003008 | 0.032331 | 0.006368 | 1,739,245.4 | `raw/aa-p50-runs.tsv:11` |
| 11 | 0.002752 | 0.005363 | 0.003872 | 1,710,094.6 | `raw/aa-p50-runs.tsv:12` |
| 12 | 0.002496 | 0.014535 | 0.003792 | 1,719,889.0 | `raw/aa-p50-runs.tsv:13` |
| 13 | 0.002320 | 0.004445 | 0.003520 | 1,777,381.4 | `raw/aa-p50-runs.tsv:14` |
| 14 | 0.003072 | 0.007534 | 0.004960 | 1,662,735.0 | `raw/aa-p50-runs.tsv:15` |
| 15 | 0.002560 | 0.003696 | 0.003792 | 1,732,934.4 | `raw/aa-p50-runs.tsv:16` |
| 16 | 0.002816 | 0.003135 | 0.003856 | 1,692,141.8 | `raw/aa-p50-runs.tsv:17` |
| 17 | 0.002400 | 0.002806 | 0.003712 | 1,698,830.8 | `raw/aa-p50-runs.tsv:18` |
| 18 | 0.003024 | 0.004397 | 0.004224 | 1,745,735.6 | `raw/aa-p50-runs.tsv:19` |
| 19 | 0.003008 | 0.006903 | 0.004480 | 1,750,587.4 | `raw/aa-p50-runs.tsv:20` |
| 20 | 0.003088 | 0.037349 | 0.074752 | 1,723,102.8 | `raw/aa-p50-runs.tsv:21` |

## 완료

`git diff --check` 통과 후 `7a65e1f013`으로 로컬 commit했다. build 디렉터리는 정리했으며 모든 측정 원시 파일은 `/root/jobs/fx/cx/v21/s033/f4162b/raw/`에 보존했다(`git status` clean; commit `7a65e1f013`).

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견; 고친 항목 0건, 이관 0건(이번 변경 경로 `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:246-314`, `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:252-314`).
