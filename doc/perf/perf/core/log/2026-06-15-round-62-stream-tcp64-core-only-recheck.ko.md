# Round 62: STREAM tcp64 core-only 재점검

## 목표

- 사용자 정정 기준을 반영한다.
- 비교 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 목표 항목:
  `RESULT,current,MULTI_STREAM,tcp,64,throughput,400124.600`
- WS/WSS는 이 목표의 기준으로 보지 않는다.
- perf runner/client/server 성능 변경은 하지 않는다.

## 시작 상태

- core/perf source diff 없음.
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- `STREAM/tcp/64B` Auto-HWM은 baseline과 current 모두
  `server stream SNDHWM=128, RCVHWM=128`.

## Clean 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round62_stream_tcp64_clean_recheck`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_070011_round62_stream_tcp64_clean_recheck.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 337,223.8 ops/s`
  - baseline 대비 약 `-15.7%`
  - completion: `success=1`, `fail=0`, `status=complete`

## Tiny gather 진단

- hypothesis:
  64B STREAM 응답에서 encoder buffer copy를 피하면 개선될 수 있다.
- command:
  `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=64 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round62_stream_tcp64_tiny_gather64_diag`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_070050_round62_stream_tcp64_tiny_gather64_diag.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 329,074.4 ops/s`
- 판정:
  - clean 대비 악화다.
  - source 후보로 채택하지 않는다.

## Single write 진단

- hypothesis:
  speculative write loop가 작은 frame에서 CPU를 오래 점유해 역효과를 낼 수 있다.
- command:
  `ZLINK_ASIO_SINGLE_WRITE=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round62_stream_tcp64_single_write_diag`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_070104_round62_stream_tcp64_single_write_diag.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 332,420.0 ops/s`
- 판정:
  - clean 대비 개선이 없다.
  - source 후보로 채택하지 않는다.

## Read drain 진단

- hypothesis:
  read drain batch가 작으면 callback churn이 남을 수 있다.
- command:
  `ZLINK_ASIO_STREAM_READ_DRAIN_MAX_LOOPS=128 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round62_stream_tcp64_read_drain128_diag`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_070115_round62_stream_tcp64_read_drain128_diag.txt`
- result:
  - `MULTI_STREAM/tcp/64B = 330,467.4 ops/s`
- 판정:
  - 개선이 없다.
  - 현재 정책 코드에서 read drain loop 값은 env를 읽지 않는 상수라, source 후보 근거가 약하다.

## 누적 판정

- `STREAM/tcp/64B`의 current clean 값은 이번 재측정에서 `337,223.8 ops/s`다.
- baseline `400,124.6 ops/s` 대비 목표 미달은 맞다.
- 다만 round45/48/60의 진단과 이번 round 결과를 합치면, 큰 회귀 축은 core runtime이 아니라
  `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`의 `session_t::send_mutex` 직렬화다.
- `send_mutex` 제거 진단은 `379K~386K ops/s`까지 회복했지만, perf helper 성능 변경은 이번 작업의
  허용 범위가 아니므로 유지하지 않는다.
- 이번 round의 core-path 후보인 tiny gather, single write, read drain 변경은 모두 clean 대비 개선이
  없어 source 변경으로 채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 유지 source 변경 없음.
- 보안 하드닝 의미 변경 없음.
