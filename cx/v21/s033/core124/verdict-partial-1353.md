# 판정

최종 판정: **미판정 — release gate 미완료**.

C 대 B1 첫 runs 3 gate는 FAIL이며, 실패 cell 222개와 aggregate 152개가 있다. 지정된 runs 5 재확인은 수행하지 못했다. B2-single preflight가 두 차례 30분 제한에 도달했고 B2-multi도 시작하지 못해 C 대 B2 및 B2 대 B1 A/A 비교가 없다.

§5.1 candidate 검사는 통과했다: `hotpath_gate` PASS, `test_wake_invariant*` 10회 PASS. §5.2의 최종 release 비교 verdict는 B2 baseline과 실패 재확인이 완료되기 전까지 산출할 수 없다. 세부 내용은 `report.md`에 있다.
