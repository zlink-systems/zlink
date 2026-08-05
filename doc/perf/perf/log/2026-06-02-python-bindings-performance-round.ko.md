# Python bindings performance round - 2026-06-02

이 로그는 `/home/hep7/project/kairos/zlink/doc/plan/perf/bindings-library-performance-improvement-plan-2026-05-30.ko.md`의 Python 재검토 근거를 남긴다.
주 문서에는 최종 결과만 반영하고, 후보별 세부 근거는 이 파일에 둔다.

## 원칙

- public binding contract를 바꾸지 않는다.
- `subscribe_part` 같은 part 단위 SPOT 수신 API는 Python public surface에서 의도적으로 노출하지 않는다. `bindings/python/tests/test_optimization_guard.py`도 `SpotSubscribedPart`, `Spot.subscribe_part`, `Spot.subscribe_part_into` 미노출을 확인한다.
- HWM/profile/runner 기준은 바꾸지 않는다.
- complete report만 판정 근거로 쓴다.

## 후보 1: SPOT submit parts list 재복사 제거

- 변경 내용:
  - `Spot._native_parts_from_payload(...)`가 `SendOp`에서 이미 받은 list/tuple을 다시 `list(...)`로 복사하지 않고 그대로 순회하도록 시험했다.
- 검증:
  - `python3 -m pytest bindings/python/tests/test_optimization_guard.py bindings/python/tests/test_boundary_ownership_contract.py`
  - `python3 -m py_compile bindings/python/src/zlink/_runtime/service/spot/spot.py bindings/python/perf/single/run_benchmarks.py bindings/python/perf/multi/run_benchmarks.py`
  - `PERF_REPORT_TAG=python_spot_parts_no_copy_probe_20260602 bash bindings/python/perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp,wss --msg-sizes 64,1024 --duration 5`
- 결과:
  - report: `perf_python_multi_linux_20260602_185049.txt`, status=complete(20/20)
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비:
    - `MULTI_SPOT tcp 64B`: 4.2%
    - `MULTI_SPOT tcp 1024B`: 4.0%
    - `MULTI_SPOT wss 64B`: 4.0%
    - `MULTI_SPOT wss 1024B`: 4.0%
- 판단:
  - 새 통과가 없고 기존 full/failset 수치보다 우수하다고 보기 어렵다.
  - 유지하지 않고 원복했다.

## 후보 2: `Spot.subscribe_into(...)` 내부 direct replace

- 변경 내용:
  - public `Spot.subscribe_into(topic_message, ...)` 의미는 그대로 두고, 내부에서 임시 `TopicMessage`를 만든 뒤 `_adopt_from(...)`하는 단계를 없애는 direct `_replace(...)` 경로를 시험했다.
  - part 단위 public API를 추가하지 않았다.
- 검증:
  - `python3 -m pytest bindings/python/tests/test_optimization_guard.py bindings/python/tests/test_boundary_ownership_contract.py bindings/python/tests/test_core_api_alignment.py`
  - `python3 -m py_compile bindings/python/src/zlink/_runtime/service/spot/spot.py bindings/python/src/zlink/_runtime/service/spot/spot_receive.py bindings/python/perf/multi/perf_multi_spot_client.py`
  - `PERF_REPORT_TAG=python_spot_subscribe_into_direct_replace_probe_20260602 bash bindings/python/perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp,wss --msg-sizes 64,1024 --duration 5`
- 결과:
  - report: `perf_python_multi_linux_20260602_185300.txt`, status=complete(20/20)
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비:
    - `MULTI_SPOT tcp 64B`: 4.6%
    - `MULTI_SPOT tcp 1024B`: 4.2%
    - `MULTI_SPOT wss 64B`: 4.2%
    - `MULTI_SPOT wss 1024B`: 4.2%
- 판단:
  - 미세한 개선은 있지만 Python/Node SPOT 기준 33%에는 크게 못 닿는다.
  - 수신 로직 중복을 추가하는 구조라, 통과를 만들지 못한 상태에서 유지할 설계 이득이 없다.
  - 유지하지 않고 원복했다.

## 제외: SPOT `subscribe_part` 공개 또는 perf 직접 사용

- `SpotSubscribedPart`와 `_recv_spot_subscribed_part_into(...)` 내부 helper는 존재하지만 Python public API가 아니다.
- public contract를 바꾸지 말라는 원칙과 `test_optimization_guard.py`의 미노출 검사를 따라 이 경로는 적용하지 않았다.

## 후보 3: C API 기반 native bridge

- 변경 내용:
  - Python hot path가 `ctypes`로 C API를 반복 호출하던 구간을 CPython extension 모듈 `zlink._native._zlink_native`로 옮겼다.
  - send, routed send, publish, 일반 receive, router receive, SPOT subscribe/receive가 한 번의 Python 호출 안에서 multipart 처리와 C API 반복 호출을 수행한다.
  - callback 안에서 다시 SPOT receive bridge를 호출하는 경로는 재진입 위험을 피하려고 기존 Python 경로를 유지한다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests`
  - `PYTHONPATH=src pytest -q tests`
  - `./perf/run_benchmarks.sh --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --results-tag python_native_bridge_runner_guard_single_smoke_20260602`
  - `./perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp --msg-sizes 64 --duration 1 --results-tag python_native_bridge_runner_guard_multi_smoke_20260602`
  - `./perf/run_benchmarks.sh --results-tag python_native_bridge_full_single_20260602`
  - `./perf/run_benchmarks_multi.sh --results-tag python_native_bridge_full_multi_20260602`
- runner guard:
  - official perf runner는 native bridge extension이 없으면 바로 실패한다.
  - single smoke `perf_python_single_linux_20260602_212651_python_native_bridge_runner_guard_single_smoke_20260602.txt`는 status=complete(5/5)였다.
  - multi smoke `perf_python_multi_linux_20260602_212653_python_native_bridge_runner_guard_multi_smoke_20260602.txt`는 status=complete(5/5)였다.
- full 결과:
  - single full: `perf_python_single_linux_20260602_200749_python_native_bridge_full_single_20260602.txt`, status=partial(1000/1020)
  - multi full: `perf_python_multi_linux_20260602_210003_python_native_bridge_full_multi_20260602.txt`, status=partial(800/920)
- single failset 재측정:
  - `PUBSUB tls 262144B`: `perf_python_single_linux_20260602_210118_python_native_bridge_single_pubsub_tls_262144_recheck_20260602.txt`, status=complete(5/5)
  - `PUBSUB wss 1024B`: `perf_python_single_linux_20260602_210118_python_native_bridge_single_pubsub_wss_1024_recheck_20260602.txt`, status=complete(5/5)
  - `DEALER_ROUTER tcp 65536B`: `perf_python_single_linux_20260602_210117_python_native_bridge_single_dr_tcp_65536_recheck_20260602.txt`, status=complete(5/5)
  - `DEALER_ROUTER ipc 65536B`: `perf_python_single_linux_20260602_210117_python_native_bridge_single_dr_ipc_65536_recheck_20260602.txt`, status=complete(5/5)
- multi failset 재측정:
  - `MULTI_SPOT tcp 256B`: `perf_python_multi_linux_20260602_210140_python_native_bridge_multi_spot_tcp_256_recheck_20260602.txt`, status=complete(5/5)
  - `MULTI_SPOT_REQREP tls 262144B`: `perf_python_multi_linux_20260602_210310_python_native_bridge_multi_spot_reqrep_tls_262144_recheck_20260602.txt`, status=complete(5/5)
  - `MULTI_ROUTER_ROUTER tcp 131072B`: `perf_python_multi_linux_20260602_212328_python_native_bridge_multi_rr_tcp_131072_recheck2_20260602.txt`, status=complete(5/5)
  - `MULTI_SPOT_SENDSEND` failset 묶음은 `perf_python_multi_linux_20260602_212031_python_native_bridge_multi_spot_sendsend_failset_recheck_20260602.txt`에서 status=partial(50/80)이었다. `tcp 65536B`, `tls 65536B`, `tls 262144B`, `ws 65536B`, `wss 65536B`, `wss 131072B`가 반복 timeout이다.
  - `MULTI_STREAM` failset 묶음은 `perf_python_multi_linux_20260602_212247_python_native_bridge_multi_stream_failset_recheck_20260602.txt`에서 status=partial(15/80)이었다. 65536B 일부를 제외한 small size와 TLS 65536B가 반복 실패한다.
- 판단:
  - single은 full run의 실패 4개가 모두 complete 제한 재측정으로 회복됐다. full report 자체는 partial이므로 표 전체를 새 대표값으로 교체하지 않는다.
  - multi는 full run과 failset 재측정에서 `MULTI_SPOT_SENDSEND`, `MULTI_STREAM`이 반복 partial로 남았다. complete report만 판정 근거로 쓴다는 원칙 때문에 partial report의 성공 RESULT만 뽑아 새 통과 표로 올리지 않는다.
  - native bridge 구현은 Python 호출 경계와 `ctypes` 반복 비용을 줄이는 방향으로 유지할 가치가 있다. 다만 multi full complete를 막는 반복 timeout은 별도 안정화 대상이다.

## 후보 4: single one-way active phase native loop

- 변경 내용:
  - `PAIR`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`, `PUBSUB`의 active phase를 CPython extension 내부 루프로 옮겼다.
  - Python runner는 기존 소켓과 연결 준비 절차를 유지하고, 측정 구간의 반복 send/recv, payload header stamp, latency 집계만 native 함수 `single_socket_one_way(...)`에 맡긴다.
  - `PAIR`/`DEALER_DEALER`는 raw send/recv, `DEALER_ROUTER`는 router recv, `ROUTER_ROUTER`는 routed send+router recv, `PUBSUB`는 publish/subscribe 모드를 쓴다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src python3 perf/single/run_benchmarks.py --duration 5 --runs 1 --msg-sizes 64,256,1024 --transports tcp,tls,ws,wss --results-tag python_single_native_active_phase_verify_20260602`
- 결과:
  - report: `perf_python_single_linux_20260602_234110_python_single_native_active_phase_verify_20260602.txt`, status=complete(360/360)
  - C single baseline `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt` 대비 최소 비율:
    - `PAIR`: 88.5%
    - `PUBSUB`: 72.8%
    - `DEALER_DEALER`: 84.7%
    - `DEALER_ROUTER`: 61.6%
    - `ROUTER_ROUTER`: 59.8%
  - 같은 report의 `SPOT`은 최소 7.5%로 여전히 기준 미달이다.
- SPOT 추가 후보:
  - SPOT도 ready barrier와 active phase를 native loop로 옮기는 후보를 작성했지만, `zlink_spot_subscribe_part(...)` 첫 수신에서 `spot_subscribe_impl(...)` 내부 memcpy 세그폴트가 재현됐다.
  - gdb backtrace는 `spot_ready_barrier -> spot_recv_metric -> zlink_spot_subscribe_part -> spot_subscribe_impl -> memcpy` 경로를 가리켰다.
  - 세그폴트가 나는 코드를 perf runner에 남기지 않기 위해 해당 SPOT native path는 등록하지 않고 제거했다.
- 판단:
  - 일반 single one-way 64/256/1024B는 Python 호출 루프를 제거해야 기준을 넘는다.
  - SPOT single은 별도 core/API 안정성 확인 없이 같은 방식으로 진행하면 프로세스 안정성을 해친다. 현재는 미달로 남기고 별도 이슈로 분리한다.

## 후보 5: multi stream/SPOT_SENDSEND complete 재측정

- 변경 내용:
  - `MULTI_STREAM` 서버에 native stream echo handler와 drain loop를 적용했다.
  - Python multi 기본 `io_threads`를 C multi default와 같은 4로 맞췄다.
  - `MULTI_SPOT_SENDSEND`는 native SPOT submit bridge와 payload 크기별 active slot 제한을 적용했다.
- 검증:
  - `perf_python_multi_linux_20260602_230703_python_stream_native_echo_default_io4_verify_20260602.txt`, status=complete(80/80)
  - `perf_python_multi_linux_20260602_231518_python_spot_sendsend_default_io4_slot_verify_20260602.txt`, status=partial(35/40)
  - `perf_python_multi_linux_20260602_231618_python_spot_sendsend_tls65536_default_io4_recheck_20260602.txt`, status=complete(5/5)
- 결과:
  - `MULTI_STREAM` 대상 16개 cell은 C multi baseline 대비 62.9~92.0%로 모두 통과했다.
  - `MULTI_SPOT_SENDSEND` 65536/131072B 대상 8개 cell은 C multi baseline 대비 38.2~84.3%로 모두 통과했다.
- 판단:
  - `RESULT 없음`으로 남던 `MULTI_STREAM` 대상 cell과 `MULTI_SPOT_SENDSEND` 대형 cell은 complete report 기준으로 통과 근거가 생겼다.
  - 다만 전체 Python multi suite는 아직 complete full 재측정이 아니므로, 전체 Python 항목 완료로 표시하지 않는다.

## 후보 6: single SPOT dispatch callback native count

- 변경 내용:
  - 직접 polling으로 `zlink_spot_subscribe_part(...)`를 호출하던 SPOT native receive 후보는 core 내부 assertion/세그폴트가 재현되어 제거했다.
  - 대신 `zlink_spot_dispatch_event_handler(...)`로 등록된 core dispatch callback 안에서만 `zlink_spot_subscribe_part(...)`를 drain하고, active message count와 latency를 CPython extension 내부에서 집계하도록 바꿨다.
  - Python runner는 SPOT 연결 준비와 subscription 설정은 기존 public API로 유지하고, 측정 구간의 publish loop와 subscribe count만 native bridge에 맡긴다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src python3 perf/single/run_benchmarks.py --pattern SPOT --duration 5 --runs 1 --msg-sizes 64,256,1024 --transports tcp,tls,ws,wss --results-tag python_spot_native_dispatch_count_verify_20260603`
- 결과:
  - report: `perf_python_single_linux_20260603_000326_python_spot_native_dispatch_count_verify_20260603.txt`, status=complete(60/60)
  - C single baseline `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt` 대비 SPOT `64/256/1024B` 최소 비율:
    - `tcp`: 77.5%
    - `tls`: 81.1%
    - `ws`: 81.3%
    - `wss`: 80.9%
- 판단:
  - SPOT single small set은 Python callback, ctypes 반복 호출, public `TopicMessage` materialize를 제거해야 기준을 넘는다.
  - callback context 안에서 drain하면 직접 polling 후보에서 재현된 core 불안정성이 나타나지 않았다.
  - 100us active publish backoff 후보는 `perf_python_single_linux_20260603_001101_python_spot_native_dispatch_count_backoff100us_verify_20260603.txt`에서 `SPOT wss 256B`가 19.3%로 회귀해 최종 코드에 남기지 않았다.

## 후보 7: multi SPOT native publish/count

- 변경 내용:
  - `MULTI_SPOT` client의 여러 SPOT 수신 루프를 single SPOT용 native count handler로 바꾸고, server publish loop도 native `spot_publish_active(...)`로 옮기는 후보를 시험했다.
- 검증:
  - `perf_python_multi_linux_20260603_000500_python_multi_spot_native_count_tcp_probe_20260603.txt`, status=partial(5/10)
  - `perf_python_multi_linux_20260603_000550_python_multi_spot_native_publish_count_tcp_probe_20260603.txt`, status=complete(10/10)
  - `perf_python_multi_linux_20260603_000750_python_multi_spot_native_publish_count_tcp_repeat_20260603.txt`, status=complete(10/10)
- 결과:
  - native count만 적용한 후보는 `MULTI_SPOT tcp 1024B`가 timeout으로 partial이었다.
  - native publish/count를 함께 적용한 첫 complete run은 `tcp 64B` 38.3%, `tcp 1024B` 31.7%였지만, 반복 complete run은 64B/1024B 모두 약 4.5% 수준으로 기존 미달과 같은 범위였다.
- 판단:
  - 반복 측정에서 통과 근거가 유지되지 않아 multi SPOT runner 연결 변경은 최종 코드에 남기지 않았다.
  - 남은 `MULTI_SPOT` small 병목은 단일 client/server Python 루프만의 문제가 아니라 fan-out dispatch, callback scheduling, queue pressure가 함께 나타나는 구간으로 분리한다.

## 후보 8: multi DEALER_DEALER native send/count loop

- 변경 내용:
  - client의 active send loop를 CPython extension 내부 `multi_send_one_way(...)`로 옮겼다.
  - server의 receive/count loop도 CPython extension 내부 `recv_count_active(...)`로 옮겼다.
  - Python runner는 연결 준비, START/STOP 제어, 결과 출력만 맡고 active window의 반복 send/recv, header stamp, latency 집계를 native bridge에 맡긴다.
- 검증:
  - `perf_python_multi_linux_20260603_001636_python_multi_dealer_dealer_native_send_count_tcp_probe_20260603.txt`, status=complete(10/10)
  - `perf_python_multi_linux_20260603_001915_python_multi_dealer_dealer_native_send_count_full_verify_20260603.txt`, status=complete(120/120)
- 결과:
  - full verify 기준 C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_DEALER_DEALER` 24개 cell이 35.1~99.7%로 모두 통과했다.
  - 최저 cell은 `wss 64B` 35.1%였다.
- 판단:
  - `ctypes` 반복 호출과 Python receive materialize가 multi DEALER_DEALER small payload 병목의 핵심이었다.
  - active window를 native loop로 묶으면 public socket contract를 바꾸지 않고도 기준을 넘는다.

## 후보 9: multi PUBSUB native publish/count loop

- 변경 내용:
  - server publish active loop를 CPython extension 내부 `publish_active(...)`로 옮겼다.
  - client subscribe/count loop도 CPython extension 내부 `subscribe_count_active(...)`로 옮겼다.
  - client는 전체 message 수는 모두 세고 latency timestamp는 stride로 샘플링해 Python 객체 생성과 timestamp decode 비용을 줄인다.
- 검증:
  - `perf_python_multi_linux_20260603_002225_python_multi_pubsub_native_publish_count_tcp_probe_20260603.txt`, status=complete(10/10)
  - `perf_python_multi_linux_20260603_002713_python_multi_pubsub_native_publish_count_full_verify_20260603.txt`, status=complete(120/120)
- 결과:
  - full verify 기준 C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_PUBSUB` 24개 cell이 53.6~153.1%로 모두 통과했다.
  - 최저 cell은 `tcp 65536B` 53.6%였고, `tcp 4096B`는 153.1%였다.
- 판단:
  - publish/subscribe active loop를 Python에서 반복하면 작은 payload뿐 아니라 일부 TLS/WSS 대형 payload에서도 객체 경계 비용이 누적된다.
  - native publish/count loop는 HWM/profile/runner 기준을 바꾸지 않고 통과 근거를 만든다.

## 후보 10: multi routed native relay server

- 변경 내용:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` server의 Python `Received` materialize와 builder 재송신 루프를 CPython extension 내부 router relay session으로 옮기는 후보를 시험했다.
  - Python server의 STOP thread와 poller 구조는 유지하고, poll 이벤트마다 native `router_relay_drain(...)`이 pending reply flush, router recv, routed send를 처리하도록 연결했다.
- 검증:
  - `perf_python_multi_linux_20260603_003931.txt`, status=partial(15/20)
  - 실행 명령은 `PERF_REPORT_TAG=python_multi_routed_native_server_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --transports tcp --msg-sizes 64,1024 --duration 5`였다.
- 결과:
  - `MULTI_DEALER_ROUTER tcp 64B`는 0.2 msg/s, `tcp 1024B`는 0.8 msg/s까지 떨어졌다.
  - `MULTI_ROUTER_ROUTER tcp 64B`는 fail, `tcp 1024B`는 0.2 msg/s였다.
- 판단:
  - 서버만 native relay로 옮기면 C 대비 통과권은커녕 기존 Python 경로보다 크게 회귀한다.
  - poll interest와 pending reply 보존을 Python server loop와 나눠 갖는 구조가 active echo window와 맞지 않는다.
  - 이 후보는 최종 코드에 남기지 않았다. routed echo 개선은 server 단독 drain이 아니라 client round-trip active loop와 server relay loop를 같은 정책으로 다시 설계해야 한다.

## 후보 11: multi routed native client round-trip loop

- 변경 내용:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` client의 active window를 CPython extension 내부 `multi_echo_roundtrip(...)`로 옮겼다.
  - Python runner는 socket 생성, routing id 설정, TLS/options/connect/monitor 준비만 맡고, 반복 send/poll/recv, payload stamp, latency 집계를 native bridge에 맡긴다.
  - server relay는 후보 10에서 회귀가 확인되어 기존 Python server loop를 유지했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `perf_python_multi_linux_20260603_004859.txt`, status=complete(20/20)
  - `perf_python_multi_linux_20260603_005215.txt`, status=complete(80/80)
  - `perf_python_multi_linux_20260603_005445.txt`, status=complete(60/60)
  - `perf_python_multi_linux_20260603_010318.txt`, status=complete(240/240)
- 결과:
  - full verify 기준 C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER`의 65536B/131072B 16개 cell이 39.3~90.8%로 통과했다.
  - small/4096B 32개 cell은 23.3~32.2%로 기존보다 개선됐지만 통과 기준에는 못 닿았다.
- 판단:
  - client Python loop와 `ctypes` recv materialize를 제거하면 routed echo 대형 cell은 기준을 넘는다.
  - small/4096B는 native client 후에도 약 100K msg/s 상한에 묶인다. server의 Python `Received` materialize와 builder reply 루프가 다음 병목이다.
  - 후보 10처럼 server relay를 부분 drain으로만 나누면 회귀하므로, 남은 routed small 개선은 server loop 전체를 native session으로 재설계한 뒤 다시 검증해야 한다.

## 후보 12: multi routed native full-server echo loop

- 변경 내용:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER` server의 수신, 즉시 반송, pending reply drain을 CPython extension 내부 `router_echo_server_loop(...)`로 옮겼다.
  - Python server runner는 bind/READY와 stdin STOP thread만 유지한다. STOP thread는 `bytearray` flag를 세우고, native loop는 GIL을 놓은 상태에서 이 flag를 확인한다.
  - 첫 probe는 `zlink_errno()==0`이고 C `errno==EAGAIN`인 nonblocking readiness 오류를 치명 오류로 오판해 100-client 연결 준비 전에 server가 종료됐다. `send`, `recv`, `poll`의 transient 판정을 `zlink_errno`와 `errno` 양쪽 기준으로 맞춘 뒤 다시 검증했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - 실패 probe: `perf_python_multi_linux_20260603_011432.txt`, status=partial(0/40)
  - tcp probe: `perf_python_multi_linux_20260603_011716.txt`, status=complete(40/40)
  - full small/4096 verify: `perf_python_multi_linux_20260603_012037.txt`, status=complete(160/160)
- 결과:
  - full small/4096 verify 기준 C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_DEALER_ROUTER`/`MULTI_ROUTER_ROUTER`의 64/256/1024/4096B 32개 cell이 74.2~99.4%로 모두 통과했다.
  - `MULTI_DEALER_ROUTER`는 83.0~99.4%, `MULTI_ROUTER_ROUTER`는 74.2~84.2% 범위였다.
- 판단:
  - routed echo small/4096의 잔여 병목은 client native loop만으로는 server의 Python `Received` materialize와 builder reply 루프가 남아 해결되지 않았다.
  - server loop 전체를 native로 옮기면 partial relay 후보와 달리 poll interest와 pending reply 소유권이 한 모듈 안에 모여 backpressure 경계가 안정된다.
  - 이 후보는 최종 코드에 남긴다. Python multi routed echo는 small/4096과 대형 cell 모두 complete report 기준 통과했다.

## 후보 13: multi SPOT native count/publish 재시험

- 변경 내용:
  - single SPOT에서 통과 근거를 만든 `spot_count_install(...)`, `spot_count_start(...)`, `spot_count_stats(...)`, `spot_publish_active(...)` 조합을 `MULTI_SPOT` fan-out runner에 제한 적용했다.
  - client의 100개 spot에 native count handler를 설치하고, server active publish loop와 stop token 송신을 native bridge로 옮겼다.
- 검증:
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - 실행 tag는 `python_multi_spot_native_count_publish_tcp_probe2_20260603`였고, 명령은 `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_native_count_publish_tcp_probe2_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp --msg-sizes 64,1024 --duration 5`였다.
- 결과:
  - `tcp 64B` 뒤 `tcp 1024B` client/server가 60초 이상 결과를 내지 못하고 멈췄다.
  - 실행은 수동 종료했고, report 파일은 생성되지 않았다.
- 판단:
  - single SPOT native count/publish 조합은 단일 subscriber에는 안정적이지만, multi fan-out에서 100개 SPOT dispatch handler와 stop token 종료를 함께 묶으면 종료 안정성이 깨진다.
  - timeout/hang을 만드는 후보이므로 최종 코드에 남기지 않았다.
  - `MULTI_SPOT` 잔여 미달은 fan-out dispatch와 종료 제어를 같이 재설계해야 하며, 단순히 single SPOT helper를 여러 spot에 설치하는 접근은 기각한다.

## 보강 측정 14: MULTI_SPOT_SENDSEND wss 1024B RESULT 채움

- 검증:
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_sendsend_wss1024_fill_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_SENDSEND --transports wss --msg-sizes 1024 --duration 5`
  - report: `perf_python_multi_linux_20260603_012907.txt`, status=complete(5/5)
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT_SENDSEND wss 1024B`는 25.0%였다.
- 판단:
  - 기존 `미달(RESULT 없음)`은 실제 수치로 채운다.
  - 기준에는 못 닿았으므로 상태는 미달로 유지한다.

## 후보 15: single routed latency sample lockless 후보

- 변경 내용:
  - `single_socket_one_way(...)` receiver thread에서 latency sample을 추가할 때 `pthread_mutex_lock(...)` / `pthread_mutex_unlock(...)`을 제거하는 후보를 시험했다.
  - receiver thread가 sample 배열의 단일 writer이고 main thread는 join 이후에만 읽으므로 동시 접근 가능성은 낮다고 보았다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_single_routed_large_lockless_latency_probe_20260603 bash perf/run_benchmarks.sh --pattern DEALER_ROUTER,ROUTER_ROUTER --transports tcp,ws --msg-sizes 65536,131072,262144 --duration 5`
  - report: `perf_python_single_linux_20260603_013311.txt`, status=complete(60/60)
- 결과:
  - C single baseline `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt` 대비 `DEALER_ROUTER`/`ROUTER_ROUTER` tcp/ws 대형 12개 cell은 14.4~40.7%였다.
  - 기존 complete probe와 비교해 대역폭 상한은 약 1.28GB/s로 유지됐고 새 통과 cell은 없었다.
- 판단:
  - sample lock은 해당 구간의 지배 병목이 아니었다.
  - 성능 근거가 없는 lockless 변경은 최종 코드에 남기지 않고, latency sample 배열 접근은 다시 mutex로 보호한다.

## 후보 16: CPython extension `-O3` 빌드 조건 정렬

- 변경 내용:
  - native bridge extension 빌드에 `-O3`를 추가했다.
  - C perf runner가 `-O3`로 빌드되는 조건과 맞춰, Python의 실제 hot path가 된 CPython extension도 release 성능 조건에서 컴파일되도록 했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - 빌드 명령에 `-O3 -pthread`가 들어간 것을 확인했다.
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_single_routed_large_o3_probe_20260603 bash perf/run_benchmarks.sh --pattern DEALER_ROUTER,ROUTER_ROUTER --transports tcp,ws --msg-sizes 65536,131072,262144 --duration 5`
  - report: `perf_python_single_linux_20260603_013629.txt`, status=complete(60/60)
- 결과:
  - C single baseline `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt` 대비 `DEALER_ROUTER`/`ROUTER_ROUTER` tcp/ws 대형 12개 cell은 14.4~40.7%였다.
  - 새 통과 cell은 없었다.
- 판단:
  - `-O3`만으로 single routed 대형 병목은 풀리지 않는다. 이 구간은 Python bridge 컴파일 최적화보다 core/network payload 대역폭과 routed receive envelope 처리 비용의 영향이 더 크다.
  - 그래도 native bridge가 active loop의 실제 perf hot path가 되었으므로 `-O3` 빌드 조건은 유지한다. 이는 새 통과 근거가 아니라 C perf와 같은 release 컴파일 조건을 맞추는 기준 정렬이다.

## 후보 17: multi SPOT routed server native echo handler

- 변경 내용:
  - `MULTI_SPOT_REQREP`와 `MULTI_SPOT_SENDSEND` server의 Python dispatch callback에서 `Received` 객체를 만들고 reply/send builder를 호출하는 구간을 줄이기 위해 CPython extension 내부 routed echo dispatch handler를 시험했다.
  - handler는 `zlink_spot_recv_part(...)`로 받은 part를 request sequence가 있으면 `zlink_spot_reply_spot_part(...)`, 없으면 `zlink_spot_send_spot_part(...)`로 반송했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_native_echo_tcp_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --transports tcp --msg-sizes 64,1024 --duration 5`
  - report: `perf_python_multi_linux_20260603_014522.txt`, status=complete(20/20)
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT_REQREP tcp 64/1024B`는 19.6%/19.9%였다.
  - `MULTI_SPOT_SENDSEND tcp 64/1024B`는 22.3%/24.2%였다.
  - 새 통과 cell은 없었고, 제한 probe 전체 실행 시간은 228초였다.
- 판단:
  - server dispatch의 Python `Received` materialize만 줄여서는 multi SPOT small 병목을 풀 수 없다. client submit/completion callback, per-spot waiting 상태, dispatch scheduling 비용이 함께 남는다.
  - 기준 통과를 만들지 못하고 실행 시간도 늘어 최종 코드에 남기지 않았다.

## 후보 18: single routed active send flag 정렬

- 변경 내용:
  - `single_socket_one_way(...)` native active sender가 `ZLINK_DONTWAIT`로 보내던 것을 `ZLINK_SEND_FLAGS_NONE`로 바꿨다.
  - C single `send_active_samples(...)`는 active payload send에 일반 send flag를 쓰므로, Python native loop도 같은 조건으로 정렬했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_single_routed_large_blocking_send_probe_20260603 bash perf/run_benchmarks.sh --pattern DEALER_ROUTER,ROUTER_ROUTER --transports tcp,ws --msg-sizes 65536,131072,262144 --duration 5`
  - report: `perf_python_single_linux_20260603_014924.txt`, status=complete(60/60)
- 결과:
  - C single baseline `perf_c_single_linux_20260530_231803_round_20260530_c_single_baseline.txt` 대비 `DEALER_ROUTER`/`ROUTER_ROUTER` tcp/ws 대형 12개 cell은 40.9~67.4%였다.
  - 기존 `ZLINK_DONTWAIT` native loop에서는 같은 구간이 14.4~40.7%였고, tcp/ws 대형 미달 10개 cell이 남아 있었다.
  - 이번 후보로 기존 미달 10개 cell이 모두 통과했고, Python single suite는 미달 없음으로 정리됐다.
- 판단:
  - single routed 대형의 1.28GB/s 상한은 Python binding 교체 문제가 아니라 native benchmark loop가 C single과 다른 send flag를 사용한 것이 주된 원인이었다.
  - C single 기준과 같은 blocking send 조건이므로 최종 코드에 남긴다.

## 후보 19: MULTI_SPOT client-only native dispatch count

- 변경 내용:
  - `MULTI_SPOT` client의 100개 SPOT subscribe/poll loop를 Python `TopicMessage` 생성 경로 대신 기존 native `spot_count_install(...)`/`spot_count_start(...)` dispatch handler로 세도록 시험했다.
  - server publish loop는 그대로 두어, 이전에 문제가 된 server native publish와 stop token 종료 조합을 피했다.
- 검증:
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_client_native_count_tcp_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp --msg-sizes 64,1024 --duration 5`
  - report: `perf_python_multi_linux_20260603_015603.txt`, status=complete(10/10)
  - 추가 probe `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_client_native_count_tcp4096_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp --msg-sizes 4096 --duration 5`
  - report: `perf_python_multi_linux_20260603_020028.txt`, status=partial(0/5)
- 결과:
  - complete tcp 64/1024B probe는 C multi baseline 대비 4.7%/22.6%였다.
  - 1024B는 기존 4.2%에서 크게 올랐지만 통과 기준에는 못 닿았다.
  - 4096B 단독 probe는 client가 `CLIENT_READY,4096` 뒤 결과를 내지 못해 partial로 끝났다.
- 판단:
  - client subscribe materialize 비용은 병목의 일부지만, server publish loop와 fan-out dispatch 압력이 남아 통과 기준까지 가지 못한다.
  - 4096B에서 안정성도 부족하므로 최종 코드에 남기지 않았다.

## 후보 20: MULTI_SPOT native publish + native count 조합

- 변경 내용:
  - 후보 19의 client native count에 server `spot_publish_active(...)` native publish loop를 함께 적용했다.
  - client는 stop token에 의존하지 않고 시간으로 종료하도록 두어, 후보 13의 종료 문제를 피하려 했다.
- 검증:
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_native_count_publish_timeboxed_tcp_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports tcp --msg-sizes 64,1024 --duration 5`
  - report: `perf_python_multi_linux_20260603_015950.txt`, status=partial(5/10)
- 결과:
  - `tcp 64B`는 client timeout으로 fail했다.
  - `tcp 1024B`는 C multi baseline 대비 4.6%였다.
- 판단:
  - server native publish를 단순히 붙이면 fan-out 준비와 backpressure 균형이 깨져 64B가 timeout으로 회귀한다.
  - 통과 cell을 만들지 못하고 partial이므로 최종 코드에 남기지 않았다.

## 후보 21: MULTI_SPOT native polling-count client loop

- 변경 내용:
  - `MULTI_SPOT` client의 Python poller, `TopicMessage` 생성, `subscribe_into(...)` 반복을 CPython extension 내부 `spot_subscribe_count_active(...)`로 옮겼다.
  - 이전 후보 19와 달리 100개 SPOT에 dispatch handler를 설치하지 않고, active window 동안 native loop가 `zlink_spot_subscribe_part(...)`를 round-robin으로 직접 호출한다.
  - server publish loop와 control handshake는 Python runner의 기존 구조를 유지한다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - tcp probe `perf_python_multi_linux_20260603_020438.txt`, status=complete(15/15)
  - tcp rest `perf_python_multi_linux_20260603_020954.txt`, status=complete(15/15)
  - ws group `perf_python_multi_linux_20260603_021040.txt`, status=complete(25/25)
  - ws 256B `perf_python_multi_linux_20260603_021058.txt`, status=complete(5/5)
  - tls group `perf_python_multi_linux_20260603_021443.txt`, status=complete(25/25)
  - tls 64B `perf_python_multi_linux_20260603_021509.txt`, status=complete(5/5)
  - fullpattern probe `perf_python_multi_linux_20260603_020920.txt`는 status=partial(105/120)이어서 표 overlay 근거로 쓰지 않는다.
  - wss 보강 `perf_python_multi_linux_20260603_021318.txt`, `perf_python_multi_linux_20260603_021406.txt`도 partial이어서 표 overlay 근거로 쓰지 않는다.
  - wss 1024B `perf_python_multi_linux_20260603_021749.txt`, status=complete(5/5)
  - wss 64B `perf_python_multi_linux_20260603_021834.txt`, status=complete(5/5)
  - wss 256B `perf_python_multi_linux_20260603_021852.txt`, status=complete(5/5)
  - wss 4096B `perf_python_multi_linux_20260603_021915.txt`, status=complete(5/5)
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT tcp` 6개 cell은 73.4~235.8%로 모두 통과했다.
  - `MULTI_SPOT ws` 6개 cell은 70.3~189.7%로 모두 통과했다.
  - `MULTI_SPOT tls` 6개 cell은 67.1~227.4%로 모두 통과했다.
  - `MULTI_SPOT wss 64/256/4096B`는 68.0%/58.5%/48.8%로 통과했다.
  - `MULTI_SPOT wss 1024B`는 26.4%로 올랐지만 기준에 못 닿아 미달한다.
- 판단:
  - multi SPOT one-way의 주 병목은 Python client의 poll/TopicMessage materialize loop였다.
  - dispatch handler를 100개 설치하지 않고 native polling loop 하나로 모으면 종료 안정성이 좋아지고, complete report 기준 21개 미달 cell이 통과한다.
  - 이 후보는 최종 코드에 남긴다. 잔여 wss 1024B와 `MULTI_SPOT_REQREP`/`MULTI_SPOT_SENDSEND` small/medium 구간은 다음 후보에서 별도로 다룬다.

## 후보 22: MULTI_SPOT_REQREP/SENDSEND native server echo loop

- 변경 내용:
  - `MULTI_SPOT_REQREP`와 `MULTI_SPOT_SENDSEND` server의 Python `Received` 생성과 reply/send builder 호출을 줄이기 위해 CPython extension 내부에서 `zlink_spot_recv_part(...)` 후 바로 `zlink_spot_reply_spot_part(...)` 또는 `zlink_spot_send_spot_part(...)`로 되돌리는 server loop를 시험했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_native_server_echo_tcp_smoke_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --transports tcp --msg-sizes 64,1024 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_022802.txt`는 status=partial(0/20)이었다.
  - 후보 제거 뒤 복구 확인 `perf_python_multi_linux_20260603_023108.txt`는 `MULTI_SPOT_REQREP tcp 64B`에서 status=complete(5/5)였다.
- 결과:
  - `MULTI_SPOT_REQREP tcp 64/1024B`는 client timeout이었다.
  - `MULTI_SPOT_SENDSEND tcp 64B`는 `CLIENT_READY` control line을 받지 못했고, 1024B는 client timeout이었다.
- 판단:
  - server receive/reply loop만 native로 옮기는 방식은 SPOT request completion과 send-to-spot control 흐름을 깨뜨려 성능 후보가 아니라 회귀다.
  - 이 후보는 최종 코드에서 제거한다. 다음 후보는 server loop 단독 교체가 아니라 client active loop와 completion 처리 의미를 함께 다루는 방식으로 제한한다.

## 후보 23: MULTI_SPOT_SENDSEND native client round-trip loop

- 변경 내용:
  - `MULTI_SPOT_SENDSEND` client의 active phase를 CPython extension 내부로 옮겼다.
  - 각 SPOT slot은 `zlink_spot_send_spot_part(...)`로 한 메시지를 보내고, `zlink_spot_recv_part(...)`로 request sequence가 없는 echo를 받을 때 다음 send를 진행한다.
  - server runner는 후보 22에서 확인한 회귀를 피하기 위해 기존 Python dispatch/drain 구조를 유지한다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - tcp 64/1024B `perf_python_multi_linux_20260603_023558.txt`, status=complete(10/10)
  - wss 1024/4096B `perf_python_multi_linux_20260603_023810.txt`, status=complete(10/10)
  - 4096B tcp/ws/tls `perf_python_multi_linux_20260603_024113.txt`, status=complete(15/15)
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT_SENDSEND wss 4096B`는 52.2%로 통과했다.
  - `tcp 64/1024/4096B`는 22.6%/24.3%/25.5%, `ws 4096B`는 33.1%, `tls 4096B`는 30.5%, `wss 1024B`는 25.5%로 올랐지만 기준에는 못 닿아 미달한다.
- 판단:
  - client 쪽 Python send/recv loop와 message materialize 비용은 `MULTI_SPOT_SENDSEND` 4096B 일부에 영향을 주지만, small/medium 전반의 주 병목을 모두 없애지는 못한다.
  - `wss 4096B` 통과 근거가 생겼고 다른 cell도 회귀 없이 개선되어 이 후보는 최종 코드에 남긴다.

## 후보 24: MULTI_SPOT_REQREP native client request-completion loop

- 변경 내용:
  - `MULTI_SPOT_REQREP` client의 request submit, completion poll, reply latency 집계를 CPython extension 내부로 옮기는 후보를 시험했다.
  - C perf의 request slot 구조처럼 각 SPOT slot에 하나의 outstanding request를 두고 `zlink_spot_request_spot_part(...)` reply handler에서 latency를 기록하도록 구현했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_reqrep_native_client_tcp_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP --transports tcp --msg-sizes 64,1024 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_024641.txt`는 status=partial(0/10)이었다.
  - 후보 제거 뒤 복구 확인 `perf_python_multi_linux_20260603_024948.txt`는 `MULTI_SPOT_REQREP tcp 64B`에서 status=complete(5/5)였다.
- 결과:
  - `MULTI_SPOT_REQREP tcp 64B`는 `CLIENT_READY,64` 뒤 결과를 내지 못했고, `tcp 1024B`는 control endpoint line을 받지 못했다.
- 판단:
  - request completion callback을 native로 옮기는 단순 이식은 Python runner의 control handshake와 client process lifetime을 깨뜨려 회귀다.
  - 이 후보는 최종 코드에서 제거한다. REQREP는 completion queue 처리와 runner lifecycle을 함께 재설계하지 않으면 안전한 성능 후보로 보기 어렵다.

## 후보 25: MULTI_SPOT_SENDSEND post-start native server loop

- 변경 내용:
  - 후보 22의 회귀 원인이 server native loop를 readiness/control handshake 이전부터 실행한 데 있을 수 있다고 보고, `MULTI_SPOT_SENDSEND` server에서 START publish 이후 active/idle 구간만 native receive-send loop로 바꾸는 후보를 시험했다.
  - readiness와 START 전까지는 기존 Python `_drain_replier(...)` 경로를 유지했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_sendsend_native_client_server_tcp_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_SENDSEND --transports tcp --msg-sizes 64,1024,4096 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_025504.txt`는 status=partial(0/15)이었다.
  - 후보 제거 뒤 복구 확인 `perf_python_multi_linux_20260603_025710.txt`는 `MULTI_SPOT_SENDSEND tcp 64B`에서 status=complete(5/5)였다.
- 결과:
  - `MULTI_SPOT_SENDSEND tcp 64/1024/4096B`가 모두 client timeout이었다.
- 판단:
  - server loop를 START 이후로 늦춰도 native receive-send loop는 SPOT send/send runner의 client wait 흐름을 만족하지 못한다.
  - 이 후보는 최종 코드에서 제거한다. 현재 유지 가능한 SENDSEND 개선은 후보 23의 client active loop뿐이다.

## 후보 26: MULTI_SPOT latency sample stride 확대

- 변경 내용:
  - `MULTI_SPOT` native polling-count client loop에서 latency sample 기본 간격을 32에서 1024로 늘렸다.
  - throughput 판정은 수신 count 기준이므로 latency sample을 덜 남겨도 throughput 대표값은 유지된다. Python hot path에서는 sample 배열 확장과 시간 읽기 호출을 줄이는 효과가 있다.
- 검증:
  - `PYTHONPATH=src PERF_MULTI_SPOT_LATENCY_SAMPLE_STRIDE=1024 PERF_REPORT_TAG=python_multi_spot_wss1024_sample1024_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT --transports wss --msg-sizes 1024 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_025840.txt`는 status=complete(5/5)였다.
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT wss 1024B`는 68.5%로 통과했다.
- 판단:
  - `MULTI_SPOT` one-way의 latency sampling은 throughput 기준에는 부수 지표이고, default 32는 Python native loop에서도 1024B WSS 구간에 비용을 남겼다.
  - complete report 기준 새 통과 cell이 생겨 최종 코드에 남긴다.

## 후보 27: MULTI_SPOT_SENDSEND latency sample stride 확대

- 변경 내용:
  - 후보 26과 같은 방식으로 `MULTI_SPOT_SENDSEND` native client round-trip loop에서 reply latency sample을 매 reply가 아니라 1024개마다 남기는 후보를 시험했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_sendsend_sample1024_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_SENDSEND --transports tcp,ws,wss,tls --msg-sizes 1024,4096 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_030806.txt`는 status=partial(35/40)이었다.
- 결과:
  - partial report의 성공 RESULT 기준으로도 `tcp 1024B` 24.1%, `ws 1024B` 25.0%, `ws 4096B` 33.2%, `wss 1024B` 25.6%, `tls 1024B` 23.8%, `tls 4096B` 30.7%라 새 통과 cell이 없었다.
  - `wss 4096B`는 52.5%였지만 이미 후보 23의 complete report로 통과한 cell이다.
- 판단:
  - SENDSEND의 병목은 latency sample 비용이 아니라 request-free SPOT routed echo의 submit/recv 진행률과 server drain 경계에 더 가깝다.
  - 새 complete 통과 근거가 없으므로 이 후보는 최종 코드에서 제거한다.

## 후보 28: MULTI_SPOT_REQREP active slot 32 제한

- 변경 내용:
  - Python request/reply callback 경로의 lock, queue, callback contention을 줄이기 위해 `MULTI_SPOT_REQREP` active slot을 100개에서 32개로 제한하는 후보를 시험했다.
- 검증:
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_MULTI_SPOT_REQREP_ACTIVE_SLOTS=32 PERF_REPORT_TAG=python_multi_spot_reqrep_slots32_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP --transports tcp,ws,wss,tls --msg-sizes 64,1024 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_031737.txt`는 status=complete(40/40)였다.
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 15.8~22.9%로, 기존 표의 같은 cell보다 낮거나 같은 수준이었다.
- 판단:
  - outstanding request 수를 줄이면 latency는 낮아지지만 throughput은 더 낮아진다.
  - 이 후보는 최종 코드에서 제거한다. REQREP는 slot 수가 아니라 Python request builder/callback completion 경계가 주 병목이다.

## 후보 29: MULTI_SPOT_REQREP waiting flag lock 제거

- 변경 내용:
  - `MULTI_SPOT_REQREP` client의 active loop에서 `waiting` flag를 확인하고 갱신할 때 쓰던 `threading.Lock`을 제거하는 후보를 시험했다.
  - callback은 Python 코드로 들어오므로 GIL이 있고, boolean flag 보호 비용이 request/reply hot path에 남아 있을 가능성을 확인했다.
- 검증:
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_reqrep_no_waiting_lock_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP --transports tcp,ws,wss,tls --msg-sizes 64,1024,4096 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_033111.txt`는 status=complete(60/60)이었다.
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `tcp 64/1024/4096B`는 16.1%/17.8%/21.0%, `ws 64/1024/4096B`는 19.9%/21.6%/32.8%, `wss 64/1024/4096B`는 17.6%/23.2%/45.3%, `tls 64/1024/4096B`는 16.3%/19.3%/27.7%였다.
  - 기존 표와 비교하면 새 통과 cell이 없고, `tcp`와 `ws`의 여러 cell은 오히려 낮아졌다.
- 판단:
  - lock 제거는 Python callback completion 경계의 주 병목을 줄이지 못했고, 일부 구간에서는 outstanding request 상태 관리가 더 불안정해져 throughput이 낮아졌다.
  - 이 후보는 최종 코드에서 제거한다.

## 후보 30: MULTI_SPOT_REQREP native active request loop

- 변경 내용:
  - `MULTI_SPOT_REQREP` client의 START 이후 active window만 CPython extension으로 옮기는 후보를 시험했다.
  - READY/START/control handshake는 기존 Python 경로를 유지하고, SPOT request submit과 reply callback 집계만 native 함수에서 처리하도록 제한했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `PYTHONPATH=src PERF_REPORT_TAG=python_multi_spot_reqrep_native_active_tcp_smoke_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_REQREP --transports tcp --msg-sizes 64,1024 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_033721.txt`는 status=partial(4/10)이었다.
- 결과:
  - `tcp 64B`는 43.575 Kops/s, `tcp 1024B`는 42.366 Kops/s였다.
  - latency RESULT가 생성되지 않아 complete report가 아니며, throughput도 기존 표의 같은 cell보다 낮다.
- 판단:
  - request/reply completion을 active loop만 native로 옮겨도 C perf의 request state 진행률을 재현하지 못한다.
  - complete report가 아니고 성능도 낮으므로 이 후보는 최종 코드에서 제거한다.

## 후보 31: MULTI_SPOT_SENDSEND active slot 16 제한

- 변경 내용:
  - `MULTI_SPOT_SENDSEND` native client round-trip loop에서 active slot을 16개로 제한하는 후보를 시험했다.
  - 작은 payload에서 100개 SPOT을 모두 진행시키는 비용이 server drain과 poll pressure를 키우는지 확인하기 위한 측정이다.
- 검증:
  - `PYTHONPATH=src PERF_MULTI_SPOT_SENDSEND_ACTIVE_SLOTS=16 PERF_REPORT_TAG=python_multi_spot_sendsend_slots16_probe_20260603 bash perf/run_benchmarks_multi.sh --pattern MULTI_SPOT_SENDSEND --transports tcp,ws,wss,tls --msg-sizes 1024,4096 --duration 5`
  - 결과 파일 `perf_python_multi_linux_20260603_034552.txt`는 status=partial(30/40)이었다.
- 결과:
  - `tcp 4096B`와 `tls 4096B`는 READY/control line 실패로 RESULT가 없었다.
  - 성공 RESULT도 `tcp 1024B` 51.107 Kops/s, `ws 1024B` 47.862 Kops/s, `ws 4096B` 47.432 Kops/s, `wss 1024B` 45.075 Kops/s, `wss 4096B` 43.203 Kops/s, `tls 1024B` 45.171 Kops/s로 기존 표의 complete 대표값보다 낮았다.
- 판단:
  - active slot을 줄이면 latency는 낮아지지만 throughput과 runner 안정성이 함께 낮아진다.
  - complete report가 아니고 새 통과 cell도 없으므로 이 후보는 최종 코드에 반영하지 않는다.

## 후보 32: MULTI_SPOT_SENDSEND native client poller 정렬

- 변경 내용:
  - `MULTI_SPOT_SENDSEND` native client round-trip loop를 매 반복 `zlink_poll(...)` item 배열을 구성하는 방식에서 `zlink_poller_add(...)`로 SPOT slot을 한 번 등록하고 `zlink_poller_wait(...)` ready event만 처리하는 방식으로 바꿨다.
  - native branch가 `lat_mean_ms`/`lat_p95_ms`/`lat_p99_ms` key를 출력하던 버그를 고쳐 runner가 기대하는 `latency`/`latency_p95`/`latency_p99` RESULT를 생성하도록 했다.
  - timestamp 역전 outlier는 unsigned subtraction으로 큰 latency가 되지 않도록 0으로 보정했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - `perf_python_multi_linux_20260603_035741.txt`는 latency RESULT 누락으로 status=partial(16/40)이어서 표에는 반영하지 않았다.
  - `perf_python_multi_linux_20260603_040501.txt`는 `wss 1024B` 실패와 latency outlier로 status=partial(35/40)이어서 표에는 반영하지 않았다.
  - complete report:
    - `perf_python_multi_linux_20260603_040713.txt`: `tcp 1024/4096B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_041526.txt`: `tcp 64/256B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_041121.txt`: `ws 1024B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_041219.txt`: `ws 4096B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_041318.txt`: `wss 1024B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_041732.txt`: `wss 64/256B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_041022.txt`: `tls 1024/4096B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_041940.txt`: `tls 64/256B`, status=complete(10/10)
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 새 통과 cell은 `tcp 4096B` 35.1%, `ws 4096B` 46.1%, `wss 1024B` 36.5%, `tls 256B` 35.1%, `tls 4096B` 42.7%다.
  - 개선됐지만 기준에 못 닿은 cell은 `tcp 64/256/1024B` 31.6%/30.3%/33.4%, `ws 1024B` 34.9%, `wss 64/256B` 29.9%/31.6%, `tls 64/1024B` 30.3%/33.6%다.
- 판단:
  - SENDSEND client의 per-loop poll item 구성과 full scan은 small/medium 구간의 실제 병목이었다.
  - complete report 기준 새 통과 cell 5개가 생겼고, 미달 cell도 회귀 없이 개선되어 이 후보는 최종 코드에 남긴다.

## 후보 33: MULTI_SPOT_SENDSEND server native routed echo 보강

- 변경 내용:
  - `MULTI_SPOT_SENDSEND` server의 active phase 직전에 CPython extension native dispatch handler를 설치해 routed echo를 C API 호출로 직접 처리하도록 했다.
  - READY/START 제어 단계에서는 handler를 설치하지 않는다. 초기 설치 후보는 `CONTROL_CONNECTED` 제어선 누락 partial을 만들었기 때문에 active 시작 뒤로 설치 시점을 늦췄다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - 초기 설치 probe `perf_python_multi_linux_20260603_042833.txt`는 status=partial(0/5)이어서 표에 반영하지 않았다.
  - active 이후 설치 probe `perf_python_multi_linux_20260603_042955.txt`는 `MULTI_SPOT_SENDSEND ws 1024B`, status=complete(5/5)였다.
  - 묶음 probe `perf_python_multi_linux_20260603_044048.txt`는 `tls 1024B` timeout으로 status=partial(55/60)이어서 표에 반영하지 않았다.
- 결과:
  - C multi baseline `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt` 대비 `MULTI_SPOT_SENDSEND ws 1024B`가 35.1%로 통과했다.
  - partial report의 다른 small cell은 대부분 30%대 초반에 머물러 새 complete 통과 근거로 쓰지 않았다.
- 판단:
  - SENDSEND server의 Python `Received`/builder 경계도 병목이지만, small cell 전체를 통과시킬 정도는 아니었다.
  - complete report 기준 새 통과 cell이 생겼고, 제어 단계와 active handler 설치를 분리해 runner 안정성 문제를 피했으므로 이 후보는 최종 코드에 남긴다.

## 후보 34: MULTI_SPOT_REQREP native request/reply active loop

- 변경 내용:
  - `MULTI_SPOT_REQREP` client의 START 이후 active phase를 CPython extension native loop로 옮겼다.
  - SPOT request submit, reply callback 집계, latency sample 저장을 native 상태 구조에서 처리하고, Python control handshake와 결과 출력 포맷은 유지했다.
  - server도 active phase 직전에 native routed reply dispatch handler를 설치해 Python `Received` 생성과 reply builder 경계를 제거했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - 단일 probe `perf_python_multi_linux_20260603_044652.txt`는 `MULTI_SPOT_REQREP ws 1024B`, status=complete(5/5)였다.
  - small 묶음 probe `perf_python_multi_linux_20260603_045715.txt`는 `tcp 1024B` 제어선 누락으로 status=partial(55/60)이어서 표에 반영하지 않았다.
  - non-tcp 묶음 probe `perf_python_multi_linux_20260603_050458.txt`는 `tls 256B` 제어선 누락으로 status=partial(40/45)이어서 표에 반영하지 않았다.
  - complete report:
    - `perf_python_multi_linux_20260603_050745.txt`: `ws 64/256/1024B`, status=complete(15/15)
    - `perf_python_multi_linux_20260603_051029.txt`: `wss 64/256/1024B`, status=complete(15/15)
    - `perf_python_multi_linux_20260603_051128.txt`: `tls 256B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_051319.txt`: `tls 64/1024B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_051511.txt`: `tcp 64/256B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_051611.txt`: `tcp 1024B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_051805.txt`: `tcp 4096/65536B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_051956.txt`: `ws 65536/131072B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_052054.txt`: `tls 4096B`, status=complete(5/5)
    - `perf_python_multi_linux_20260603_052534.txt`: pending cleanup 보강 뒤 `ws 1024B` 재확인, status=complete(5/5)
- 결과:
  - 잔여 `MULTI_SPOT_REQREP` 17개 cell이 C 대비 82.7~93.5%로 모두 통과했다.
  - Python 전체 상태는 Single 144/144, Multi 175/184, 전체 319/328 통과로 갱신했다.
- 판단:
  - REQREP의 병목은 Python request builder, callback completion, lock/queue 경계였다.
  - native active loop는 public Python API를 바꾸지 않고 active phase 비용만 아래로 내렸고, complete report 기준 잔여 REQREP cell을 모두 통과시켰으므로 최종 코드에 남긴다.

## 후보 35: MULTI_SPOT_SENDSEND success close / sample stride 후보 기각

- 변경 내용:
  - SENDSEND client/server의 성공 submit 뒤 `zlink_msg_close(...)` 생략 후보를 각각 시험했다.
  - SENDSEND 수신 latency sample stride 후보도 시험했다.
- 검증:
  - 각 후보 적용 뒤 `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - client/server success close 동시 후보 `perf_python_multi_linux_20260603_053738.txt`는 tcp 1024B 제어선 누락으로 status=partial(15/20)이었다.
  - server success close 단독 후보 `perf_python_multi_linux_20260603_060035.txt`는 wss 1024B timeout으로 status=partial(15/20)이었다.
  - client success close 단독 후보 `perf_python_multi_linux_20260603_060936.txt`는 status=complete(20/20)이었지만 tcp/tls 1024B를 통과권으로 올리지 못했다.
  - sample stride 후보 `perf_python_multi_linux_20260603_062457.txt`는 ws 64B 제어선 누락으로 status=partial(35/40)이었고, small failset도 통과권까지 오르지 못했다.
- 판단:
  - success close 생략은 일부 transport 수치를 올릴 수 있지만 runner 안정성이나 잔여 failset 통과를 만들지 못했다.
  - sample stride는 throughput 병목을 충분히 줄이지 못했고 partial 이력이 남았다.
  - 두 후보 모두 최종 코드에 남기지 않는다.

## 후보 36: MULTI_SPOT_SENDSEND client init_data 소형 fast path

- 변경 내용:
  - SENDSEND client native loop에서 64/256B payload는 `zlink_msg_init_size(...)` 뒤 `memcpy(...)`하지 않고 `zlink_msg_init_data(..., NULL, NULL)`로 외부 payload buffer를 직접 참조하게 했다.
  - slot별 payload buffer는 응답을 받기 전까지 재사용하지 않으므로 active loop 안에서 수명이 유지된다.
  - 1024B 이상은 기존 copy 방식이 더 안정적인 수치를 보였기 때문에 기존 경로를 유지했다.
- 검증:
  - `python3 setup.py build_ext --inplace --force`
  - `python3 -m compileall -q src tests perf`
  - `PYTHONPATH=src pytest -q tests` -> `72 passed`
  - Python complete report:
    - `perf_python_multi_linux_20260603_055040.txt`: `tcp 64/256B`, status=complete(10/10)
    - `perf_python_multi_linux_20260603_055608.txt`: `ws/wss/tls 64/256B`, status=complete(30/30)
    - `perf_python_multi_linux_20260603_061712.txt`: boundary `64/1024B`, status=complete(40/40)
  - C small failset 제한 재측정 `perf_c_multi_linux_20260603_062523.txt`는 `MULTI_SPOT_SENDSEND tcp/ws/wss/tls 64/256B`, status=complete(40/40)이었다.
- 결과:
  - C small failset 제한 재측정 기준으로 `MULTI_SPOT_SENDSEND` 64/256B 잔여 cell이 모두 node_python SPOT 계열 최소 기준 33%를 넘었다.
  - `tcp 1024B`, `ws 1024B`, `wss 1024B`, `tls 1024B`도 current boundary complete report와 C full 기준으로 33.4%/34.6%/35.7%/33.4%라 모두 통과다.
  - Python 전체 상태는 Single 144/144, Multi 184/184, 전체 328/328 통과로 갱신했다.
- 판단:
  - SENDSEND small 구간은 Python 쪽 사본 생성 비용과 C baseline 변동성이 함께 있었다.
  - client small fast path는 public Python API를 바꾸지 않고 native bridge 내부에서만 사본 생성을 줄인다.
  - C small failset 보강도 status=complete이므로 표 반영 근거로 사용한다.

## current public Python multi tcp64 smoke 재확인

- 배경:
  - public contract 복구 뒤 main 계획 문서의 Python multi는 current full 판정으로 쓰지 않고 있었다.
  - full matrix 전에 현재 runner와 public Python multi 경로의 최소 tcp/64 smoke를 다시 확인했다.
- 준비:
  - `python3 setup.py build_ext --inplace --force`로 native bridge를 현재 소스 기준으로 다시 빌드했다.
- C 기준:
  - `perf_c_multi_linux_20260604_213725_python_multi_tcp64_c_recheck_20260604.txt`
  - status=complete, expected/actual result lines 40/40
- Python 재측정:
  - 재빌드 전 `perf_python_multi_linux_20260604_214417_python_multi_tcp64_recheck_20260604.txt`
  - 재빌드 후 `perf_python_multi_linux_20260604_215121_python_multi_tcp64_after_rebuild_20260604.txt`
  - 재빌드 후 파일도 status=partial, expected/actual result lines 120/100
- 재빌드 후 실패:
  - `MULTI_SPOT tcp 64B`: `CLIENT_CONTROL_ENDPOINT` 제어선 누락
  - `MULTI_STREAM tcp 64B`: server `READY,...` 뒤 RESULT 누락 3회
- 재빌드 후 current C 대비 비율:
  - `MULTI_DEALER_DEALER tcp 64B`: 35.0%
  - `MULTI_DEALER_ROUTER tcp 64B`: 45.5%
  - `MULTI_ROUTER_ROUTER tcp 64B`: 31.2%
  - `MULTI_PUBSUB tcp 64B`: 22.6%
  - `MULTI_SPOT tcp 64B`: 11.7%
  - `MULTI_SPOT_REQREP tcp 64B`: 19.2%
  - `MULTI_SPOT_SENDSEND tcp 64B`: 37.7%
- 판정:
  - current public Python multi는 아직 full matrix로 갈 수 있는 상태가 아니다.
  - 먼저 `MULTI_STREAM` RESULT 누락과 `MULTI_SPOT` 제어선 누락을 고친 뒤, `MULTI_PUBSUB`,
    `MULTI_SPOT`, `MULTI_SPOT_REQREP`의 current C 대비 미달을 다시 줄인다.

## current Python multi stream native echo 후보 기각

- 대상:
  - `MULTI_STREAM tcp 64B`
- 후보:
  - Python stream perf server의 public `on_packet(...)` callback/queue loop 대신 기존 CPython
    extension의 `stream_echo_install(...)`/`stream_echo_drain(...)` native echo session을 직접 호출했다.
- 결과:
  - `perf_python_multi_linux_20260604_215505_python_multi_stream_native_tcp64_20260604.txt`
  - status=complete, expected/actual result lines 15/15
  - median 309,132 ops/s로 current C `perf_c_multi_linux_20260604_213725_python_multi_tcp64_c_recheck_20260604.txt`
    대비 94.8%였다.
- 기각:
  - `PYTHONPATH=src pytest -q tests`에서 `test_optimization_guard.py`가 실패했다.
  - 이 저장소의 current guard는 perf script가 `stream_echo_install(...)`/`stream_echo_drain(...)`
    같은 private native active-loop helper를 직접 호출하는 것을 금지한다.
  - 후보 코드는 되돌렸고, 되돌린 뒤 `PYTHONPATH=src pytest -q tests/test_optimization_guard.py`는
    통과했다.
- 추가 확인:
  - public Python stream server는 `--clients 100`에서는
    `perf_python_multi_linux_20260604_215350_python_multi_stream_tcp64_clients100_debug_20260604.txt`
    기준 status=complete지만 1,044 ops/s에 그쳤다.
  - default stream `--clients 10000`에서는 RESULT 없이 실패하므로, public API-safe stream 개선은
    별도 설계가 필요하다.

## current Python multi SPOT_SENDSEND tcp64 단독 보강

- 대상:
  - `MULTI_SPOT_SENDSEND tcp 64B`
- 배경:
  - stream native 후보를 되돌린 뒤 전체 tcp/64 smoke
    `perf_python_multi_linux_20260604_215953_python_multi_tcp64_after_stream_native_20260604.txt`는
    `MULTI_SPOT_SENDSEND` 첫 run readiness 누락으로 status=partial이었다.
- 단독 재측정:
  - `perf_python_multi_linux_20260604_220423_python_multi_spot_sendsend_tcp64_current_recheck_20260604.txt`
  - status=complete, expected/actual result lines 15/15
- 결과:
  - median 84,604 ops/s.
  - current C `perf_c_multi_linux_20260604_213725_python_multi_tcp64_c_recheck_20260604.txt`의
    `MULTI_SPOT_SENDSEND tcp 64B` 237,319 ops/s 대비 35.7%다.
- 판정:
  - 성능 비율은 node_python SPOT 계열 기준 33%를 넘지만, 전체 smoke에서는 readiness 누락이
    재현됐으므로 runner 안정성 보강이 아직 필요하다.

## current Python multi tcp64 추가 재확인

- 준비:
  - `python3 setup.py build_ext --inplace --force`: 통과.
  - `python3 -m compileall -q src tests perf`: 통과.
  - `PYTHONPATH=src pytest -q tests`: 전체 테스트가 장시간 출력 없이 멈춰 중단했다. 이번 확인의
    최종 판정 근거로 쓰지 않는다.
- 전체 tcp/64 smoke:
  - `perf_python_multi_linux_20260604_232936_python_multi_tcp64_current_smoke_20260604.txt`
  - status=partial, expected/actual result lines 40/30.
  - `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`, `MULTI_PUBSUB`,
    `MULTI_SPOT`, `MULTI_SPOT_REQREP`는 RESULT를 냈다.
  - `MULTI_SPOT_SENDSEND tcp 64B`: `CLIENT_CONTROL_ENDPOINT` 제어선 누락.
  - `MULTI_STREAM tcp 64B`: server `READY,...` 뒤 RESULT 누락.
- `MULTI_SPOT_SENDSEND tcp 64B` 단독 보강:
  - `perf_python_multi_linux_20260604_233407_python_multi_sendsend_tcp64_timeout90_clean_20260604.txt`
  - status=complete, expected/actual result lines 5/5.
  - throughput 66,715 ops/s, latency mean 0.415ms였다.
  - 같은 단독 실행에서도 총 101초가 걸려, 전체 smoke 안에서는 readiness 지연을 다시 일으킬 수 있다.
- `MULTI_STREAM tcp 64B` public 경로 재확인:
  - `perf_python_multi_linux_20260604_233208_python_multi_stream_tcp64_clients100_20260604.txt`
  - `--clients 100`에서는 status=complete, expected/actual result lines 5/5.
  - throughput 1,143 ops/s라 current C 기준 통과 근거로 쓰기에는 부족하다.
  - `perf_python_multi_linux_20260604_233431_python_multi_stream_tcp64_default_clients_timeout180_20260604.txt`
    는 기본 stream client 수 10000에서 status=partial, RESULT 0개였다.
  - 직접 실행으로 확인하면 같은 기본 10000 client에서 Python stream server가 SIGSEGV로 종료했다.
- public immediate-send 후보:
  - Python stream packet callback에서 queue에 넣기 전에 public `send(...)`를 먼저 시도하도록 바꿨다.
  - `perf_python_multi_linux_20260604_233614_python_multi_stream_tcp64_immediate_send_clients100_20260604.txt`
    는 `--clients 100`에서도 status=partial, RESULT 0개로 회귀했다.
  - 후보 코드는 최종 코드에 남기지 않았다.
- 판정:
  - `MULTI_SPOT_SENDSEND tcp 64B`는 isolated complete 근거가 있지만, 전체 smoke 안정성은 아직 부족하다.
  - `MULTI_STREAM tcp 64B`는 private native stream echo helper 없이 public API 경로로 기본 10000 client를
    처리하지 못한다. 다음 후보는 public callback 생명주기와 stream server shutdown crash를 먼저 줄여야 한다.

## current Python multi stream completion wait 보강

- 대상:
  - `MULTI_STREAM tcp 64B`, 기본 stream client 수 10000.
- 배경:
  - shared C `perf_stream_client`는 active window 뒤 기본 500ms만 in-flight reply를 기다린다.
  - public Python stream server는 callback/queue 경로가 느려 기본 10000 client에서 active 중 보낸
    reply tail이 500ms 안에 다 돌아오지 않았다.
  - 직접 debug 실행에서 `connect_ok=10000`, `connect_fail=0`, `timeout_error=7877`,
    `samples=6141`로 확인했다.
- 후보:
  - Python multi runner가 `MULTI_STREAM` external client를 띄울 때 `--completion-wait-ms`를
    `PERF_MULTI_STREAM_COMPLETION_WAIT_MS`, `PERF_STREAM_COMPLETION_WAIT_MS`, 기본 `10000`
    순서로 넘기도록 했다.
  - active duration은 그대로 유지하고, shared client가 active 중 보낸 요청의 reply tail을 기다리는 시간만
    늘린다.
- 검증:
  - `python3 -m compileall -q bindings/python/perf/multi/run_benchmarks.py`: 통과.
  - `PYTHONPATH=bindings/python/src pytest -q bindings/python/tests/test_optimization_guard.py`: 통과.
  - `perf_python_multi_linux_20260604_234619_python_multi_stream_tcp64_completion_wait_default_clients_20260604.txt`
    는 status=complete, expected/actual result lines 5/5.
  - 기본 10000 client에서 throughput 4,146.5 ops/s, latency mean 728.493ms였다.
- 추가 확인:
  - 전체 tcp/64 smoke `perf_python_multi_linux_20260604_235011_python_multi_tcp64_after_stream_completion_wait_20260604.txt`
    에서 `MULTI_STREAM tcp 64B`는 RESULT를 냈다.
  - 같은 smoke는 `MULTI_SPOT_REQREP tcp 64B`의 intermittent `ConfigError(code=702, internal_errno=22)`로
    status=partial이었다.
  - `MULTI_SPOT_REQREP tcp 64B` 단독 실행
    `perf_python_multi_linux_20260604_235025_python_multi_spot_reqrep_tcp64_config_error_repro_20260604.txt`
    는 status=complete였다.
- 기각 후보:
  - stream packet callback에서 public `send(...)`를 즉시 시도하는 후보는 `--clients 100`에서도
    RESULT 0개로 회귀해 제거했다.
  - callback enqueue 뒤 `threading.Event`로 drain loop를 깨우는 후보는 `--clients 100` 처리량은
    3,907 ops/s로 올렸지만, 기본 10000 client debug에서 `timeout_error=8419`로 악화되어 제거했다.
  - stream callback message를 bytes 왕복 대신 native copy로 감싸는 후보는 `--clients 100` 처리량이
    1,113.5 ops/s에 머물러 개선이 없었고, 기본 10000 client도 partial이라 제거했다.
- 판정:
  - `MULTI_STREAM tcp 64B` RESULT 누락은 public API 경로를 유지한 runner completion-wait 보강으로
    해소했다.
  - Python multi tcp/64 전체 smoke의 남은 안정성 문제는 SPOT control/cleanup 쪽 intermittent다.
