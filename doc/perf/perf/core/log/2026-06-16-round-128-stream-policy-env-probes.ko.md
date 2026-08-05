# Round 128: STREAM policy env probes

## 목표

- STREAM/tcp 64B 목표인 400kops에 가까워질 수 있는 ASIO/auto-HWM 정책 후보를 source 변경 없이 확인한다.
- env probe에서 명확한 개선이 없으면 source 기본값으로 옮기지 않는다.

## 시작 상태

- source diff는 round125 `zlink_spot_send_spot_part()` FINAL-only fast path만 남아 있다.
- round127 STREAM routing-id decode-once 최소 후보는 같은 창 A/B에서 하락해 원복했다.

## Probe 1: ASIO STREAM initial target cap 8192

- 명령:
  `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=8192 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round128_stream_initial_target_cap_8192_tcp_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_002940_round128_stream_initial_target_cap_8192_tcp_probe.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,1.26 5.41 5.60`
- result:
  `MULTI_STREAM/tcp/64 = 293797.8`
- 판단:
  default 저부하 재측정과 May26 기준보다 낮다. source 후보로 올리지 않는다.

## Probe 2: auto-HWM throughput profile

- 명령:
  `PERF_CTX_AUTO_HWM_PROFILE=throughput PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round128_stream_auto_hwm_throughput_tcp_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_003049_round128_stream_auto_hwm_throughput_tcp_probe.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,0.87 4.48 5.27`
- auto-HWM:
  `SNDHWM=512`, `RCVHWM=512`
- result:
  `MULTI_STREAM/tcp/64 = 304533.8`

## Default A/B after HWM probe

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 7 --connect-ready-timeout-ms 5000 --results-tag round128_stream_default_tcp_after_hwm_probe`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_003148_round128_stream_default_tcp_after_hwm_probe.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,0.69 3.84 5.01`
- auto-HWM:
  `SNDHWM=128`, `RCVHWM=128`
- result:
  `MULTI_STREAM/tcp/64 = 305438.4`

## 판단

- `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=8192`는 개선 신호가 없다.
- `PERF_CTX_AUTO_HWM_PROFILE=throughput`도 default보다 낮아, STREAM 기본 auto-HWM 정책 변경 근거가 없다.
- 두 후보 모두 source 변경 없이 반려한다.
