# Round 28: public send command throttle window 검토

- goal: 전체 64B one-way 공통 경로의 public send command poll 비용을 계약 유지 범위에서 줄인다.
- 완료 기준:
  - targeted 64B one-way set(`DEALER_DEALER,PUBSUB,SPOT`) 평균 또는 중앙값이 현재 기준 대비 `+10%` 이상 반복 개선된다.
  - 관련 core 테스트가 통과한다.
  - perf runner가 `core/build` runtime을 사용한다.
  - 작업 로그에 변경/측정/원복 여부를 남긴다.
- 시작 시각: 2026-06-14
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 시작 git status:
  - core source diff 없음.
  - unrelated `.NET` 문서 변경과 기존 perf log untracked 파일이 있음.
- 대상 pattern/transport/size:
  - 1차: `DEALER_DEALER,PUBSUB,SPOT`, `tcp,tls,ws,wss`, `64B`
  - 관찰: `STREAM/tcp 64B`는 별도 중요 지표이나 round28의 단독 목표는 아니다.

## 기준 비교

- 공통 64B throughput present: `26`
- 큰 하락:
  - `MULTI_SPOT tcp`: `-47.21%`
  - `MULTI_SPOT tls`: `-46.00%`
  - `MULTI_STREAM ws`: `-33.54%`
  - `MULTI_PUBSUB tls`: `-26.61%`
  - `MULTI_PUBSUB tcp`: `-25.30%`
  - `MULTI_STREAM tcp`: `-25.17%`
  - `MULTI_DEALER_DEALER tcp/tls/ws/wss`: 약 `-21.78%` ~ `-23.20%`

## 가설

- 가설 1: public send hot path의 `process_commands(0, true)`는 계약상 필요하지만, throttle window가 너무 짧아 64B one-way 연속 송신에서 mailbox/signal read 비용이 과하게 반복된다.
- 가설 2: throttle window를 늘리면 subscribe/routing/ready/discovery command 처리 지연 위험이 있다. round27의 단순 poll 제거는 이 계약을 깨서 탈락했으므로, 이번 후보는 지연 폭만 제한적으로 늘린다.
- 가설 3: 회귀의 주원인은 command poll이 아니라 ASIO/pipe/fanout 경로이며, throttle window 조정은 5% 미만 오차에 그칠 수 있다.
- 선택한 가설: 가설 1을 먼저 검증한다. 이유는 `DEALER_DEALER`, `PUBSUB`, `SPOT` 모두 public send 경로를 공유하며, stream 단독 후보보다 전체 64B 목표에 맞기 때문이다.

## 읽은 코드

- `core/src/runtime/sockets/common/socket_base_msg.cpp`
  - `send_direct_with_retry()`가 정상 hot path마다 `process_commands(0, true)`를 호출한다.
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
  - `process_commands(timeout=0, throttle=true)`는 `rdtsc()`와 `socket_command_runtime_t::should_skip_throttled_command_poll()`로 최근 poll 여부를 확인한다.
  - skip하지 않으면 `_mailbox->recv(&cmd, 0)`로 signaler와 mailbox lock 경로를 탄다.
- `core/src/runtime/sockets/common/socket_runtime.cpp`
  - `should_skip_throttled_command_poll()`는 `tsc - last_command_tsc <= max_command_delay`이면 command poll을 생략한다.
- `core/src/runtime/utils/config.hpp`
  - `max_command_delay = 3000000`
  - 주석상 현재 CPU 기준 약 `1~2ms` command 처리 지연 상한이다.
- `core/src/runtime/core/mailbox.cpp`
  - non-blocking `recv(0)`도 `_active` 상태에 따라 signaler read, `_sync` lock, ypipe read를 수행한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드릴 수 있는 보안 항목:
  - 직접 관련 없음. message guard, decoder guard, maxmsgsize, WS/WSS pending copy, mtrie, IPC unlink, port parsing을 건드리지 않는다.
- 보안 의미를 유지한 근거:
  - 후보는 command poll throttle 상수만 제한적으로 조정한다.
  - public send guard와 command poll 자체는 제거하지 않는다.
- 추가로 실행할 회귀 테스트:
  - round27에서 깨졌던 `test_socket_with_handler`, `test_multi_socket_contract_regressions`, `test_pubsub`, `test_pubsub_filter_xpub`, `test_xpub_nodrop`, `test_zmp_request_reply`, `test_transport_matrix`, `test_spot_pubsub_scenario`

## 후보 A: max_command_delay 2배 probe

- 변경 방향:
  - `core/src/runtime/utils/config.hpp`의 `max_command_delay`를 `3000000`에서 `6000000`으로 임시 변경한다.
- perf 전용 변경이 아닌 이유:
  - 실제 public API send hot path의 command polling 정책이다.
  - benchmark 조건이나 perf client/server를 바꾸지 않는다.
- 판정 기준:
  - 관련 CTest가 통과해야 한다.
  - targeted one-way 64B set에서 현재 기준 대비 `+10%` 반복 개선이 없으면 source 변경을 남기지 않는다.

### 후보 A 검증

- 변경:
  - `core/src/runtime/utils/config.hpp`
  - `max_command_delay = 6000000`
- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
  - 참고: 일부 `Clock skew detected` 경고가 있었지만 최종 종료 코드는 성공이었다.
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(socket_with_handler|multi_socket_contract_regressions|pubsub|pubsub_filter_xpub|xpub_nodrop|zmp_request_reply|transport_matrix|spot_pubsub_scenario)$'`
  - 결과: 8/8 통과
- perf:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern DEALER_DEALER,PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round28_command_delay_6m_oneway`
  - runner 확인:
    - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_232432_round28_command_delay_6m_oneway.txt`
  - status:
    - success `12`, fail `0`
  - load:
    - `18.03 20.24 18.90`

#### round27 current 대비

| pattern | transport | round27 | round28 6M | delta |
|---------|-----------|---------|------------|-------|
| `MULTI_DEALER_DEALER` | `tcp` | `2,932,326.6` | `2,939,911.8` | `+0.26%` |
| `MULTI_DEALER_DEALER` | `tls` | `3,050,878.2` | `3,028,608.1` | `-0.73%` |
| `MULTI_DEALER_DEALER` | `ws` | `3,036,225.6` | `2,985,979.9` | `-1.65%` |
| `MULTI_DEALER_DEALER` | `wss` | `3,210,382.0` | `3,115,680.7` | `-2.95%` |
| `MULTI_PUBSUB` | `tcp` | `2,497,982.3` | `2,358,562.9` | `-5.58%` |
| `MULTI_PUBSUB` | `tls` | `2,280,933.9` | `2,333,250.3` | `+2.29%` |
| `MULTI_PUBSUB` | `ws` | `2,026,170.1` | `2,196,416.8` | `+8.40%` |
| `MULTI_PUBSUB` | `wss` | `2,505,506.0` | `2,538,749.0` | `+1.33%` |
| `MULTI_SPOT` | `tcp` | `4,117,426.1` | `3,762,510.0` | `-8.62%` |
| `MULTI_SPOT` | `tls` | `3,927,292.3` | `3,874,674.6` | `-1.34%` |
| `MULTI_SPOT` | `ws` | `3,608,641.4` | `3,427,410.8` | `-5.02%` |
| `MULTI_SPOT` | `wss` | `3,443,946.0` | `3,411,433.1` | `-0.94%` |

- 평균 delta: `-1.21%`
- 중앙값 delta: `-1.14%`

### 후보 A 판정

- 계약 테스트는 통과했지만, targeted one-way 64B set이 개선되지 않았다.
- `PUBSUB ws`만 `+8.40%`였고 10% 기준에도 못 미치며, SPOT tcp/ws와 PUBSUB tcp는 오히려 낮았다.
- 전체 평균/중앙값이 음수이므로 source 변경을 남길 이유가 없다.
- `max_command_delay` 변경은 원복했다.

## Round 28 현재 결론

- public send command poll 자체는 계약상 필요하고, 단순 제거는 round27에서 실패했다.
- throttle window를 2배로 늘리는 보수적 조정은 계약 테스트를 통과했지만 성능 개선이 없었다.
- command poll throttle 상수는 현재 64B one-way 공통 회귀의 주 병목으로 보기 어렵다.
- source에 남은 변경은 없다.
- 다음 후보는 public send poll이 아니라 pipe/ASIO/fanout 중 실제 data-plane에서 관찰되는 경로로 옮긴다.

## 기준 보정: STREAM/tcp 64B

- 사용자 확인 기준:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 현재 문제 보고서: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- `MULTI_STREAM,tcp,64,throughput`:
  - baseline: `400,124.6`
  - 현재 문제 보고서: `299,395.0`
  - delta: `-25.17%`
- STREAM의 목표 `400kops`는 이 `tcp 64B` 기준이다.
- WS/WSS STREAM은 다른 기준으로 다룬다. round28의 후보 A/B 판정에 WS STREAM 수치를 섞지 않는다.

## 후보 B: publish 단일 FINAL fast path probe

- 변경 방향:
  - `zlink_publish_part()`에서 `ZLINK_PART_FINAL`이고 topic이 있으며 active send sequence가 없는 경우, generic `submit_simple_part()` 경유를 피하고 topic frame과 payload frame을 같은 public send scope에서 직접 전송하는 임시 fast path를 검증했다.
- perf 전용 변경이 아닌 이유:
  - C public publish API의 실제 단일 메시지 송신 경로다.
  - benchmark runner/client/server를 바꾸지 않았다.
- 기대:
  - `MULTI_PUBSUB`는 `perf_zlink_publish_parts(..., part_count=1, ZLINK_DONTWAIT)`를 사용하므로 직접 영향을 받는다.
  - `MULTI_SPOT`의 publish 경로에도 일부 영향을 줄 수 있다.
- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
  - 참고: 일부 `Clock skew detected` 경고가 있었지만 최종 종료 코드는 성공이었다.
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|spot_pubsub_scenario|spot_subject_access|transport_matrix|multi_socket_contract_regressions)$'`
  - 결과: 7/7 통과
- perf:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,SPOT --transports tcp,tls,ws,wss --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round28_publish_final_fastpath`
  - runner 확인:
    - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_233553_round28_publish_final_fastpath.txt`
  - status:
    - success `8`, fail `0`
  - load:
    - `9.74 10.59 12.64`

### 후보 B round27 current 대비

| pattern | transport | round27 | round28 publish fast path | delta |
|---------|-----------|---------|---------------------------|-------|
| `MULTI_PUBSUB` | `tcp` | `2,497,982.3` | `2,533,181.1` | `+1.41%` |
| `MULTI_PUBSUB` | `tls` | `2,280,933.9` | `2,339,991.3` | `+2.59%` |
| `MULTI_PUBSUB` | `ws` | `2,026,170.1` | `2,252,967.5` | `+11.19%` |
| `MULTI_PUBSUB` | `wss` | `2,505,506.0` | `2,545,038.3` | `+1.58%` |
| `MULTI_SPOT` | `tcp` | `4,117,426.1` | `3,630,609.0` | `-11.82%` |
| `MULTI_SPOT` | `tls` | `3,927,292.3` | `4,101,961.3` | `+4.45%` |
| `MULTI_SPOT` | `ws` | `3,608,641.4` | `3,430,034.6` | `-4.95%` |
| `MULTI_SPOT` | `wss` | `3,443,946.0` | `3,529,624.6` | `+2.49%` |

- 평균 delta: `+0.87%`
- 중앙값 delta: `+2.03%`

### 후보 B 판정

- 계약 테스트와 targeted perf 실패 수는 문제없었다.
- 그러나 평균/중앙값 개선이 작고, `MULTI_SPOT tcp`가 `-11.82%`로 악화했다.
- `MULTI_PUBSUB ws`만 `+11.19%`였지만, 사용자 기준 STREAM/tcp 64B 목표와도 무관하고 전체 one-way 목표를 만족하지 않는다.
- source 변경을 원복했다.
- 다음 후보는 `MULTI_STREAM,tcp,64`를 baseline `400,124.6`으로 회복시키는 방향으로 좁힌다.

## STREAM/tcp 64B clean source 재측정

- source 상태:
  - 후보 A/B source 변경은 모두 원복했다.
  - `core/src`, `core/include`, `core/tests`에 남은 source diff는 없다.
- baseline 메타:
  - `META,commit,cb605c6c1`
  - `META,load_avg,0.10 0.36 0.42`
  - `connect_concurrency: 128 (default)`
- current clean rerun 1:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round28_stream_tcp_current_rerun`
  - runner 확인:
    - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_234408_round28_stream_tcp_current_rerun.txt`
  - load:
    - `45.19 14.24 11.66`
  - result:
    - `STREAM/tcp 64B`: `325,032.0 ops/s`
- current clean rerun 2:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round28_stream_tcp_quiet_rerun`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_234516_round28_stream_tcp_quiet_rerun.txt`
  - load:
    - `13.15 11.16 10.78`
  - result:
    - `STREAM/tcp 64B`: `330,305.4 ops/s`
- baseline-compatible connect concurrency:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round28_stream_tcp_connect128_clean`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_234533_round28_stream_tcp_connect128_clean.txt`
  - load:
    - `10.50 10.68 10.63`
  - result:
    - `STREAM/tcp 64B`: `332,152.2 ops/s`

### STREAM/tcp clean source 판정

- 현재 clean source는 문제 report `299,395.0 ops/s`보다는 높지만 baseline `400,124.6 ops/s`에는 아직 `-16.99%` 수준이다.
- `connect_concurrency` 차이는 400k 회복 원인이 아니다.
- baseline 대비 `stream.cpp`, `tcp_transport.cpp`, `asio_stream_fastpath_policy.hpp`의 직접 diff는 대부분 포맷/파일 이동이며, 의미 있는 차이는 작다.
- round26/27에서 native send, read drain, batch, packet body view, direct xsend, inflight memory order, HWM/thread 설정을 이미 시도했지만 400k에 도달하지 못했다.
- 다음 단계는 추가 미세 변경보다 `STREAM/tcp 64B`의 실제 CPU hot spot을 profiler로 좁히는 것이다.

## STREAM perf helper send serialization 진단

- baseline `cb605c6c1`의 `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`:
  - `try_send_packet_now()`가 callback 안에서 바로 `zlink_send_rid(stream_socket, rid, packet, 1, ZLINK_DONTWAIT)`를 호출했다.
  - `session_t`에 `send_mutex`가 없었다.
- current:
  - `1b60c0159 fix: serialize C multi stream sends`에서 `send_mutex`가 추가됐다.
  - `f1bdecfa2` 이후 send helper는 `perf_zlink_send_rid_parts(...)`를 사용한다.
- core 계약 근거:
  - `core/tests/integration/test_stream_threadsafe.cpp`에 STREAM send thread-safety 회귀 테스트가 있다.
  - `core/tests/integration/test_thread_safe_contract_policy.cpp`는 public threading doc에 `send`/`publish`/`send_rid` 동시 호출 계약이 있음을 확인한다.
- 진단 전용 A/B:
  - 변경:
    - `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`에서 `send_mutex`와 두 `lock_guard`를 임시 제거했다.
    - 이 변경은 runner 변경이며 최종 source에 남기지 않았다.
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round28_stream_tcp_no_send_mutex_probe`
  - runner 확인:
    - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_234722_round28_stream_tcp_no_send_mutex_probe.txt`
  - load:
    - `1.86 7.43 9.45`
  - result:
    - `STREAM/tcp 64B`: `373,518.2 ops/s`
- 비교:
  - current clean `connect_concurrency=128`: `332,152.2 ops/s`
  - no-send-mutex probe: `373,518.2 ops/s`
  - delta: `+12.45%`
  - baseline `400,124.6 ops/s` 대비 no-send-mutex probe는 아직 `-6.65%`
- 판정:
  - `STREAM/tcp 64B` 하락의 큰 부분은 core runtime hot path가 아니라 perf helper의 benchmark-side send serialization에서 나온다.
  - runner 변경은 계획 범위 밖이므로 최종 source에 남기지 않았다.
  - 오염 방지를 위해 원복 후 `cmake --build bindings/c/build --target comp_src_stream_server -j$(nproc)`로 원래 runner 바이너리를 다시 링크했다.
  - core source diff는 없다.
- 추가 분리 시도:
  - mutex는 유지하고 send API만 baseline의 `zlink_send_rid(...)` 형태로 되돌리는 probe를 시도했다.
  - 현재 header에는 `zlink_send_rid`가 없어서 `comp_src_stream_server` 빌드가 실패했다.
  - 이 probe는 유효하지 않으므로 즉시 원복했고, 다시 `comp_src_stream_server`를 정상 빌드했다.

## Round 28 최종 결론

- `STREAM/tcp 64B` 목표 기준은 baseline `400,124.6 ops/s`가 맞다.
- current clean source + current runner는 `332,152.2 ops/s` 수준이다.
- current core source에서 여러 STREAM/TCP 후보를 시도했지만 400k를 회복한 core 변경은 없다.
- benchmark-side `send_mutex` 제거 진단만 `373,518.2 ops/s`까지 회복했다.
- 따라서 지금 관찰된 `250k~330k` STREAM 하락은 core source 미세 최적화 실패라기보다, current perf helper가 baseline과 다른 송신 직렬화 조건을 갖게 된 영향이 크다.
- 이 라운드에서는 perf runner 변경을 최종 source에 남기지 않았고, core source에도 남은 변경이 없다.
