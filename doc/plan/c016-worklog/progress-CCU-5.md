# CCU-5 진행 (상한 1 h)

- review-ccu4 읽음. B-CCU4-1(applied>planned는 정상 deferred shrink와 구분 불가·장주기 반복),
  B-CCU4-2(증분 record가 deadline을 0으로 지워 attach마다 timer 재무장).
- 규칙: 수렴 의무 = deadline 자체. arm_debounce는 deadline이 0일 때만 세우고 wake 1회,
  deadline 해제는 full pass 완료 지점 한 곳, recalc_due는 "deadline 있고 지났다"만 본다.

## 구현+테스트 완료
- arm_debounce: deadline 0일 때만 세우고 wake 1회. clear_debounce는 full pass 완료 지점 한 곳.
  record_applied_plan은 deadline 미접촉. recalc_due는 deadline만 본다.
- 신규 테스트 10/10 PASS (수렴 1회·반복 없음, deferred shrink 반례, 각 attach 증분 진입 assert, barrier).
- 다음: until-fail:5, 전체 ctest, TSan, CCU 4000.

## 완료
- 신규 unittest 10/10, until-fail:5 32×5, 전체 211/211, TSan 9/9(report 0), CCU 4000 209.1 kops PASS(idle).
- 보고서 core-rf-CCU-5-report.md, 패치 CCU-5.patch.
