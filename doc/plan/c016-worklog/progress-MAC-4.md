# MAC-4 진행

- 시작. `wip/0.17.4`(b27f25dea1) → 브랜치 `wip/mac-4`, worktree `~/project/zlink-work/mac4`.
- 대상: 사전 run 34219639442 macOS ARM64 serial `63 - test_stream_packet_progress (Failed) 7.13 s`. 실패 문구 없음(build.sh에 --output-on-failure 없음).
- 가설: ALL-3의 WS shutdown fixture(ping/pong fence) 변경 + MAC-3의 `await_input` msleep 결합.
- 1단계: 진단 workflow 올려 실패 문구/재현율 확보.
- run **34221148788**(진단, `^test_stream_packet_progress$` until-fail:5): 1회차 PASS, 2회차 FAIL.
  문구: `test_stream_packet_progress.cpp:350 test_shutdown_during_drain: receive ignored shutdown until all partial input was drained`
  `shutdown: 262152 chunks at request, 0 at receive return`.
  → ALL-3의 ping/pong fence 단언(`TEST_ASSERT_EQUAL_UINT64(bytes.size()-1, f.pending())`)은 통과. 깨진 것은 **마지막 단언 `remaining > 0`**.
- Linux 계측: `at request 262152, at recv return 255561, after join 255561, recv_us=402` → recv가 ~6591청크만 소비하고 TERMINATED 반환.
- macOS는 join 후 0. recv 반환 시점 값이 필요 → 진단 추가 후 재실행.
- run **34222529635**(until-fail:20) 1회차에서 재현. `DIAG-MAC4 shutdown: 262152 at request, 0 at recv return (rc=0), 0 after join, recv_us=13449`.
  → recv가 262152청크를 13.4 ms 동안 **전부 소비한 뒤** TERMINATED 반환. Linux는 402 us/6591청크.
  청크당 속도는 두 플랫폼이 비슷하다 → 차이는 **shutdown이 보이기까지의 시간**(0.4 ms vs 13.4 ms).
- 원인(Core 결함): `ctx_termination.cpp:begin_shutdown_locked()`가 모든 소켓의 `stop_monitor(false)`를 **먼저** 돌리고 그 다음에 `stop()`(= `_ctx_terminated` 게시)을 한다. 모니터 teardown은 워커를 기다릴 수 있어 macOS에서 ~13 ms. 게다가 `socket_base_msg.cpp`의 블로킹 recv 루프 3곳은 `_ctx_terminated`를 **진입 시에만** 확인하고 루프 안에서는 명령 도착만 기다린다(블로킹 send는 이미 매 턴 확인).
- 수정: (1) `publish_ctx_terminated()`를 모든 소켓에 **teardown 전에** 먼저 게시, (2) 블로킹 recv 루프 3곳에서 매 턴 `_ctx_terminated` 확인 → ETERM. public 헤더/ABI 무변경.
- Linux 확인: `at request 262152, at recv return 262089, recv_us=19` — 정확히 pump 1라운드(63청크) 뒤 반환.
- run **34223549078**: macOS `test_stream_packet_progress` **20/20 통과**(수정 전 1~2회 내 실패).
- Linux 전체 ctest 211/212 — 실패는 `hotpath_gate` 뿐이며, 변경 없는 mac1 dev 트리에서도 동일 수치로 실패(dev/LTO OFF 빌드 성질).
- run **34224529148** full: **serial 150/150, parallel 57/57, success**.
- MAC-4.patch(5파일 +49/−1), 보고서 작성 완료. 종료.
