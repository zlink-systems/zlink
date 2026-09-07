# S-11 진행 (pipe_t::_in_active 소유권 통일)

- worktree ~/project/zlink-work/s11 @ baaa68d67b (detached). 커밋 없음.
- 02:5x build-dev + build-tsan 구성/빌드
- 03:0x TSan before: `_in_active` race 확인 (process_activate_read pipe.cpp:2751 vs read_internal :1196), test_two_poller_wake 경고 3
- 03:1x 원인 규명: receive lease(lock-free public recv)와 receive.sync(process_commands)는 서로 배제하지 않는 두 배타 도메인
- 03:2x 설계 B 채택: `_in_active` → atomic<bool> acquire/release. 같은 축으로 `_state` → atomic + `_state_active` 미러 삭제(상태 −1)
- 03:3x TSan after: `_in_active` race 소멸, test_two_poller_wake 경고 0/exit 0
- 03:4x ctest 5회(1건 pre-existing 간헐 test_close_completion_poller_release), lost-wake --repeat until-fail:10 2회 전부 통과
- 03:4x callgrind 축소셀 Ir/msg 9,474 (S-1 9,887 대비 −4.2 %), 호출 횟수 진리표 동일
- 03:5x with_stream: 1차 일괄은 외부 부하로 폐기, 크기별 재측정 64/1024/65536 = 266.5 / 256.2 / 32.8 kops
- 완료. 보고서 core-rf-S-11-summary.md
