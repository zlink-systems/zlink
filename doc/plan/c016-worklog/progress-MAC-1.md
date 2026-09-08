# MAC-1 진행

- 시작. 공통 규칙 읽음. worktree ~/project/zlink-work/mac1 (wip/mac-1 <- origin/wip/0.17.3-all2) 생성 중.
- 진단 run 34190928956 (main) 진행 중 — 대기.

## 05:45 진단
- worktree/branch wip/mac-1 준비, main의 fff3533f0e·26ea44eba0 cherry-pick 완료(build.yml 충돌 해결).
- `.github/workflows/core-macos-test.yml` 추가. workflow_dispatch는 default branch에만 노출되므로 push 트리거(wip/mac-1)로 구동. run 34191179487 진행 중.
- 34189038691 macOS ARM64 로그 분석: 11 실패 중 8개가 Timeout이며 **모두 "직전 테스트까지 PASS 출력 후 다음 테스트에서 멈춤"** 패턴.
  - test_router_multiple_dealers → ipc 케이스에서 hang
  - test_endpoint_release → inproc_unbind_disconnected_and_not_found 에서 hang
  - unittest_request_timeout_scheduler → schedule_after_canceling_last_task_across_idle_exit 에서 hang
  - unittest_flow_state_socket → stale_flow_state_command_cannot_override_a_newer_epoch 에서 hang
- 가설: macOS(kqueue) poller에서 wake 유실 / idle-exit 경로. test_wake_invariants·test_transport_matrix timeout과 정합.

## 06:00 근본 원인 1 확정
- hang sampler run 34191321097 결과:
  - test_endpoint_release / unittest_request_timeout_scheduler / unittest_flow_state_socket / test_stream_packet_progress 는 **단독 실행 시 전부 PASS**. ctest의 10 s TIMEOUT을 macOS에서 넘긴 것(각각 15 s / 13.8 s / 9.7 s / 6.7 s).
  - test_router_multiple_dealers: `zlink_bind(router,"ipc://*")` → errno 48 EADDRINUSE → Unity abort → 소켓 미정리 → `zlink_ctx_term` → `wait_for_reaper_done` 무한 대기(샘플로 확인).
  - test_wake_invariants: 동일한 ipc bind EADDRINUSE + `test_multi_dealer_dealer_tcp_large_hwm_drain_wakes_all_pollout: socket published an unexpected extra completion`.
- **원인**: `core/CMakeLists.txt:528 check_cxx_symbol_exists(mkdtemp stdlib.h HAVE_MKDTEMP)`. Darwin은 mkdtemp를 `<unistd.h>`에 선언 → CI 로그 "Looking for mkdtemp - not found" → `ip.cpp:443` mkstemp 폴백이 **일반 파일**을 만들고, `asio_ipc_listener.cpp:86` 가 소켓이 아니라며 EADDRINUSE 반환.
- 수정: 헤더 목록을 `"stdlib.h;unistd.h"` 로. Linux 동작 불변.

## 06:35 mkdtemp 수정 후 결과 (run 34192110911)
- serial 8 → 6 실패. test_router_multiple_dealers·test_transport_matrix 해소. 총 시간 832 s → 534 s.
- 남은 serial 실패: test_ctx_options, test_xpub_nodrop, test_endpoint_release(Timeout 10 s), test_single_lane_wire_mandatory_count, test_single_lane_wire_old_peer_rejected, test_wake_invariants(이제 1.09 s에 Failed — hang 아님).
- parallel 실패: unittest_request_timeout_scheduler (Timeout 10 s; 단독 13.8 s).
- 진단 커밋 push → run 34193225840 (임시 printf: ctx_options blocked send errno, wait_for_raw_close last_rc/errno, extra completion 내용).

## 07:25 진단 2회차 (run 34193638775)
- 좁은 regex 단독 실행에서 **test_ctx_options(1.41 s)·test_xpub_nodrop(1.38 s) PASS** → 두 건은 full serial run에서만 재현되는 간헐 실패.
- wait_for_raw_close 실패 내용: `last_rc=1 errno=60 (ETIMEDOUT) bytes_drained=67` — 3 s 안에 EOF/RST가 오지 않고 handshake 바이트 67개만 들어옴.
- wake_invariants extra completion: `kind=3 id=1 send_result=0 terminal_errno=0` (kind 3 = ?), 즉 예상 밖 completion 1건.
- Linux 로컬 검증(mkdtemp 수정 포함, build-dev): 9개 대상 테스트 전부 PASS. ctx_term 0~7 ms.
- run 34194246491: setup/teardown 시간 + raw-close hex 진단.

## 08:05 최종 패치 push
- 진단 커밋 되돌림(core/tests 원복). 최종 패치 2개: core/CMakeLists.txt mkdtemp 탐지, core/tests/CMakeLists.txt APPLE TIMEOUT 3배.
- Linux: 대상 9개 테스트 100% PASS, APPLE 분기 미적용 확인(test_endpoint_release TIMEOUT=10 유지).
- 패치 저장: ~/project/zlink-work/all-artifacts/MAC-1.patch. 보고서 작성 완료.
- 최종 macOS run 34195054163 결과 대기 중.

## 08:35 최종 run 1차 결과와 보정
- run 34195054163: serial 8 → **5** 실패(test_endpoint_release 해소, 15.84 s Passed), 총 832.8 → 716.8 s.
- 다만 parallel 그룹의 unittest는 TIMEOUT 스케일이 안 먹었다 — unittest는 `core/tests/unittest/`(별도 CMake 디렉터리 스코프)에 등록되어 부모의 DIRECTORY TESTS 루프에 안 잡힘.
- 스케일 로직을 `core/tests/scale_test_timeouts.cmake`로 빼고 두 디렉터리에서 include. `-DAPPLE=1` 강제 구성으로 30/30/360/540 s 스케일 확인, 실제 Linux 구성에서는 10 s 유지 확인.
- 재실행 run 34196588768 대기.

## 09:20 완료
- 최종 run 34196588768: serial 149 중 **5 실패**(이전 8), parallel 57 중 **0 실패**(이전 1). serial 832.8 s → 716.0 s.
- 남은 5건은 보고서 §3에 근거와 함께 기록(수정 미완). 브랜치 wip/mac-1 push 상태 유지, 패치 저장 완료.
