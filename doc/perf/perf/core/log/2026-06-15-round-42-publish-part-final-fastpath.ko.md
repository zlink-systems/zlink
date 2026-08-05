# Round 42: publish_part FINAL fast path

- goal: 전체 64B one-way hot path에서 공개 helper `zlink_publish_part()`의
  single-part publish 비용을 줄인다.
- 완료 기준:
  - core source 변경 뒤 `cmake --build core/build -j$(nproc)` 통과
  - PUB/SUB와 helper 관련 core test 통과
  - `MULTI_PUBSUB` 64B targeted perf에서 문제 report 대비 반복 개선 확인
  - 효과가 없으면 변경 원복
- 기준 commit: `72d893595`
- 시작 git status:
  - `core/src`, `core/include`, `core/tests`, `bindings/c/perf` source diff 없음
  - perf 로그 파일 untracked 다수 존재
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 실패 0개 64B sweep:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`

## 64B 기준 수치

- 문제 report 대비 current64 common 64B:
  - n=26
  - mean `+0.91%`
  - median `+1.12%`
- 문제 report 대비 current64 one-way:
  - mean `-5.78%`
  - median `-5.31%`
- current64 vs problem one-way worst:
  - `MULTI_SPOT/tcp/64`: `-15.91%`
  - `MULTI_SPOT/tls/64`: `-9.35%`
  - `MULTI_PUBSUB/tls/64`: `-7.12%`
  - `MULTI_DEALER_DEALER/ws/64`: `-5.62%`
  - `MULTI_DEALER_DEALER/tcp/64`: `-5.33%`

## 가설

- 가설 1: `zlink_publish_part()`는 1-part FINAL publish에서도 `submit_simple_part()`를
  거치며 per-handle send sequence lookup, spec 생성, scoped send state 처리를 수행한다.
  SPOT publish에는 이미 FINAL-only fast path가 있으므로 PUB/XPUB helper에도 같은 성격의
  fast path를 두면 실제 공개 helper API와 `MULTI_PUBSUB` 64B one-way가 함께 개선될 수 있다.
- 가설 2: PUB/SUB matching 또는 pipe fanout LMSG refcount 비용이 더 큰 병목이다. 이 경우
  public helper fast path의 효과는 5% 미만이거나 noise에 묻힌다.
- 선택한 가설: 가설 1.
- 이유: perf helper는 `perf_zlink_publish_parts()`에서 `zlink_publish_part()`를 호출한다.
  이 경로는 perf 전용이 아니라 공개 helper substrate API이며, SPOT에는 이미 동일한
  FINAL-only 우회가 있어 계약상 설계 방향도 맞다.

## 읽은 코드

- `bindings/c/perf/common/perf_zlink_part_helpers.hpp`:
  `perf_zlink_publish_parts()`가 각 part를 `zlink_publish_part()`로 보낸다.
- `core/src/api/socket/socket_message_send_api.cpp`:
  `zlink_publish_part()`는 FINAL-only 1-part도 `submit_simple_part()`를 통과한다.
- `core/src/api/spot/core/service_spot_api.cpp`:
  `zlink_spot_publish_part()`는 FINAL-only이고 send sequence가 없으면
  `spot_publish_no_sequence_check()`로 바로 보낸다.
- `core/src/runtime/core/multipart_send_txn.cpp`:
  `logical_multipart_publish()`는 public send scope 아래 topic prefix와 payload parts를 보낸다.

## 변경 계획

- `zlink_publish_part()`에 FINAL-only/no-active-sequence fast path를 추가한다.
- `part_flag` 검증은 기존 `submit_simple_part()` 위임과 같은 오류 계약을 유지하기 위해
  함수 초입에서 수행한다.
- direct publish 실패 시 아직 닫히지 않은 part는 consume해서 기존 helper 계약을 유지한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: decoder/message/send guard.
- 보안 의미를 유지한 근거: NULL/invalid flag/socket type 검증과 실패 시 part consume 계약을 유지한다.
- 추가로 실행한 회귀 테스트:
  `ctest --test-dir core/build -R 'test_pubsub$|test_pubsub_filter_xpub|test_xpub_nodrop|test_helper_send_part_basic|test_helper_more_bad_send|test_helper_interleave|test_helper_ownership|test_multi_socket_contract_regressions|test_backpressure_oneway_matrix|test_backpressure_matrix|unittest_service_mode_policy' --output-on-failure`
  통과, 17/17.

## 검증 결과

- build:
  `cmake --build core/build -j$(nproc)` 통과.
- targeted perf:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round42_publish_part_final_fastpath_pubsub64`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_024814_round42_publish_part_final_fastpath_pubsub64.txt`
- 결과:
  - `MULTI_PUBSUB/tcp/64`: `2,472,407.6 ops/s`
  - `MULTI_PUBSUB/tls/64`: `2,291,940.2 ops/s`
  - `MULTI_PUBSUB/ws/64`: `2,176,075.0 ops/s`
  - `MULTI_PUBSUB/wss/64`: `2,509,176.2 ops/s`
- 비교:
  - problem: tcp `2,628,104.8`, tls `2,446,707.8`, ws `2,220,372.2`,
    wss `2,679,903.2`
  - round29 current64: tcp `2,599,897.6`, tls `2,272,469.6`, ws `2,216,451.0`,
    wss `2,556,827.4`
- 판정:
  - 10% 개선 없음.
  - tcp/ws/wss는 round29보다 낮고, tls만 `+0.86%` 수준이다.
  - load_avg가 높았지만 개선 신호가 없으므로 후보를 유지하지 않는다.
- 조치: `zlink_publish_part()` fast path 후보 원복.

## 다음 후보

- `zlink_publish_part()` helper scaffolding은 64B PUBSUB 병목의 주 원인으로 확인되지 않았다.
- 다음 후보는 PUB/SUB matching보다 아래의 pipe/session wakeup 또는 SPOT fanout의 반복
  current 결손을 다시 확인해야 한다.

## SPOT tcp 반복 확인

- 이유: round29 current64 기준에서 문제 report 대비 가장 큰 one-way 결손은
  `MULTI_SPOT/tcp/64` `-15.91%`였다.
- 사전 부하 확인:
  - `uptime`: load average `28.22 15.96 11.63`
  - benchmark 프로세스 없음.
  - 60초 대기 후 load average `8.80 12.63 10.79`.
- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round42_spot_tcp64_clean_repeat`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025349_round42_spot_tcp64_clean_repeat.txt`
- 결과:
  - `MULTI_SPOT/tcp/64`: `3,616,080.4 ops/s`
- 비교:
  - problem `3,896,078.6` 대비 `-7.19%`
  - round29 current64 `3,276,035.2` 대비 `+10.38%`
- 판정:
  - round29의 `-15.91%` 결손은 clean standalone repeat에서 재현되지 않았다.
  - 현재 문제 report 대비 10% 이상 반복 결손이 아니므로, 이번 라운드에서는
    `MULTI_SPOT/tcp/64`를 source 수정 후보로 삼지 않는다.
