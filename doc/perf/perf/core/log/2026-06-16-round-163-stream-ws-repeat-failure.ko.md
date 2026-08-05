# Round 163: STREAM ws 반복 실패 분리

## goal

- full multi perf 실패 0개 목표를 막는 `MULTI_STREAM ws/wss` 반복 실패를 먼저 분리한다.
- 완료 기준:
  - 실패가 core runtime 수명주기 문제인지, 실행 환경/runner 계열 문제인지 근거로 구분한다.
  - core 수정 후보가 확인되면 최소 변경 뒤 build, 관련 test, targeted perf를 통과한다.
  - core 수정 후보가 없으면 변경 없이 근거를 남기고 다음 라운드 후보를 정한다.

## 기준 report

- 과거 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 현재 문제:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최종 저장 multi full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260616_081700_final_retained_spot_multi_full_baseline_20260616.txt`
- 직전 정리:
  `doc/plan/perf/core/log/2026-06-16-round-162-final-full-baseline-and-stream-ws-triage.ko.md`

## 시작 상태

- 현재 HEAD: `aaf17b588`
- 직전 perf 개선 commit `3e583d101` 뒤에 사용자/외부 변경이 더 올라온 상태다.
- core source diff: 없음.
- perf runner/client/server diff: 없음.
- baseline 저장 파일과 round 162/163 로그는 아직 untracked 상태다.

## 실패 증거

multi full baseline:

- status: `partial`
- success/fail: `180/12`
- expected/result lines: `960/900`
- 실패: `MULTI_STREAM current ws 64B..131072B`
- partial report에서 나온 `MULTI_STREAM` 64B:
  - tcp `240002.200`
  - tls `186733.800`
  - ws/wss 결과 누락

focused 재현:

- standalone `STREAM ws`: complete, `ws/64B 234341.800`
- standalone `STREAM tcp,tls,ws,wss`: complete, `ws/64B 239419.200`, `wss/64B 174604.400`
- `SPOT_SENDSEND,STREAM` debug:
  - partial
  - `case_failed size=1024 connect_ok=0 connect_fail=10000 send_error=0 recv_error=10000`

## 실행 환경 확인

- `ulimit -n`: `1048576`
- `/proc/sys/net/ipv4/ip_local_port_range`: `1024 65535`
- `/proc/sys/net/ipv4/tcp_tw_reuse`: `1`
- 현재 `TIME_WAIT`와 `ESTABLISHED` 수는 낮다.
- 실패 뒤 남은 `perf_multi_stream_server`/`perf_stream_client` 프로세스는 확인되지 않았다.

## 병목/실패 가설

1. WebSocket STREAM 서버에서 이전 size wave의 세션/handshake 종료가 늦어지고,
   새 wave의 accept/handshake가 backlog 또는 내부 세션 상태 때문에 전부 실패한다.
2. `asio_raw_engine_t + ws_transport_t` 조합의 종료 경로가 TCP/TLS와 달라서,
   반복 실행 뒤 accept한 fd 또는 Beast WebSocket 상태가 더 오래 살아 새 연결 실패를 만든다.
3. OS ephemeral port나 fd 고갈은 현재 관찰값으로는 약하다. 다만 10000 connection wave라서
   재현 직후 socket state 변화는 추가 확인이 필요하다.

## 먼저 검증할 가설

- `SPOT_SENDSEND,STREAM ws` focused 반복에서 실패 시점 전후의 socket state와 server/client
  종료 상태를 관찰한다.
- core 쪽 후보는 `asio_raw_engine_t`, `asio_engine_t`, `ws_transport_t`,
  `asio_ws_listener_t`의 accept/handshake/terminate 경로로 제한한다.

## 확인한 core 경로

읽은 파일:

- `core/src/runtime/transports/ws/asio_ws_listener.cpp`
- `core/src/runtime/transports/ws/ws_transport.cpp`
- `core/src/runtime/engine/asio/asio_raw_engine.cpp`
- `core/src/runtime/engine/asio/asio_engine.cpp`
- `core/src/runtime/core/session_base.cpp`

확인 내용:

- STREAM ws/wss bind 쪽은 `asio_ws_listener_t`가 TCP accept 뒤
  `asio_raw_engine_t + ws_transport_t` 조합을 만든다.
- raw engine은 transport가 handshake를 요구하면 `start_transport_handshake()`에서
  server-side `ws_transport_t::async_handshake()`를 호출한다.
- handshake 실패나 transport error는 `asio_engine_t::error()`에서 session error,
  `unplug()`, transport close, queued delete로 이어진다.
- TCP/TLS와 다른 즉시 obvious fd leak 증거는 코드 읽기만으로 확인되지 않았다.

## focused 재현 1: 성공 뒤 TIME_WAIT 폭증

명령:

```bash
PERF_DEBUG=1 PERF_FAIL_FAST=1 PERF_CAPTURE_MAX_BYTES=16777216 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern SPOT_SENDSEND,STREAM --transports ws \
  --duration 5 --runs 1 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round163_spotsendsend_stream_ws_repro
```

결과:

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_121750_round163_spotsendsend_stream_ws_repro.txt`
- status: `complete`
- `MULTI_STREAM/ws/64B`: `234593.400`
- 종료 직후 socket state:
  - `TIME_WAIT`: `60065`
  - `ESTABLISHED`: `35`
- 종료 뒤 남은 perf server/client 프로세스:
  - 없음

해석:

- `STREAM ws` 10000개 client를 size 6개로 반복하면 한 run만으로도 loopback
  `TIME_WAIT`가 약 60000개까지 쌓인다.
- 현재 ephemeral port range는 `1024..65535`로 약 64512개다.
- 이 상태에서는 다음 10000 connection wave를 열 공간이 거의 남지 않는다.

## focused 재현 2: 바로 이어서 실패

명령:

```bash
PERF_DEBUG=1 PERF_FAIL_FAST=1 PERF_CAPTURE_MAX_BYTES=16777216 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports ws \
  --duration 5 --runs 1 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round163_stream_ws_after_timewait_pressure
```

결과:

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_121934_round163_stream_ws_after_timewait_pressure.txt`
- status: `partial`
- `STREAM ws 64B`는 통과했지만 256B에서 실패했다.
- failure detail:
  `case_failed size=256 connect_ok=0 connect_fail=10000 send_error=0 recv_error=10000`
- 종료 직후 socket state:
  - `TIME_WAIT`: `55700`
  - `ESTABLISHED`: `36`

## 판정

- 이번 실패는 core STREAM 처리량 hot path보다 loopback high-CCU WebSocket benchmark의
  client-side ephemeral port/TIME_WAIT 압력으로 보는 것이 맞다.
- standalone run이 성공하고, 성공 run 직후 `TIME_WAIT`가 ephemeral port 범위와 거의 같은
  수준까지 증가하며, 즉시 재실행하면 같은 `connect_ok=0/connect_fail=10000` 실패가
  재현된다.
- core 서버 프로세스 잔존이나 obvious fd leak은 확인되지 않았다.
- core에서 이를 성능 개선으로 고치는 것은 부적절하다. 서버가 benchmark client의
  active close/TIME_WAIT 정책을 바꾸도록 abortive close 같은 동작을 넣으면 public
  transport 의미와 안전성을 해칠 수 있다.
- 따라서 이번 라운드에서는 core 변경을 남기지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증,
    IPC unlink 순서, decoder/message/send guard, `maxmsgsize` 정책을 변경하지 않았다.
- 추가로 실행한 회귀 테스트:
  - 코드 변경 없음. 테스트 실행 없음.
