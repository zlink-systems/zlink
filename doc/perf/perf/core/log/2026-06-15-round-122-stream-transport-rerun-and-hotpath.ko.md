# Round 122: STREAM transport 재측정과 hot path 후보 선정

## 이번 라운드 목표

- Round 121에서 부하가 섞인 뒤 실패한 `STREAM tls/ws/wss 64B`를 다시 분리 측정한다.
- 실패가 0개로 돌아오면 May26 corrected baseline 대비 남은 하락 폭을 다시 계산한다.
- STREAM core hot path에서 POSD-safe 후보가 있는지 코드 기준으로 하나만 고른다.

## 기준 report

- corrected smoke baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- problem report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- previous current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222403_round121_stream_current_same_window.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 건드리지 않는다.
- 현재 부하:
  - `uptime`: `load average: 1.02, 2.21, 2.56`
  - round121 때 보였던 Node sample high-CPU 프로세스는 종료됨.

## 가설

- 가설 1:
  Round 121의 `tls-only` 실패는 source 변경이 아니라 외부 부하 또는 직전 probe 후의 transient 상태다.
- 가설 2:
  STREAM 64B의 남은 하락은 socket routing보다 ASIO raw engine read/write target 또는 transport write path 영향이 크다.
- 가설 3:
  batch size 기본값 변경은 transport 실패를 만들 수 있으므로, 환경변수 probe가 실패한 값은 source 기본값 후보로 삼지 않는다.

## 먼저 검증할 가설

- 가설 1. 부하가 내려간 상태에서 `STREAM tls,ws,wss 64B`를 기본값으로 다시 실행한다.

## STREAM transport rerun

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round122_stream_tls_ws_wss_rerun`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222903_round122_stream_tls_ws_wss_rerun.txt`
- status: complete
- fail: 0
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- load_avg: `0.81 2.08 2.51`

| transport | current kops |
|-----------|--------------|
| tls | 203.851 |
| ws | 253.510 |
| wss | 174.368 |

### corrected May26 full 대비

| transport | May26 full kops | round122 current kops | delta |
|-----------|-----------------|-----------------------|-------|
| tls | 214.575 | 203.851 | -5.00% |
| ws | 251.311 | 253.510 | +0.87% |
| wss | 184.722 | 174.368 | -5.61% |

### corrected May26 smoke 대비

| transport | May26 smoke kops | round122 current kops | delta |
|-----------|------------------|-----------------------|-------|
| tls | 229.781 | 203.851 | -11.28% |
| ws | 263.180 | 253.510 | -3.67% |
| wss | 200.642 | 174.368 | -13.10% |

## STREAM ASIO 후보 재검토

- `core/src/runtime/engine/asio/asio_engine.cpp`
  - `process_output()`와 `prepare_output_buffer()`의 encoder-fill 중복 제거는 round112에서 처음 채택 후보였지만
    round117-118 A/B에서 안정 채택 기준을 만족하지 못해 미채택으로 전환됐다.
  - tiny gather threshold, initial target cap, packet complete-frame fast path도 이전 라운드에서 하락 또는 실패로 배제됐다.
- `core/src/runtime/transports/tls/ssl_transport.hpp`
  - TLS transport는 `supports_speculative_write() == false`다.
  - round80에서 TLS async write 방식 변경은 하락으로 폐기됐다.
- `core/src/runtime/transports/ws/ws_transport.hpp` / `core/src/runtime/transports/tls/wss_transport.hpp`
  - WS/WSS transport는 `supports_speculative_write() == false`이고 gather write도 false다.
  - WebSocket synchronous complete-frame write는 blocking 위험이 있어 round106에서 제외한 판단을 유지한다.

## 판단

- Round 121의 TLS failure는 낮은 부하에서 반복되지 않았다.
- corrected May26 full 기준으로 STREAM은 `tcp +1.20%`, `ws +0.87%`, `tls -5.00%`, `wss -5.61%`다.
  tcp/ws는 source 회귀로 보기 어렵고, tls/wss는 5% 안팎의 약한 하락이다.
- 이미 실패한 ASIO/STREAM 후보를 반복하지 않는다.
- 이번 라운드의 다음 검토 대상은 STREAM 전용 정책이 아니라 공통 `msg_t`/`pipe` enqueue-write hot path다.

## low-load all64 reduced full

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round122_lowload_all64_reduced_full`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`
- status: complete
- fail: 0
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- load_avg: `0.73 1.76 2.34`

### problem report 대비

- common 64B throughput 항목: 26개
- 전체 평균: `+7.05%`
- 전체 중앙값: `+4.76%`
- one-way 평균: `+8.03%`
- one-way 중앙값: `+2.06%`
- echo 평균: `+6.44%`
- echo 중앙값: `+5.75%`

| worst item | current | problem | delta |
|------------|---------|---------|-------|
| MULTI_PUBSUB/ws | 2197.126 kops | 2220.372 kops | -1.05% |
| MULTI_PUBSUB/tls | 2438.543 kops | 2446.708 kops | -0.33% |
| MULTI_PUBSUB/wss | 2687.476 kops | 2679.903 kops | +0.28% |
| MULTI_DEALER_DEALER/ws | 3180.134 kops | 3156.838 kops | +0.74% |
| MULTI_DEALER_ROUTER/wss | 377.369 kops | 371.400 kops | +1.61% |

### corrected May26 full 대비 worst

| item | current | May26 full | delta |
|------|---------|------------|-------|
| MULTI_SPOT/wss | 6100.115 kops | 6776.301 kops | -9.98% |
| MULTI_PUBSUB/tls | 2438.543 kops | 2623.065 kops | -7.03% |
| MULTI_SPOT_SENDSEND/tcp | 259.857 kops | 271.206 kops | -4.18% |
| MULTI_PUBSUB/wss | 2687.476 kops | 2760.571 kops | -2.65% |
| MULTI_SPOT_SENDSEND/tls | 249.922 kops | 254.010 kops | -1.61% |

### corrected May26 smoke 대비 worst

| item | current | May26 smoke | delta |
|------|---------|-------------|-------|
| MULTI_SPOT/tcp | 4148.820 kops | 4798.500 kops | -13.54% |
| MULTI_SPOT_SENDSEND/wss | 256.814 kops | 277.203 kops | -7.36% |
| MULTI_STREAM/wss | 189.070 kops | 200.642 kops | -5.77% |
| MULTI_PUBSUB/tls | 2438.543 kops | 2537.614 kops | -3.90% |
| MULTI_SPOT_SENDSEND/tcp | 259.857 kops | 264.042 kops | -1.59% |

## pipe/msg hot path triage

- `core/src/runtime/core/pipe.cpp`
  - `write_and_flush()`, `write_and_flush_no_recursive_hwm_check()`,
    `write_single_message_and_flush_no_recursive_hwm_check()`가 이미 있다.
  - HWM 확인과 flush는 `_out_sync`를 한 번 잡은 상태에서 처리된다.
  - STREAM 단일 메시지는 routing-id가 없는 final frame이라는 조건으로 `_msgs_written++`와 flush를 직접 수행한다.
- `core/src/runtime/core/ypipe.hpp`
  - `flush()`는 `_w == _f`이면 곧바로 true를 반환하고, 새 완료 메시지가 있을 때만 CAS를 수행한다.
- 판단:
  - 공통 pipe write path에는 지금 바로 넣을 수 있는 하락 없는 후보가 보이지 않는다.
  - low-load all64에서 `MULTI_DEALER_DEALER`가 3Mops 이상이고 echo tcp/ws도 400kops 이상이므로,
    공통 pipe enqueue/dequeue를 먼저 바꾸는 것은 현재 병목 증거가 약하다.

## 다음 후보

- 현재 problem report 대비 중앙값을 끌어올리려면 거의 개선이 없는 항목을 올려야 한다.
- 우선순위:
  1. `MULTI_PUBSUB/tls,ws` 64B: problem 대비 `-0.33%`, `-1.05%`라 전체 중앙값을 누르고 있다.
  2. `MULTI_SPOT_SENDSEND/tcp,wss` 64B: May26 기준 약한 하락이 있고 echo 평균에 영향이 있다.
  3. `MULTI_SPOT/wss` 64B: May26 full 대비 -9.98%지만 problem 대비로는 이미 큰 폭 개선된 항목이라 전체 목표 관점에서는 후순위다.
