# Round 46: distributor prechecked HWM write 후보

- goal: `MULTI_PUBSUB`와 SPOT data-plane fanout의 64B one-way pipe 비용을 줄인다.
- 완료 기준:
  - targeted `PUBSUB,SPOT` 64B set에서 문제 report 대비 평균/중앙값이 의미 있게 개선된다.
  - 후보 전후 비교에서 10% 이상 반복 개선이 없으면 원복한다.
  - 관련 core tests 통과.
- 기준 commit: `72d893595`
- 시작 git status:
  - `core/src`, `core/include`, `core/tests`, `bindings/c/perf` source diff 없음
  - 기존 perf 로그 파일 untracked 다수 존재
- 기준 report:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최신 current sweep:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`

## 가설

- 가설 1: non-lossy PUB/XPUB fanout은 `dist_t::check_hwm()`에서 matching pipe를 먼저
  검사한 뒤 `dist_t::write_at()`에서 같은 pipe의 HWM을 다시 검사한다. 같은 public send
  call 안에서는 writer가 하나이므로 두 번째 HWM 검사는 중복 비용일 수 있다.
- 가설 2: 병목은 pipe HWM 산술보다 ypipe flush/wakeup 또는 transport write batching에 있어
  두 번째 HWM 검사를 줄여도 64B 처리량은 거의 오르지 않을 수 있다.
- 선택한 가설: 가설 1을 먼저 검증한다. round11의 final-frame helper와 달리 이번 후보는
  HWM 선검사 결과를 같은 distributor send에 전달하는 변경이다.

## 읽은 코드

- `core/src/runtime/sockets/pubsub/xpub.cpp`:
  - non-lossy path는 `_dist.check_hwm()` 성공 뒤 `send_to_all()` 또는 `send_to_matching()`을 호출한다.
  - lossy path는 `check_hwm()`을 건너뛸 수 있으므로 prechecked write를 쓰면 안 된다.
- `core/src/runtime/sockets/internal/dist.cpp`:
  - `check_hwm()`은 matching pipe 전체를 훑는다.
  - `write_at()`은 다시 `pipe_t::write_*_no_recursive_hwm_check()`를 호출해 HWM을 재검사한다.
- `core/src/runtime/core/pipe.cpp`:
  - `write_no_hwm_check()`는 active/termination 상태는 확인하고 HWM만 생략한다.
  - final+flush용 no-HWM helper는 아직 없다.

## 변경 계획

- `pipe_t`에 final+flush용 `write_and_flush_no_hwm_check()`를 추가한다.
- `dist_t`에 HWM prechecked 전용 send 경로를 추가한다.
- `xpub_t`는 `_lossy`가 false이고 `check_hwm()`이 성공한 경우에만 prechecked 경로를 호출한다.
- lossy path와 HWM 미확인 path는 기존 write 경로를 유지한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드리는 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
  - pipe active/termination 상태 검사는 유지하고, HWM 검사를 생략하는 경로는 같은
    distributor send call에서 `check_hwm()`이 이미 성공한 non-lossy path로 제한한다.
- 추가로 실행한 회귀 테스트: 후보 적용 후 기록한다.

## 후보 적용

- 변경 파일:
  - `core/src/runtime/core/pipe.hpp`
  - `core/src/runtime/core/pipe.cpp`
  - `core/src/runtime/sockets/internal/dist.hpp`
  - `core/src/runtime/sockets/internal/dist.cpp`
  - `core/src/runtime/sockets/pubsub/xpub.cpp`
- 변경 내용:
  - `pipe_t::write_and_flush_no_hwm_check()`를 임시 추가했다.
  - `dist_t`에 HWM prechecked 전용 write 경로를 임시 추가했다.
  - `xpub_t`의 non-lossy path에서 `check_hwm()` 성공 뒤 prechecked 경로를 호출했다.

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - 명령:
    `ctest --test-dir core/build -R 'test_pubsub$|test_pubsub_filter_xpub|test_xpub_nodrop|test_spot_pubsub_scenario|test_backpressure_oneway_matrix|test_backpressure_matrix|test_multi_socket_contract_regressions|test_transport_matrix|unittest_ypipe' --output-on-failure`
  - 결과:
    - 23개 중 21개 통과, 2개 실패.
    - 실패:
      - `test_spot_pubsub_scenario`
      - `test_spot_pubsub_scenario_node_child_interop`
    - 실패 세부:
      `test_spot_node_direct_local_and_child_interop:FAIL: Expected TRUE Was FALSE`
- 추가 분리:
  - `send_all_data` 경로만 기존 `send_to_all()`로 되돌리고 다시 빌드했다.
  - 명령:
    `ctest --test-dir core/build -R 'test_spot_pubsub_scenario|test_spot_pubsub_scenario_node_child_interop' --output-on-failure`
  - 결과:
    - `test_spot_pubsub_scenario_node_child_interop`가 계속 실패했다.

## 판정

- 후보는 성능 측정 전에 SPOT interop 계약을 깨므로 폐기한다.
- `check_hwm()` 직후 같은 send call에서 두 번째 HWM 검사를 생략하는 방식은 SPOT fanout의
  child/local interop 의미와 맞지 않는다.
- source 변경은 전부 원복했다.

## 원복 확인

- `git diff -- core/src/runtime/core/pipe.hpp core/src/runtime/core/pipe.cpp core/src/runtime/sockets/internal/dist.hpp core/src/runtime/sockets/internal/dist.cpp core/src/runtime/sockets/pubsub/xpub.cpp --stat`:
  출력 없음.
- 원복 후 build:
  - `cmake --build core/build -j$(nproc)` 통과.
- 원복 후 test:
  - `ctest --test-dir core/build -R 'test_spot_pubsub_scenario|test_spot_pubsub_scenario_node_child_interop' --output-on-failure`
  - 9/9 통과.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거:
  - 후보는 원복되어 최종 source 변경이 없다.
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않았다.
- 추가로 실행한 회귀 테스트:
  - 위 SPOT scenario 재실행 9/9 통과.
