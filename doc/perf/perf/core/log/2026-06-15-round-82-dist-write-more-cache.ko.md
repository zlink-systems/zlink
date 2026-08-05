# Round 82 - dist write more flag reuse

## 이번 라운드 목표

- one-way fanout hot path에서 새 상태를 추가하지 않는 작은 중복 제거 후보를 검증한다.
- 완료 기준: focused one-way 64B set에서 하락 항목 없이 의미 있는 상승이 있고, 관련 core tests가 통과한다.
- 효과가 없거나 하락하면 변경을 되돌린다.

## 기준 report

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- current reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
- current `PUBSUB/tls` low-load:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_134041_round78_current_pubsub_tls_lowload_recheck.txt`

## 병목 가설

1. `dist_t` fanout loop가 각 matching pipe마다 `msg_->flags()`를 다시 읽는다.
   - `PUBSUB`는 topic part와 payload part를 100 client pipe에 반복 배포하므로 작은 중복도 누적될 수 있다.
2. multipart 여부는 message 하나의 배포 동안 변하지 않는다.
   - helper에 이미 계산한 값을 넘기면 새 상태 없이 반복 분기를 줄일 수 있다.
3. 효과가 있더라도 매우 작을 수 있다.
   - 하락 항목이 있거나 노이즈 수준이면 남기지 않는다.

## 후보

- `dist_t::write_at()`에 `msg_more_`를 인자로 넘겨 pipe마다 `msg_->flags()`를 다시 읽지 않게 한다.
- 새 캐시, workload 전용 분기, perf 전용 shortcut은 추가하지 않는다.

## POSD 검토

- 인터페이스는 `dist_t` private helper 내부에만 바뀐다.
- helper가 필요한 사실인 “현재 part가 more인지”를 호출자가 한 번 계산해 전달하므로 숨겨진 상태를 늘리지 않는다.
- 배포 정책과 pipe write 정책은 기존 위치에 그대로 둔다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions|backpressure_oneway_matrix|backpressure_matrix|transport_matrix|spot_pubsub_scenario)$'`
  - 결과: 6/6 통과.

## 검증 결과

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- perf command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round82_dist_more_pubsub_focused`
- runner runtime:
  - `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_141449_round82_dist_more_pubsub_focused.txt`
- load_avg:
  - `5.85 11.33 12.36`
- results:
  - `MULTI_PUBSUB/tcp/64B = 2,448,720.8 ops/s`
  - `MULTI_PUBSUB/tls/64B = 2,288,764.0 ops/s`
  - `MULTI_PUBSUB/wss/64B = 2,517,795.2 ops/s`

## 판정

- round74 standalone 대비:
  - `tcp`: `2,493,177.8 -> 2,448,720.8`, 약 `-1.78%`
  - `tls`: `2,274,859.0 -> 2,288,764.0`, 약 `+0.61%`
  - `wss`: `2,504,005.6 -> 2,517,795.2`, 약 `+0.55%`
- 상승은 모두 1% 안팎이고 `tcp`는 하락했다.
- 후보 효과가 노이즈 수준이고 하락 항목이 있으므로 변경을 되돌렸다.
