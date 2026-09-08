# G-11a 진행

- 시작 12:0x. worktree ~/project/zlink-work/g11a (detached 08da256f1e).
- 읽기 완료: _common-rules, 브리프, G-11 본문+부록 A, framework 06 §4-5 / 07 §6.3.
- PAIR 예외 출처 추적: d548675abe(2026-08-27) "The frequent PAIR activation/pending
  commands remain lock-free" — perf 사유. 8b6c2aa906(2026-09-03) PAIR per-message overhead 축소.
- 다음: PAIR blocking send가 turn을 놓는지 확인 → 예외 제거 시도.
- 11:50 변경 적용 완료: socket_base_lifecycle.cpp process_commands가 항상 turn을 잡는다.
  PAIR 예외·reaper-close 예외·pair pipe-lifetime probe/retry 경로 전부 삭제(is_pair_pipe_lifetime_command 포함).
- unittest_receive_transaction의 test_pair_commands_only_fence_pipe_lifetime_transitions가
  "PAIR activation은 fence를 잡으면 안 된다"를 못박고 있어 실패 → 기대값을 turn 획득으로 갱신하고 이름 변경.
- dev 빌드 OK, 대상 61개 suite 1회 전부 PASS. 다음: 5회 반복 + stress + TSan + 계측.
- 12:20 완료. 보고서 core-rf-G-11a-summary.md 작성. 커밋하지 않음.
