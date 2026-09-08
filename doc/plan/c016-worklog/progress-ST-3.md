# progress ST-3

- 07:45 시작. 베이스라인 diff/테스트 파일을 scratchpad에 보관(`st2-before-st3.patch`, `st2-before-st3-test.cpp`).
- 08:00 review-st2 / 소스 읽기 완료. 수정 설계 확정.
- 08:10 코드 수정 완료(socket_runtime.hpp, socket_base_lifecycle.cpp, socket_base_msg.cpp, 신규 테스트).
  - B-ST2-1: `release_receive_sync_from_async_owner()`가 `receive.sync`를 잡고 store.
  - B-ST2-2: `progress_epoch`/`waiters`를 atomic화.
  - W-ST2-3: command 대기 CV를 비재귀 `lease_handoff_sync`+전용 CV로 분리, notifier 1개(`publish_receive_progress_locked`)로 통합, 주석 수정.
  - B-ST2-3/4: 테스트를 per-thread io_context 소유 + async 연산 + 소유 thread cancel로 재작성.
- 08:12 빌드 시작.
- 08:20 검증 완료: dev 빌드 OK / 신규 테스트 20/20 / 관련 98개 3회 100% / 전체 210개 100% / wake-invariant 20회 exit 0 / TSan 잔여 11건 전부 mailbox(receive 소유권 0건) / ASan 2/2 / 공개 인터페이스 diff 없음.
- 08:25 재현력 대조: core/src만 되돌려 재빌드 후 3/3 FAIL(fq.cpp:39 `_pipes.empty()` assertion). patch 재적용·재빌드 후 20/20 재확인.
- 08:35 보고서 core-rf-ST-3-report.md 작성 완료. 커밋하지 않음.
