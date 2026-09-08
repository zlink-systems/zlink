# WIN-2 진행 (MERGE-1 후속: MAC-3 적용 + Windows writable_resubmit 원인 확정)

- 시작 21:30. worktree ~/project/zlink-work/rel174 (wip/0.17.4, head 84d25131a6).
- 상태: MAC-3 증분 확인 중.
- 21:45 MAC-3 증분 적용·빌드·suite until-fail:5 9/9 PASS(172.75s)·커밋 bdedf4300d·push 완료. core/src·core/include 무변경 확인.
- 21:55 WIN-2 원인 확정: **Smart App Control(SAC) 차단**. CodeIntegrity 이벤트 3033/3077, Policy ID {0283ac0f-...}, VerifiedAndReputablePolicyState=1, UsermodeCodeIntegrityPolicyEnforcementStatus=2. Core 결함 아님.
- 22:40 나머지 3개 Windows 실패 재확인(수정 없음). hotpath 셀 idle 재측정 15609.787(0.9486) → reference 갱신, 5셀 gate PASS, 커밋 b27f25dea1 push.
- 23:05 보고서 doc/plan/c016-worklog/win-2-summary.md 작성 완료. 종료.
