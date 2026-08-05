# Round 26: 공통 64B hot path 재정렬

- goal: 전체 64B 공통 항목의 현재 기준선을 다시 잡고, one-way와 echo에 함께 남는 core hot path 병목을 찾는다.
- 완료 기준: current HEAD 64B targeted set을 failure 0으로 확보하고, 최소 하나의 core hot path 후보를 코드 기준으로 검증한다. 변경을 넣는 경우 targeted 64B 중앙값 +10% 이상 또는 후보를 되돌린다.
- 시작 시각: 2026-06-14 19:30 KST
- 기준 commit: `bc944bded`
- 시작 git status: Java binding 변경 다수와 round 9-25 로그 파일이 dirty/untracked 상태다. core/perf 소스 diff는 없다.
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 대상 pattern/transport/size: 전체 multi pattern의 64B 공통 항목. `STREAM/ws/64B`는 별도 관찰 항목으로 유지한다.

## 기준 비교

- 공통 64B 항목 수: 26
- 현재 문제 report의 과거 기준 대비 평균: `-15.62%`
- 현재 문제 report의 과거 기준 대비 중앙값: `-14.86%`
- one-way 평균: `-27.36%`
- echo 평균: `-8.28%`
- 10% 이상 하락 주요 항목:
  - `MULTI_SPOT tcp`: `-47.21%`
  - `MULTI_SPOT tls`: `-46.00%`
  - `MULTI_STREAM ws`: `-33.54%`
  - `MULTI_PUBSUB tls`: `-26.61%`
  - `MULTI_PUBSUB tcp`: `-25.30%`
  - `MULTI_STREAM tcp`: `-25.17%`
  - `MULTI_DEALER_DEALER tcp/tls/ws/wss`: 약 `-21.78%` ~ `-23.20%`

## 가설

- 가설 1: 현재 가장 큰 gap은 특정 STREAM 문제가 아니라 one-way 공통 경로의 작은 메시지 이동 비용이다. `msg_t` close/init, pipe write/read, session pull/push, mailbox wakeup 중 하나가 공통 비용으로 남아 있다.
- 가설 2: SPOT/PUBSUB의 큰 gap은 fanout/matching 계층이 message frame을 반복 복사하거나 refcount/ownership 비용을 늘리는 데서 온다. 공통 one-way 경로 개선과 별도로 fanout 전용 후보가 필요하다.
- 가설 3: STREAM/ws 64B는 별도 목표처럼 보이지만, 현재 core source diff가 없는 상태에서도 225k~292k 사이로 흔들렸다. source 회귀보다 현재 시스템 상태와 STREAM 자체 hot path 부족을 분리해야 한다.
- 선택한 가설: 먼저 가설 1을 검증한다. 전체 목표가 64B 평균/중앙값이므로 DEALER/PUBSUB/SPOT/STREAM을 함께 움직일 수 있는 공통 경로가 우선이다.

## 읽을 코드

- `core/src/runtime/core/msg.*`: 작은 메시지 allocation/refcount/close 비용
- `core/src/runtime/core/pipe.*`, `ypipe.*`: one-way enqueue/dequeue 비용
- `core/src/runtime/core/mailbox.*`, `signaler.*`, `io_thread.*`: wakeup 비용
- `core/src/runtime/sockets/pubsub*`, `services/spot/*`: fanout과 matching 비용
- `core/src/runtime/engine/asio/*`, `sockets/stream/*`: STREAM 관찰 항목

## 변경

- 변경 파일: 아직 없음
- 변경 이유: current 64B targeted 기준선을 먼저 확정한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음
- 보안 의미를 유지한 근거: 아직 source 변경 전이다.
- 추가로 실행한 회귀 테스트: source 변경 전에는 없음

## 검증 예정

- current 64B targeted:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round26_current_64b_targeted`

## current 64B targeted 결과

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round26_current_64b_targeted`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_193103_round26_current_64b_targeted.txt`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- completion: success 32, fail 0, status complete
- problem report 대비 공통 64B:
  - 평균: `-3.22%`
  - 중앙값: `-3.87%`
  - one-way 평균: `-6.45%`
  - echo 평균: `-1.20%`
- baseline 대비 공통 64B:
  - 평균: `-15.93%`
  - 중앙값: `-13.12%`
  - one-way 평균: `-33.37%`
  - echo 평균: `-5.47%`
- problem report 대비 worst:
  - `MULTI_SPOT tcp`: `-10.98%`
  - `MULTI_STREAM ws`: `-10.06%`
  - `MULTI_SPOT tls`: `-9.50%`
  - `MULTI_STREAM tcp`: `-9.50%`
  - `MULTI_SPOT_SENDSEND tls`: `-9.36%`
- baseline 대비 worst:
  - `MULTI_SPOT tcp`: `-53.00%`
  - `MULTI_SPOT tls`: `-51.14%`
  - `MULTI_SPOT wss`: `-40.92%`
  - `MULTI_SPOT ws`: `-40.83%`
  - `MULTI_STREAM ws`: `-40.23%`

## 갱신 판정

- current 64B targeted는 failure 0이다.
- problem report 대비 목표인 평균 `+8%`, 중앙값 `+10%`, one-way `+10%`, echo `+5%`는 모두 미달이다.
- baseline 대비 one-way gap이 가장 크며, 특히 SPOT/PUBSUB/DEALER_DEALER가 크다.
- 다음 코드 추적은 SPOT data plane과 PUBSUB fanout을 보되, 공통 `msg_t`/pipe/session 이동 비용을 함께 본다.

## 읽은 코드

- `core/src/runtime/sockets/pubsub/xpub.cpp`: XPUB delivery-ready count 갱신은 attach/write/subscription/pipe termination에서 수행되고, data send마다 호출되지는 않는다.
- `core/src/runtime/sockets/pubsub/xsub.cpp`: XSUB delivery-ready count 갱신은 attach/write/subscription/pipe termination에서 수행된다. data frame upstream send는 바로 distributor로 간다.
- `core/src/runtime/sockets/internal/dist.cpp`: 단일 matching pipe fast path와 VSM fanout 경로가 이미 있다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`: SPOT local fanout은 즉시 전송 성공 후에도 `refresh_poller_interest()`를 호출한다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_loop.cpp`: data-plane tick 끝에서도 poller interest refresh를 수행한다.

## 후보 A: SPOT local fanout 성공 경로 poller refresh 조건화

- 변경 파일: `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 변경 내용: `forward_local_fanout()`에서 pending message가 실제로 추가된 경우에만 마지막 `refresh_poller_interest()`를 호출하도록 임시 변경했다.
- 가설: 정상 64B steady-state에서는 즉시 전송이 성공하고 pollout interest가 바뀌지 않으므로, 매 publish마다 local/remote target을 순회하는 비용을 줄이면 SPOT one-way가 오른다.
- perf 전용 변경이 아닌 이유: SPOT data-plane 실제 publish forward 경로의 불필요한 관리 작업을 줄이는 변경이다.

### 검증

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_|test_spot_pubsub_scenario|test_spot_poller|test_spot_runtime_activation|test_backpressure_(oneway_)?matrix_spot_regression|test_pubsub$|test_pubsub_filter_xpub|test_xpub_nodrop'`
  - 결과: 18/18 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round26_spot_local_poll_refresh_probe`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_194037_round26_spot_local_poll_refresh_probe.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - load_avg: `28.71 11.31 6.10`
  - completion: success 4, fail 0, status complete
  - 결과:
    - `SPOT tcp`: `3,462,700.0 msg/s`
    - `SPOT tls`: `3,454,563.0 msg/s`
    - `SPOT ws`: `4,026,036.4 msg/s`
    - `SPOT wss`: `3,546,754.6 msg/s`

### 판정

- load가 높아 판정력은 약하지만, round26 current 기준과 비교해 반복 +10% 개선이 없다.
- `tcp`는 사실상 동일하고, `tls`는 +2% 수준, `ws/wss`는 하락했다.
- 후보 A는 목표 미달이므로 source 변경을 되돌렸다.
- 되돌린 뒤 `cmake --build core/build -j$(nproc)` 통과.
- 현재 core source diff 없음.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 변경하지 않았다. 임시 변경은 SPOT data-plane poller interest refresh 조건뿐이었고 최종적으로 되돌렸다.
- 추가로 실행한 회귀 테스트: SPOT/PUBSUB focused 18개 테스트 통과.

## 다음 후보

- SPOT/PUBSUB만이 아니라 DEALER_DEALER도 baseline 대비 -25%대이므로, 다음은 `dist_t`보다 아래의 pipe/session/message 이동 비용을 다시 본다.
- 이전 round에서 단일 pipe fast path, pipe flush flag, monitor counter 제거가 효과 없었으므로, 남은 후보는 `pipe_t`/`ypipe_t` enqueue-dequeue와 `session_base_t` activation/wakeup 경계다.

## STREAM/ws 64B 목표 재정렬

- 사용자 지적:
  - 목표: `STREAM/ws 64B` 400kops
  - 과거 관측: 300kops 근처
  - 현재 관측: 250kops 이하
- round26 current targeted:
  - `STREAM/ws 64B`: `219,138.0 ops/s`
  - problem report 대비: `-10.06%`
  - baseline 대비: `-40.23%`
- 이 상태에서는 공통 평균보다 `STREAM/ws 64B` 복구가 먼저다.

### 설정 민감도 측정

- 변경 없이 환경변수로만 측정했다. perf runner와 source는 바꾸지 않았다.
- `ZLINK_ASIO_STREAM_BATCH_SIZE=8192`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_194926_round26_stream_ws_batch8192_probe.txt`
  - `STREAM/ws 64B`: `251,477.6 ops/s`
- `ZLINK_ASIO_STREAM_BATCH_SIZE=2048`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_194958_round26_stream_ws_batch2048_probe.txt`
  - `STREAM/ws 64B`: `260,780.4 ops/s`
- `ZLINK_ASIO_STREAM_BATCH_SIZE=1024`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_195010_round26_stream_ws_batch1024_probe.txt`
  - `STREAM/ws 64B`: `235,893.0 ops/s`
- `--auto-hwm-profile throughput`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_195028_round26_stream_ws_auto_hwm_throughput_probe.txt`
  - `STREAM/ws 64B`: `222,011.2 ops/s`
- 병렬로 실행한 `batch16384`와 `lwm16` probe는 서로 간섭했다. 수치가 크게 나빠졌으므로 후보 판정 자료로 쓰지 않는다.

### 후보 B: WS STREAM 초기 target 축소

- 파일: `core/src/runtime/transports/ws/asio_ws_engine.cpp`
- 가설:
  - `STREAM` socket은 기본 batch를 4096으로 낮췄지만, WS engine은 STREAM read/write 초기 target을 64KB로 강제한다.
  - 64B echo에서는 64KB target이 너무 커서 write scheduling과 latency가 악화될 수 있다.
  - 환경변수 측정에서 2048/8192가 현재보다 나은 신호가 있었으므로, WS STREAM 초기 target을 작게 낮춰 검증한다.
- perf 전용 변경이 아닌 이유:
  - 실제 WS STREAM engine의 batch/read-write scheduling 정책 변경이다.
  - benchmark client/server나 runner 동작은 바꾸지 않는다.

#### 검증

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_stream_|test_transport_matrix|test_multi_stream_server_reassembly|test_zmp_request_reply'`
  - 결과: 23/23 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round26_stream_ws_ws_target_2k_probe`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_195334_round26_stream_ws_ws_target_2k_probe.txt`
  - load_avg: `14.80 14.50 10.01`
  - `STREAM/ws 64B`: `228,087.2 ops/s`

#### 판정

- 목표 400kops에 전혀 못 미치고, round26 current `219,138.0 ops/s` 대비 개선폭도 작다.
- load가 높았지만, 환경변수 측정에서 보인 `260,780.4 ops/s` 신호를 재현하지 못했다.
- 후보 B는 source 변경을 되돌렸다.

### 후보 C: STREAM batch 기본값 축소

- 파일: `core/src/runtime/sockets/stream/stream_batch_policy.hpp`
- 변경 내용: `ZLINK_ASIO_STREAM_BATCH_SIZE` 기본값을 `4096`에서 `2048`로 낮춘다.
- 가설:
  - 환경변수 측정에서 `ZLINK_ASIO_STREAM_BATCH_SIZE=2048`이 `STREAM/ws 64B`를 `260,780.4 ops/s`까지 올렸다.
  - WS engine의 64KB target 자체보다 encoder 생성 시점의 STREAM batch 크기가 작은 메시지 echo 경로에 더 직접적으로 영향을 줄 수 있다.
- perf 전용 변경이 아닌 이유:
  - STREAM socket의 실제 기본 batch 정책 변경이다.
  - benchmark runner나 client/server는 바꾸지 않는다.

#### 검증

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
  - 참고: `/tmp` clock skew 경고가 있었지만 최종 빌드는 완료됐다.
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_stream_|test_transport_matrix|test_multi_stream_server_reassembly|test_zmp_request_reply'`
  - 결과: 23/23 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round26_stream_ws_batch_default_2k_probe`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_195611_round26_stream_ws_batch_default_2k_probe.txt`
  - load_avg: `14.28 17.92 12.25`
  - `STREAM/ws 64B`: `201,973.2 ops/s`

#### 판정

- 목표 400kops와 반대 방향이다.
- 환경변수 단독 측정의 `260,780.4 ops/s`는 source 기본값 변경으로 재현되지 않았다.
- 후보 C는 source 변경을 되돌렸다.

## 다음 조사 방향

- WS STREAM 64B는 batch 기본값보다 `asio_ws_engine_t::speculative_write()`와 transport write policy의 영향이 클 가능성이 높다.
- 다음은 `supports_speculative_write()`, `write_some()`, async fallback 조건을 확인한다.

## STREAM/tcp 64B 기준 정정

- 기준 파일: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- baseline:
  - `MULTI_STREAM tcp 64B`: `400,124.6 ops/s`
  - `MULTI_STREAM ws 64B`: `366,639.2 ops/s`
- problem report:
  - `MULTI_STREAM tcp 64B`: `299,395.0 ops/s`
  - `MULTI_STREAM ws 64B`: `243,650.8 ops/s`
- 400kops 목표는 `STREAM/tcp 64B` 기준이다. 앞선 WS 중심 판단은 목표 기준을 잘못 잡은 것이다.

### STREAM/tcp 현재 재측정

- quiet 기본값 재측정:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_221735_round26_stream_tcp_default_quiet_rerun.txt`
  - load_avg: `0.78 0.31 0.53`
  - `STREAM/tcp 64B`: `321,878.0 ops/s`
- `ZLINK_ASIO_SINGLE_WRITE=1`:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_221721_round26_stream_tcp_single_write_probe.txt`
  - `STREAM/tcp 64B`: `309,559.4 ops/s`
- `--auto-hwm-profile low_latency`:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_221803_round26_stream_tcp_low_latency_probe.txt`
  - `STREAM/tcp 64B`: `331,283.6 ops/s`
- `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=8192`:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_221817_round26_stream_tcp_initial_cap8192_probe.txt`
  - `STREAM/tcp 64B`: `335,098.0 ops/s`
- 병렬로 실행한 batch 8192/2048 probe는 서로 간섭했으므로 판정 자료로 쓰지 않는다.

### 후보 D: STREAM read drain 반복 상한 축소

- 파일: `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`
- 가설:
  - TCP STREAM은 async read 완료 뒤 `read_drain`에서 동기 `read_some()`을 최대 64회 반복한다.
  - 10,000 connection 64B echo에서는 한 callback이 read만 오래 점유하면 send wakeup과 다른 연결 처리가 밀릴 수 있다.
  - 반복 상한을 낮추면 I/O thread 공정성이 좋아져 tail latency와 echo throughput이 회복될 수 있다.

#### 검증

- `read_drain_max_loops=8`
  - build: `cmake --build core/build -j$(nproc)` 통과
  - test: `ctest --test-dir core/build --output-on-failure -R 'test_stream_|test_transport_matrix|test_multi_stream_server_reassembly|test_zmp_request_reply'`
  - 결과: 23/23 통과
  - perf report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222053_round26_stream_tcp_read_drain_8_probe.txt`
  - `STREAM/tcp 64B`: `334,241.8 ops/s`
- `read_drain_max_loops=0`
  - build: `cmake --build core/build -j$(nproc)` 통과
  - perf report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222147_round26_stream_tcp_read_drain_0_probe.txt`
  - `STREAM/tcp 64B`: `328,203.0 ops/s`

#### 판정

- 둘 다 baseline `400,124.6 ops/s`와 거리가 크다.
- `8`은 조용한 기본값 `321,878.0 ops/s` 대비 약간 낫지만, 목표 복구 후보로는 약하다.
- 후보 D는 source 변경을 되돌렸다.

### 후보 E: TCP speculative write native send

- 파일: `core/src/runtime/transports/tcp/tcp_transport.cpp`
- 변경 내용:
  - `tcp_transport_t::write_some()`에서 Boost.Asio `socket.write_some()` 대신 native `send()`를 사용한다.
  - Linux/Unix는 `MSG_DONTWAIT`와 `MSG_NOSIGNAL`을 사용한다.
  - Windows는 `send()` 결과와 `WSAGetLastError()`를 기존 errno 의미로 매핑한다.
- 가설:
  - TCP STREAM hot path의 `read_some()`은 이미 native `recv()`를 쓰지만, `write_some()`은 Boost.Asio wrapper를 사용한다.
  - 64B echo에서는 speculative write가 매우 자주 호출되므로 wrapper 비용을 줄이면 throughput이 오른다.

#### 검증

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_stream_|test_transport_matrix|test_multi_stream_server_reassembly|test_zmp_request_reply'`
  - 결과: 23/23 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round26_stream_tcp_native_send_probe`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222521_round26_stream_tcp_native_send_probe.txt`
  - `STREAM/tcp 64B`: `338,695.8 ops/s`
  - `--connect-concurrency 128` report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222603_round26_stream_tcp_native_send_connect128_probe.txt`
  - `STREAM/tcp 64B`: `334,574.0 ops/s`
- `ZLINK_ASIO_TCP_ASYNC_WRITE_SOME=1`:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222625_round26_stream_tcp_native_send_async_write_some_probe.txt`
  - `STREAM/tcp 64B`: `336,346.4 ops/s`

#### 판정

- native send는 기본 quiet rerun `321,878.0 ops/s` 대비 개선 신호가 있다.
- 하지만 baseline `400,124.6 ops/s`까지는 아직 부족하다.
- 후보 E는 보류하고, 추가 후보와 조합한 뒤 유지 여부를 다시 판단한다.

### 후보 F/G: STREAM dispatch atomic 완화와 packet frame fast path

- 후보 F:
  - `_dispatch_inflight` hot-path fetch add/sub를 `acq_rel`에서 `relaxed`로 낮췄다.
- 후보 G:
  - `stream_dispatch_packet_msg_from_io()`에서 한 payload 안에 완성된 packet frame이 있을 때 staging state를 거치지 않는 fast path를 추가했다.
- 검증:
  - build 통과
  - STREAM/transport 테스트 23/23 통과
  - perf report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_222942_round26_stream_tcp_native_send_relaxed_inflight_probe.txt`
  - `STREAM/tcp 64B`: `335,680.2 ops/s`
  - packet fast path report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_223227_round26_stream_tcp_packet_fastpath_probe.txt`
  - `STREAM/tcp 64B`: `319,624.0 ops/s`
- 판정:
  - F는 E 단독보다 개선이 없다.
  - G는 악화했다.
  - 후보 F/G는 source 변경을 되돌렸다.

### 추가 검증: STREAM/tcp 64B 후속 후보

- 기준 정정:
  - 목표 `400kops`는 `STREAM/tcp 64B` 기준이다.
  - `STREAM/ws 64B`는 별도 기준이므로 이 절의 성공/실패 판정에서 제외한다.
- 후보 E 인접 크기 재측정:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_223634_round26_stream_tcp_native_send_adjacent_sizes.txt`
  - `64B`: `335,484.6 ops/s`
  - `256B`: `329,170.6 ops/s`
  - `1024B`: `327,850.8 ops/s`
  - `4096B`: `294,079.6 ops/s`
- 후보 H: current-dispatch write credit 선갱신
  - 변경: `stream_dispatch_send_current_msg_from_io()`에서 `write_single_message...()` 전에 `refresh_write_credit()`을 호출했다.
  - test: STREAM/transport 테스트 23/23 통과
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_224107_round26_stream_tcp_credit_prefetch_probe.txt`
  - `STREAM/tcp 64B`: `328,002.8 ops/s`
  - 판정: 후보 E보다 낮고 목표와 거리가 크다. source 변경을 되돌렸다.
- 후보 I: encoder batch 64B env probe
  - env: `ZLINK_ASIO_STREAM_BATCH_SIZE=64`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_224316_round26_stream_tcp_batch64_zero_copy_probe.txt`
  - `STREAM/tcp 64B`: `325,298.4 ops/s`
  - 판정: raw encoder copy 하나만의 문제가 아니며, 작은 batch는 syscall/wakeup 증가로 손해일 가능성이 크다.
- 후보 J: 같은 I/O thread의 `activate_read` 직접 처리
  - 변경: `object_t::send_activate_read()`가 목적 pipe와 같은 thread이면 command mailbox 대신 즉시 `process_command()`를 호출하도록 했다.
  - test: STREAM/transport 테스트 23/23 통과
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_224600_round26_stream_tcp_direct_activate_read_probe.txt`
  - `STREAM/tcp 64B`: `323,431.2 ops/s`
  - 판정: 즉시 재진입이 처리량을 올리지 못했다. source 변경을 되돌렸다.
- 후보 E 재확인:
  - native send confirm report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_224759_round26_stream_tcp_native_send_confirm.txt`
  - `STREAM/tcp 64B`: `322,480.3 ops/s`
  - Boost write confirm report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_224921_round26_stream_tcp_boost_write_confirm.txt`
  - `STREAM/tcp 64B`: `323,199.3 ops/s`
  - 판정: 같은 부하대에서 native send의 안정 개선을 확인하지 못했다. 후보 E도 source 변경을 되돌렸다.
- 실행 조건 probe:
  - `--server-io-threads 8 --client-io-threads 8`: `299,961.6 ops/s`
  - `--server-io-threads 2 --client-io-threads 2`: `232,169.2 ops/s`
  - `--hwm 1024 --buf 1m`: `314,216.4 ops/s`
  - 판정: thread 수나 HWM/버퍼 조정으로 400k 복구가 되지 않는다.

#### 현재 판정

- `STREAM/tcp 64B` 기준 baseline은 `400,124.6 ops/s`이고, 현재 재측정은 대체로 `321k~323k ops/s` 수준이다.
- 이번 라운드에서 시도한 source 후보는 모두 목표 복구에 실패했거나 안정 개선을 확인하지 못해 되돌렸다.
- 기준 커밋 `cb605c6c1`에는 현재 ASIO proactor 경로가 없었다. 큰 회귀는 단일 micro-optimization보다 ASIO STREAM/TCP 작은 메시지 경로의 구조적 비용으로 보는 것이 맞다.
