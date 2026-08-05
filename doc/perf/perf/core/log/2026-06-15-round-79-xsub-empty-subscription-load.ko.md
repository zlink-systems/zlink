# Round 79 - XSUB empty-subscription load ordering

## 목표

- `PUBSUB/tls/64B`의 반복 하락을 줄일 수 있는, 상태를 추가하지 않는 작은 core 후보만 검증한다.
- 완료 기준: focused `PUBSUB/tls/64B`가 round78 current `2,293,853.4 ops/s` 대비 의미 있게 상승하고, 관련 PUBSUB 테스트가 통과한다.
- 후보 효과가 작거나 하락하면 되돌린다.

## 후보

- `xsub_t::match()`의 바깥 `_has_empty_subscription` load를 acquire에서 relaxed로 낮춘다.
- 이 값은 보호된 trie 데이터를 읽기 위한 동기화 지점이 아니라, 빈 구독이면 매칭을 즉시 true로 끝내는 힌트다.
- 새 상태나 캐시를 추가하지 않으므로 POSD 관점에서 모듈 경계를 넓히지 않는다.

## 위험 검토

- 빈 구독의 true/false 전환은 기존에도 lock-free fast path와 잠금 내부 재확인 사이의 경쟁을 허용한다.
- non-empty 구독 경로는 false를 읽은 뒤 기존처럼 `_subscriptions_mu`를 잡고 trie를 확인한다.
- inverted matching에서는 기존처럼 이 fast path를 쓰지 않는다.

## 검증

- build: `cmake --build core/build -j$(nproc)` 통과.
- tests: `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions|transport_matrix)$'`
  - 결과: 5/5 통과.
- perf:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round79_xsub_empty_relaxed_pubsub_tls`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_134722_round79_xsub_empty_relaxed_pubsub_tls.txt`
  - load_avg: `4.46 9.28 10.37`
  - result: `MULTI_PUBSUB/tls/64B = 2,298,855.0 ops/s`

## 판정

- round78 current `2,293,853.4 ops/s` 대비 약 `+0.22%`다.
- May26 smoke `2,537,614.0 ops/s`, May26 full `2,623,065.0 ops/s`, May26 replay `2,460,474.8 ops/s` 대비 하락을 의미 있게 줄이지 못했다.
- 효과가 노이즈 수준이므로 변경을 되돌렸다.
