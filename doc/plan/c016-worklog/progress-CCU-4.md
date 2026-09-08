# CCU-4 진행 (상한 1.5 h)

- B-CCU3-1 수정: 증분 성공 뒤 `schedule_auto_hwm_convergence()` 호출. pending generation은 올리지 않고
  debounce 마감만 재무장. 수렴 의무는 plan 자신이 표현(`total_applied > total_planned`)하고
  `recalc_due()`가 그걸 읽는다 → 새 state/flag 0, guard 무변경, 연속 증분 유지.
- 테스트 전면 개편(W-CCU3-1/2/3): settle()로 pending 소진 후 attach, 증분 진입을 applied>planned로 assert,
  plan 소유 field 전부 비교, fallback은 applied까지 완전 동일 요구, 동시 attach에 시작 barrier + generation 정확히 +1.
- 9/9 PASS. 다음: 주석 정정, until-fail:5, 전체 ctest, TSan, CCU 4000.

## 완료
- 신규 unittest 9/9, until-fail:5 32×5, 전체 211/211, TSan 9/9(report 0).
- CCU 4000: 초안 150.9 → 타이머 1회 무장 최적화 후 203.2 kops(idle). 수렴 예약 유지.
- 보고서 core-rf-CCU-4-report.md, 패치 CCU-4.patch.
