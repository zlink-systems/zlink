# MAC-1 — macOS ARM64 Core 테스트 게이트 보고서

- 브랜치: `wip/mac-1` (base `wip/0.17.3-all2`), push 완료. 부수 브랜치 `wip/mac-1-hang`(hang sampler).
- 진단 워크플로: `.github/workflows/core-macos-test.yml`(macos-15, RelWithDebInfo, `-L serial -j1` → `-LE serial -j3`, 두 그룹 모두 항상 실행), `.github/workflows/core-macos-hang.yml`(멈춘 프로세스를 `sample`로 스택 덤프).
  - `workflow_dispatch`는 **default branch에만 노출**되어 `gh workflow run --ref wip/mac-1`이 404가 난다. 그래서 두 워크플로 모두 해당 브랜치 push 트리거로 구동하고, 실행 범위는 파일 상단 `env: CTEST_REGEX/CTEST_EXCLUDE/CTEST_REPEAT`로 조절한다.
- 코드 패치: `/home/hep7hep7/project/zlink-work/all-artifacts/MAC-1.patch` (`wip/0.17.3-all2` 기준, cherry-pick한 main 커밋과 진단 워크플로 제외).

## 1. 전/후 실패 목록

| 그룹 | 이전 (run 34191179487, base+cherry-pick) | 이후 (run 34196588768, 최종 패치) |
|---|---|---|
| `-L serial -j1` (149) | **8 실패** | **5 실패** |
| `-LE serial -j3` (57) | 1 실패 (`unittest_request_timeout_scheduler` Timeout) | **0 실패 (100% pass)** |
| serial 총 실행시간 | 832.8 s | 716.0 s |

이후에 남은 5건: `test_ctx_options`(Failed), `test_xpub_nodrop`(Failed), `test_single_lane_wire_mandatory_count`(Failed), `test_single_lane_wire_old_peer_rejected`(Timeout — 실패 후 `ctx_term` 대기), `test_wake_invariants`(Failed). **해소된 4건**: `test_router_multiple_dealers`, `test_transport_matrix`(mkdtemp), `test_endpoint_release`(15.84 s로 통과), `unittest_request_timeout_scheduler`(TIMEOUT 스케일).

이전 8건: `test_ctx_options`(Failed), `test_xpub_nodrop`(Failed), `test_endpoint_release`(Timeout 10 s), `test_router_multiple_dealers`(Timeout 10 s), `test_transport_matrix`(Timeout 120 s), `test_single_lane_wire_mandatory_count`(Timeout 90 s), `test_single_lane_wire_old_peer_rejected`(Timeout 45 s), `test_wake_invariants`(Timeout 180 s).

중간 단계(run 34192110911, mkdtemp 수정만): serial **8 → 6**, 총 시간 **832.8 s → 534.0 s**. `test_router_multiple_dealers`·`test_transport_matrix` 해소, `test_wake_invariants`는 Timeout 180 s → Failed 1.09 s.

## 2. 근본 원인과 수정

| # | 증상(파일:라인) | macOS 고유 원인 | 수정 |
|---|---|---|---|
| 1 | `test_router_multiple_dealers.cpp:113` `zlink_bind(router,"ipc://*")` → `errno 48 (EADDRINUSE)`; `test_wake_invariants.cpp:293` 동일; 이어서 Unity abort로 소켓이 남아 `zlink_ctx_term`→`ctx_t::wait_for_reaper_done`에서 영구 대기(`sample` 스택으로 확인) | `core/CMakeLists.txt:528` `check_cxx_symbol_exists(mkdtemp stdlib.h HAVE_MKDTEMP)`. Darwin은 `mkdtemp`를 `<unistd.h>`에 선언 → CI 로그 `Looking for mkdtemp - not found`. `core/src/runtime/utils/ip.cpp:443` **mkstemp 폴백**이 일반 파일을 만들고, `core/src/runtime/transports/ipc/asio_ipc_listener.cpp:86` `unlink_existing_ipc_socket()`이 “소켓이 아님”으로 `EADDRINUSE`를 돌려준다 | 헤더 목록을 `"stdlib.h;unistd.h"`로. Linux는 이미 stdlib.h에서 찾으므로 완전 동일 |
| 2 | `test_endpoint_release` Timeout 10 s / `unittest_request_timeout_scheduler`·`unittest_flow_state_monitor` Timeout 10 s — 단독 실행 시 **모두 PASS**(각 15 s / 13.8 s, 프로세스는 rc=0으로 정상 종료) | macOS 러너가 같은 커밋에서 Linux보다 약 3배 느리다(Linux CI: 3.33 s / 5.93 s). `set_tests_properties(... TIMEOUT 10)`에 여유가 없어 건강한 테스트가 죽는다. ctx 생성/종료 자체는 macOS에서도 0 ms로 측정되어 Core 종료 경로 문제 아님 | 새 파일 `core/tests/scale_test_timeouts.cmake` — `if(APPLE)`일 때만 그 디렉터리에 등록된 모든 테스트의 `TIMEOUT`을 3배로. `core/tests/CMakeLists.txt`와 `core/tests/unittest/CMakeLists.txt`(unittest는 별도 CMake 디렉터리 스코프라 부모 루프에 안 잡힌다) 양쪽 끝에서 include. 테스트 본문·내부 대기·기대값 불변, 다른 플랫폼 값은 바이트 동일 |

## 3. 남은 실패와 확보한 근거 (미해결)

| 테스트 | 관측 | 확보한 근거 | 판단 |
|---|---|---|---|
| `test_ctx_options.cpp:657` `test_auto_hwm_applied_limit_blocks_and_resumes_after_drain` | `Expected 4096 Was -1` | 임시 진단으로 **블로킹 send의 errno = 35(EAGAIN)** 확인. `options.cpp:115` 기본 `sndtimeo = 1000` ms이므로 “블로킹” send는 1 s 한도. 즉 drain 뒤 sender를 1 s 안에 깨우지 못했다. 좁은 regex 단독 실행(run 34193638775)에서는 PASS → full serial run에서만 재현되는 **간헐** 실패 | Core의 backpressure→writable wake가 macOS에서 늦다. 수정 미완 |
| `test_xpub_nodrop.cpp:243` | `blocking publish timeout sent=N recv=N` (N=7193/6161/3425로 매번 다름) | 발행자와 수신자가 **같은 지점에서 동시에 멈춤**(보낸 수 = 받은 수). 단독 실행 시 PASS(1.38 s / 2.05 s) | 위와 같은 wake 계열로 보임. 수정 미완 |
| `test_dealer_router_single_lane_contract.cpp:1348 / :1515` (`test_single_lane_wire_mandatory_count`, `..._old_peer_rejected`) | `wait_for_raw_close` 가 3 s 안에 EOF/RST를 못 봄 | 진단 출력: `last_rc=1 errno=60 (ETIMEDOUT) bytes_drained=67`(각각 67 / 61바이트). 즉 라우터의 handshake 응답 바이트는 도착했지만 3 s 동안 연결이 닫히지 않는다. Linux에서는 같은 테스트가 10.25 s / 1.41 s로 통과 | 잘못된 READY에 대한 라우터의 프로토콜 오류 종료가 macOS에서 3 s 안에 관측되지 않음. 원인 미확정 |
| `test_wake_invariants.cpp:569` `test_multi_dealer_dealer_tcp_large_hwm_drain_wakes_all_pollout` | `socket published an unexpected extra completion` | 진단 출력: 남은 completion `kind=3 id=1 send_result=0 terminal_errno=0 peer_rid_size=0 reply_parts=0` | 여분 completion 1건이 큐에 남는다. Core completion 발행 경로의 macOS 타이밍 의존. 수정 미완 |

부수 관찰(수정하지 않음): macOS에서 `Looking for clock_gettime - not found`가 뜬다. `set(CMAKE_REQUIRED_LIBRARIES rt)`로 프로브하는데 Darwin에는 librt가 없어 링크가 실패하기 때문이다. 다만 `core/src/runtime/utils/clock.hpp:16`이 `ZLINK_HAVE_OSX`일 때 `HAVE_CLOCK_GETTIME`을 강제 정의하므로 실제 시계 경로는 `clock_gettime(CLOCK_MONOTONIC)`로 동일하다 — 무해하여 건드리지 않았다.

## 4. macOS run id

| run | 내용 |
|---|---|
| 34189038691 | 0.17.3 릴리스 run (게이팅 전, `-j$(ncpu)`) — 11 실패 |
| 34190928956 | main 진단 run (새 게이팅) — serial 8 실패 |
| 34191179487 | `wip/mac-1` 기준선(cherry-pick만) — serial 8 / parallel 1 |
| 34191321097 | hang sampler — `ctx_term`→`wait_for_reaper_done` 대기 스택 확보, 4개 테스트는 단독 실행 시 PASS 확인 |
| 34192110911 | mkdtemp 수정 — serial 8 → 6, 832.8 s → 534.0 s |
| 34193225840 / 34193638775 / 34194246491 | 임시 진단(errno·hex·ctx 타이밍). 진단 커밋은 최종 브랜치에서 되돌림 |
| 34195054163 | mkdtemp + APPLE TIMEOUT 3배 1차 — serial 5 실패, 그러나 unittest 하위 디렉터리는 스케일 미적용 |
| 34196588768 | **최종** — serial 5 실패 / parallel 0 실패, serial 716.0 s |

## 5. Linux 검증

worktree `~/project/zlink-work/mac1`, `core/build-dev`(RelWithDebInfo, 테스트 ON), 최종 패치 적용 상태:

```
ctest -R '^(test_ctx_options|test_xpub_nodrop|test_endpoint_release|test_router_multiple_dealers|test_transport_matrix|test_single_lane_wire_mandatory_count|test_single_lane_wire_old_peer_rejected|test_wake_invariants|unittest_request_timeout_scheduler)$' -j1
→ 100% tests passed, 0 tests failed out of 9
```

또한 `cmake .` 재구성 후 Linux에서 `test_endpoint_release`·`unittest_request_timeout_scheduler`의 TIMEOUT이 여전히 10임을 `ctest --show-only=json-v1`로 확인했다(APPLE 분기가 타지 않음). 반대로 `-DAPPLE=1`을 강제한 별도 구성에서는 30 / 30 / 360(`test_transport_matrix`) / 540(`test_wake_invariants`)으로 스케일됨을 확인했다.

## 6. 변경 분류

- `core/CMakeLists.txt` mkdtemp 탐지: **B — 기존 결함**(빌드 구성이 Darwin에서 조용히 잘못된 폴백을 골랐다).
- `core/tests/CMakeLists.txt` APPLE TIMEOUT 3배: **C — 우회**(러너 속도 차이를 ctest 마감시간으로 흡수. 테스트 본문·기대값·내부 대기는 불변).
- 공개 인터페이스(`core/include/**`, `libzlink.vers`), 계약 테스트 기대값, 옵션·플래그, 계약 동작 어느 것도 바꾸지 않았다.

## 7. 멈춘 지점

시간 상한 안에서 §3의 4개 잔여 실패(총 4개 테스트)는 원인을 관측 수준까지만 좁히고 수정하지 못했다. 다음 작업의 출발점은 (a) 블로킹 send가 drain 뒤 1 s 안에 깨어나지 못하는 경로(`socket_base_msg.cpp:535` 이후 wait 루프와 그 wake 원천), (b) 잘못된 READY 뒤 라우터가 raw peer 연결을 닫는 경로다.
