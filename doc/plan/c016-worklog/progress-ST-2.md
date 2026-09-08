# progress ST-2

- 06:20 시작. worktree /home/hep7hep7/project/zlink-work/st1, 사전 백업 `<scratch>/st1-before-st2.patch` + `test_stream_concurrent_pull_send.cpp.orig` 완료.
- 06:20~06:50 리뷰·ST-1 보고서·코드 정독. B-1 소유권 프로토콜 설계 확정:
  `receive_owner`에 `command`(임시 command turn)와 `public_waiting`(lease + 대기 중인 command turn) 값을 추가,
  lock-free lease 실패자는 전부 `receive.sync`에서 블록 후 **상태어 재검증**, command turn은 spin 대신
  lease 해제(RMW exchange)의 직접 handoff로 깨어난다. hot path는 take 1 CAS + release 1 RMW 유지.
- 다음: socket_runtime.hpp / socket_base_lifecycle.cpp / socket_base_msg.cpp / socket_base_api.cpp 편집.
- 07:35 구현 완료(socket_runtime.hpp 프로토콜, socket_base_lifecycle.cpp command turn, socket_base_msg.cpp/socket_base_api.cpp 진입점, has_in), dev 빌드 성공.
- 07:40 테스트를 Boost.Asio로 재작성(B-2) — POSIX 헤더/raw fd 제거, 파라미터(100×40, HWM 4096, 64 B) 유지. 새 테스트 `until-fail:20` PASS(0.12 s).
- 다음: 관련 suite until-fail:3 → 전체 ctest → TSan → ASan.
- 08:30 관련 suite(95개) until-fail:3 100% PASS, 전체 ctest -E hotpath_gate 210/210 PASS.
- 08:55 TSan(무-suppression): 새 테스트 0경고, stream_socket/threadsafe 0경고. packet_progress에서 has_in의 lease화가
  pump의 notify_receive_progress_locked를 sync 밖으로 내보내는 회귀를 잡아냄 → has_in은 lease+sync를 함께 잡도록 수정, 재실행 후 해당 경고 소멸.
- 09:20 미수정 트리(core/src revert)에서 재작성 테스트 3/3 미완료(testutil alarm(121) SIGALRM kill) = FAIL 확인. 패치 복원·재빌드 후 새 테스트 20/20 PASS.
- 09:55 wake-invariant 라벨 4개 until-fail:20 완료.
- 10:35 ASan(build-asan 이어서 완료): 새 테스트 2/2 PASS, 보고 0건. TSan 재실행: 새 테스트/stream_socket/threadsafe 0경고,
  packet_progress 9(8 기존 mailbox + 1 progress_epoch), phase3 2(기존 mailbox).
- 10:40 최종: 관련 suite 95/95 ×3, 전체 210/210, wake-invariant ×20 통과. 공개 인터페이스 diff 없음.
  보고서 `core-rf-ST-2-report.md` 작성 완료. 패치는 미커밋 상태로 남김.
