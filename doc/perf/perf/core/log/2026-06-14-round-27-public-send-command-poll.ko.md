# Round 27: public send command poll 비용 확인

- goal: 전체 64B 공통 항목 중 one-way 계열에 공통으로 들어가는 public send hot path 비용을 확인한다.
- 완료 기준: 문제 report 대비 targeted 64B set 중앙값 +10% 이상 또는 평균 +8% 이상을 반복 확인하고, core targeted tests 통과, source 변경이 실제 runtime hot path에 남을 수 있음.
- 시작 시각: 2026-06-14 23시대
- 기준 commit: `72d893595`
- 시작 git status:
  - core source diff 없음
  - `doc/plan/perf/core/log/2026-06-14-round-*.ko.md` 여러 파일 untracked
  - `framework/languages/dotnet/doc/*` 변경은 perf/core 작업과 무관하므로 건드리지 않는다.
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 대상 pattern/transport/size:
  - 1차: `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_SPOT` / `tcp,tls,ws,wss` / `64B`
  - 보조: `MULTI_STREAM tcp 64B`

## 기준 비교

- 문제 report의 64B throughput RESULT:
  - baseline 대비 평균 delta: `-15.62%`
  - RESULT 누락: 6개
    - `MULTI_SPOT ws/wss`
    - `MULTI_SPOT_REQREP ws/wss`
    - `MULTI_SPOT_SENDSEND ws/wss`
  - 큰 하락:
    - `MULTI_SPOT tcp`: `-47.21%`
    - `MULTI_SPOT tls`: `-46.00%`
    - `MULTI_PUBSUB tcp/tls`: `-25.30%`, `-26.61%`
    - `MULTI_DEALER_DEALER tcp/tls/ws/wss`: 약 `-21.78%` ~ `-23.20%`
    - `MULTI_STREAM tcp`: `-25.17%`
- 단, round 12-23의 반복 측정에서는 문제 report 대비 10% 이상 결손이 자주 재현되지 않았다.
- 따라서 이번 라운드는 문제 report의 단일 수치만으로 source를 바꾸지 않고, one-way 공통 call path에서 아직 검증하지 않은 비용만 분리한다.

## 가설

- 가설 1: public send 성공 경로의 `process_commands(0, true)` 호출은 mailbox read 자체는 throttle되지만, 매 메시지마다 `rdtsc`와 command-runtime 분기를 수행한다. 64B one-way에서는 이 비용이 `DEALER_DEALER`, `PUBSUB`, `SPOT`에 공통으로 보일 수 있다.
- 가설 2: public send scope/lifecycle guard 비용이 64B one-way에서 공통 비용이지만, public API safety 계약을 유지하려면 줄일 수 있는 폭이 작다.
- 가설 3: 장기 하락의 큰 부분은 perf client/server 측정 의미 변화와 run-to-run variance이며, core source 후보는 없다.
- 선택한 가설: 먼저 가설 1을 코드 기준으로 확인한다. 단, command 처리 지연 계약을 약화하는 최종 변경은 허용하지 않는다.

## 읽은 코드

- `core/src/runtime/sockets/common/socket_base_msg.cpp`
  - `socket_base_t::send_direct_with_retry()`는 정상 성공 경로에서도 `process_commands(0, true)`를 먼저 호출한다.
  - 이후 `prepare_direct_send_message()`, `_auto_hwm_send_attempts.fetch_add()`, `xsend()` 순서로 진행한다.
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
  - `process_commands(0, true)`는 `clock_t::rdtsc()`를 읽고 `max_command_delay` 이내면 mailbox read를 생략한다.
  - throttle이 false이거나 delay를 넘으면 `_mailbox->recv(&cmd, timeout_)`를 수행한다.
- `core/src/runtime/sockets/common/socket_runtime.cpp`
  - `socket_command_runtime_t::should_skip_throttled_command_poll()`는 `last_command_tsc` 기준으로 command poll 생략 여부를 결정한다.

## 변경

- 아직 없음.
- perf 전용 변경이 아닌 이유:
  - 검토 대상은 모든 public send가 지나는 core runtime path다.
  - perf runner/client/server는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음
- 보안 의미를 유지한 근거:
  - message/send guard, maxmsgsize, WS/WSS pending buffer, mtrie, IPC unlink, decoder guard를 변경하지 않는다.
- 추가로 실행한 회귀 테스트: source 변경 후 기록한다.

## 검증 계획

- source 변경 전 현재 기준 재측정:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round27_current_oneway_send_poll_base`
- 후보 probe가 필요하면:
  - `cmake --build core/build -j$(nproc)`
  - 관련 ctest
  - 같은 targeted perf
- 10% 이상 반복 개선이 없으면 source 변경은 남기지 않는다.

## 결과

### 현재 기준 재측정

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round27_current_oneway_send_poll_base`
- runner 확인:
  - stale `core/build` runtime을 감지해 자동 rebuild를 수행했다.
  - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_225458_round27_current_oneway_send_poll_base.txt`
- status:
  - success `12`, fail `0`
- load:
  - `41.35 18.49 15.74`
  - 높으므로 절대 수치 판단에는 부적합하다. 같은 라운드 안 전후 비교 기준으로만 사용한다.

#### 64B throughput

| pattern | transport | throughput |
|---------|-----------|------------|
| `MULTI_DEALER_DEALER` | `tcp` | `2,932,326.6` |
| `MULTI_DEALER_DEALER` | `tls` | `3,050,878.2` |
| `MULTI_DEALER_DEALER` | `ws` | `3,036,225.6` |
| `MULTI_DEALER_DEALER` | `wss` | `3,210,382.0` |
| `MULTI_PUBSUB` | `tcp` | `2,497,982.3` |
| `MULTI_PUBSUB` | `tls` | `2,280,933.9` |
| `MULTI_PUBSUB` | `ws` | `2,026,170.1` |
| `MULTI_PUBSUB` | `wss` | `2,505,506.0` |
| `MULTI_SPOT` | `tcp` | `4,117,426.1` |
| `MULTI_SPOT` | `tls` | `3,927,292.3` |
| `MULTI_SPOT` | `ws` | `3,608,641.4` |
| `MULTI_SPOT` | `wss` | `3,443,946.0` |

#### 현재 기준 판정

- `MULTI_SPOT ws/wss`는 문제 report에서 누락됐지만 현재 기준 측정에서는 성공했다.
- `MULTI_SPOT tcp/tls`도 문제 report보다 높다. 이번 라운드 source 후보는 SPOT 장기 하락 복구가 아니라 public send 공통 비용 분리로 제한한다.
- `MULTI_PUBSUB`는 현 run에서 낮게 나와 후보 전후 비교 대상으로 쓸 수 있다.

### 후보 A: public send command poll 비용 probe

- 변경 방향:
  - `send_direct_with_retry()`의 정상 성공 hot path에서 `process_commands(0, true)` 호출 비용을 임시로 분리한다.
  - 최종 변경으로 남기려면 command 처리 지연 계약을 유지하는 설계가 필요하다.
- 판정 기준:
  - targeted one-way set 평균/중앙값이 현재 기준 대비 10% 이상 반복 개선되지 않으면 source 변경을 남기지 않는다.

#### 후보 A 검증

- 임시 변경:
  - 파일: `core/src/runtime/sockets/common/socket_base_msg.cpp`
  - `send_direct_with_retry()` 성공 경로의 `process_commands(0, true)` 호출을 제거하고 `int rc = 0`으로 둔 probe.
- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|backpressure|backpressure_oneway|multi_socket_contract_regressions|socket_with_handler|zmp_request_reply|spot_|transport_matrix)'`
  - 결과: 실패
  - 대표 실패:
    - `test_socket_with_handler`: subscribe recv `EAGAIN`
    - `test_multi_socket_contract_regressions`: routed recv 실패
    - `test_pubsub`: recv `EAGAIN`
    - `test_pubsub_filter_xpub`: timeout
    - `test_xpub_nodrop`: blocking publish 실패
    - `test_zmp_request_reply`: spot-to-spot TLS request/reply 실패
    - `test_transport_matrix`: timeout
    - `test_spot_pubsub_scenario`: discovery interop 실패
- 판정:
  - public send의 throttled command poll은 단순 비용이 아니라 subscribe, routing, ready, discovery command 처리 타이밍에 필요한 계약 경로다.
  - perf 측정 없이도 후보 A는 계약 위반으로 탈락한다.
  - source 변경을 되돌렸다.
- 원복 검증:
  - `cmake --build core/build -j$(nproc)` 통과
  - `ctest --test-dir core/build --output-on-failure -R 'test_(socket_with_handler|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop|zmp_request_reply|transport_matrix|spot_pubsub_scenario)$'`
  - 결과: 8/8 통과

### 후보 B: PUBSUB fanout lmsg refcount/write path

- 관찰:
  - 64B payload는 `msg_t::max_vsm_size`보다 커서 LMSG 경로를 탄다.
  - `dist_t::distribute()`는 matching pipe가 여러 개이면 `msg_->add_refs(_matching - 1)` 뒤 각 pipe에 같은 메시지를 쓴다.
  - round 8/11은 distributor index lookup/final helper를 봤지만, LMSG refcount/write path 자체는 아직 충분히 좁히지 않았다.
- 다음 확인:
  - `msg_t::add_refs/rm_refs`, `pipe_t::write_message_unlocked`, `ypipe_t::write/flush`를 읽고, PUBSUB에만 과도한 비용이 있는지 확인한다.

#### 후보 B 판정

- 코드 확인:
  - `msg_t::add_refs()`는 LMSG 또는 zcopy 메시지에서만 refcount를 건드린다.
  - `dist_t::distribute()`는 matching pipe 수만큼 같은 `msg_t` header를 pipe에 쓰고, LMSG payload는 refcount로 공유한다.
  - `pipe_t::write_and_flush_no_recursive_hwm_check()`와 `ypipe_t::write/flush()`는 각 pipe의 lock/HWM/flush 계약과 직접 묶여 있다.
- 판정:
  - round 8/11에서 이미 distributor matching index와 final frame helper를 검증했고, 안정적인 10% 개선이 없었다.
  - 현재 경로에서 refcount나 pipe flush를 줄이는 변경은 PUBSUB fanout 소유권/HWM/activate-read 계약을 건드린다.
  - clear-win 후보가 없어 source 변경은 하지 않았다.

### 사용자 기준 재정렬: STREAM/tcp 64B

- 사용자가 지정한 기준:
  - baseline 파일: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 대상: `MULTI_STREAM tcp 64B`
  - baseline throughput: `400,124.600 ops/s`
  - 문제 report throughput: `299,395.000 ops/s`
  - 목표는 `STREAM/tcp 64B`에서 `400kops` 회복이다. `STREAM/ws`는 다른 기준으로 본다.

### 후보 C: stream packet body view fast path

- 가설:
  - `STREAM` multi benchmark 서버는 packet handler를 사용한다.
  - packet parser는 64B body를 `state.body.init_size()`로 새 LMSG에 할당하고, 입력 payload에서 복사한 뒤 콜백에 넘긴다.
  - 입력 `msg_t`가 이미 LMSG이면 header가 비어 있고 body가 현재 msg 안에 완전히 들어 있는 경우 `msg_t::init_view()`로 body view를 넘겨 body 할당/복사를 줄일 수 있다.
- 임시 변경:
  - 파일: `core/src/runtime/sockets/stream/stream.cpp`
  - `stream_dispatch_packet_msg_from_io()`에서 `header_size == 0`, `body_size > msg_t::max_vsm_size`, 현재 payload에 body가 모두 있을 때 `body_out.init_view(*msg_, offset, body_size)`를 사용했다.
- test:
  - `cmake --build core/build -j$(nproc)` 통과
  - `ctest --test-dir core/build --output-on-failure -R 'test_(stream_socket|stream_fastpath|stream_threadsafe|stream_send_blocking_wakeup|multi_stream_server_reassembly|transport_matrix)$'`
  - 결과: 6/6 통과
- perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --results-tag round27_stream_packet_body_view`
    - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_231128_round27_stream_packet_body_view.txt`
    - load: `20.44 13.74 12.83`
    - throughput: `337,300.800 ops/s`
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --results-tag round27_stream_packet_body_view_repeat`
    - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_231422_round27_stream_packet_body_view_repeat.txt`
    - load: `49.12 25.49 17.20`
    - throughput: `329,422.200 ops/s`
  - A/B no-body-view:
    - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --results-tag round27_stream_packet_no_body_view_ab`
    - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_231521_round27_stream_packet_no_body_view_ab.txt`
    - load: `58.57 31.04 19.53`
    - throughput: `323,333.000 ops/s`
- 판정:
  - 문제 report의 `299,395 ops/s`보다는 높지만, 같은 시간대 A/B 개선은 약 `1.9%`다.
  - 이 plan의 noise 기준인 5%에도 못 미치므로 clear-win이 아니다.
  - source 변경은 되돌렸다.

### 후보 D: stream dispatch callback direct xsend

- 가설:
  - packet handler callback 안에서 같은 routing id로 echo를 보내는 경우 dispatch TLS에 현재 pipe가 있다.
  - `stream_t::xsend()`의 routing-id 단일 frame 경로에서 shard map lookup/lock을 생략하고 현재 dispatch pipe의 peer에 직접 쓸 수 있다.
- 임시 변경:
  - 파일: `core/src/runtime/sockets/stream/stream.cpp`
  - `xsend()` 초입에서 `resolve_direct_dispatch_output_pipe(this, routing_id)`가 성공하면 direct pipe write를 시도했다.
- test:
  - `cmake --build core/build -j$(nproc)` 통과
  - `ctest --test-dir core/build --output-on-failure -R 'test_(stream_socket|stream_fastpath|stream_threadsafe|stream_send_blocking_wakeup|multi_stream_server_reassembly|transport_matrix)$'`
  - 결과: 6/6 통과
- perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --results-tag round27_stream_dispatch_direct_xsend`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_231717_round27_stream_dispatch_direct_xsend.txt`
  - load: `38.01 30.54 20.62`
  - throughput: `323,210.000 ops/s`
- 판정:
  - no-body-view A/B의 `323,333.000 ops/s`와 사실상 같다.
  - route shard lookup/lock은 현재 stream/tcp 64B 병목이 아니거나, runner noise보다 작다.
  - source 변경은 되돌렸다.

### 후보 E: stream dispatch inflight memory order

- 가설:
  - `_dispatch_inflight`는 stream stop 동기화에는 쓰이지 않고 public 조회값 성격이므로 fetch add/sub를 relaxed로 낮출 수 있다.
- 임시 변경:
  - `stream_dispatch_inflight()` load와 dispatch callback add/sub를 relaxed로 바꿨다.
- perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --results-tag round27_stream_packet_body_view_relaxed_inflight`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_231323_round27_stream_packet_body_view_relaxed_inflight.txt`
  - throughput: `332,377.000 ops/s`
- 판정:
  - body-view 단독 측정보다 낮고 반복 clear-win이 아니다.
  - source 변경은 되돌렸다.

### 후보 F: low_latency + initial target cap 8192 환경 probe

- 목적:
  - round 26에서 `--auto-hwm-profile low_latency`와 `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=8192`가 각각 330k대 신호를 보였다.
  - 두 설정을 조합해 400k 복구가 설정성 문제인지 확인한다.
- command:
  - `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=8192 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 2 --auto-hwm-profile low_latency --results-tag round27_stream_tcp_low_latency_cap8192_probe`
- runner 확인:
  - mtime 때문에 stale core runtime을 감지해 자동 rebuild했다.
  - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_232010_round27_stream_tcp_low_latency_cap8192_probe.txt`
- result:
  - load: `45.47 27.80 20.73`
  - `STREAM/tcp 64B`: `315,538.600 ops/s`
  - low latency auto-HWM detail: unit budget `16KB`, SNDHWM/RCVHWM `64`, SNDBUF/RCVBUF `256KB`
- 판정:
  - low latency와 larger initial target cap 조합은 400k 복구로 이어지지 않았다.
  - 설정성 문제로 보기 어렵고, ASIO stream/tcp 64B echo 구조 비용을 더 좁혀야 한다.

## Round 27 현재 결론

- public send command poll 제거는 계약 테스트를 깨서 탈락했다.
- PUBSUB LMSG fanout 경로는 이미 round 8/11과 겹치는 영역이고, 남은 변경은 소유권/HWM/flush 계약 위험이 크다.
- `STREAM/tcp 64B` 기준은 `400,124.600 ops/s`가 맞다. 이번 stream 후보들은 최대 `337,300.800 ops/s`까지 보였지만, 같은 시간대 A/B 개선은 5% 미만이었다.
- `low_latency + initial target cap 8192` 조합도 `315,538.600 ops/s`라 설정만으로 복구되지 않았다.
- 따라서 round 27에서 source에 남길 clear-win 변경은 없다.
- 다음 라운드는 stream packet handler 외부의 core 공통 경로보다, `STREAM/tcp 64B`의 실제 병목을 profiler나 더 긴 A/B 반복으로 먼저 좁혀야 한다.
