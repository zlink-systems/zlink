# Round 124: routed send queue room check 중복 제거

## 이번 라운드 목표

- `SPOT_SENDSEND tcp/wss 64B`의 남은 약한 하락을 기준으로, Spot routed send queue hot path에서
  POSD-safe 후보를 하나만 적용한다.
- 완료 기준:
  - core build 통과.
  - spot/request-reply 관련 CTest 통과.
  - `SPOT_SENDSEND tcp,tls,wss 64B` targeted perf에서 실패 0개.
  - 하락 항목이 있거나 효과가 없으면 source 변경을 되돌린다.

## 기준 report

- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- corrected smoke baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- current low-load all64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`

## 시작 상태

- `core/src`, `core/include`, `core/tests`: source diff 없음.
- perf runner/client/server는 수정하지 않는다.
- 시작 부하:
  - `load average: 1.58, 1.95, 2.38`

## 병목 가설

- 가설 1:
  `enqueue_runtime_routed_send()`는 steady-state hot path에서 같은 queue room 판정을 lock 안에서 두 번 수행한다.
  중복 판정을 한 번으로 줄이면 SPOT routed send enqueue 비용이 아주 작게 줄 수 있다.
- 가설 2:
  `std::vector<zlink_msg_t>`를 stack-backed 타입으로 바꾸는 후보는 queue ownership 경계까지 새 타입을 퍼뜨려
  복잡도를 늘린다. 이번 라운드에서는 제외한다.
- 가설 3:
  request/reply additional fast path는 round95/105에서 wss 하락 또는 근거 부족으로 배제됐다.
  같은 후보를 반복하지 않는다.

## 먼저 검증할 가설

- 가설 1. `enqueue_runtime_routed_send()`에서 첫 `routed_queue_has_room()` 결과를 캐시해
  backpressure marking과 nonblocking failure 판정에 함께 쓴다.

## POSD 검토

- 큐 정책, HWM 의미, timeout 의미, signaler 의미는 바꾸지 않는다.
- 새 API나 새 자료구조를 만들지 않는다.
- 중복 조건 평가를 제거해 queue admission 정의를 한 곳에서 읽히게 한다.

## 보안 하드닝 보존 확인

- WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.

## 적용한 변경

- `core/src/api/spot/request_reply/service_spot_request_reply_routed_delivery.cpp`
  - `enqueue_runtime_routed_send()`에서 같은 lock, 같은 `hwm`, 같은 `byte_limit`, 같은 `entry_bytes`로
    호출하던 첫 `routed_queue_has_room()` 결과를 `has_room`에 저장한다.
  - 이 값을 backpressure marking과 nonblocking failure 판정에 함께 사용한다.
  - blocking wait loop의 재검사는 대기 중 queue 상태가 바뀔 수 있으므로 그대로 둔다.

## 기능 검증

- `cmake --build core/build -j$(nproc)`
  - 통과.
- `ctest --test-dir core/build --output-on-failure -R 'spot|zmp_request_reply|request_reply'`
  - 38/38 통과.

## 성능 검증

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round124_routed_send_queue_room_cache`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_231231_round124_routed_send_queue_room_cache.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,8.70 9.28 5.43`
- 결과, round122 current 대비:
  - tcp: `264038.0` vs `259856.6` = `+1.61%`
  - tls: `250651.8` vs `249922.4` = `+0.29%`
  - wss: `254946.8` vs `256813.8` = `-0.73%`

부하가 높았고 wss 하락이 1% 미만이라 한 번 더 재측정했다.

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round124_routed_send_queue_room_cache_rerun`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_231522_round124_routed_send_queue_room_cache_rerun.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,2.74 6.25 4.92`
- 결과, round122 current 대비:
  - tcp: `266793.8` vs `259856.6` = `+2.67%`
  - tls: `237182.6` vs `249922.4` = `-5.10%`
  - wss: `255702.0` vs `256813.8` = `-0.43%`

## 판정

- rejected.
- POSD 관점의 변경 폭은 작지만, 반복 측정에서 tls/wss 하락 항목이 남았다.
- source 변경은 되돌렸다.
- perf runner/client/server는 수정하지 않았다.
