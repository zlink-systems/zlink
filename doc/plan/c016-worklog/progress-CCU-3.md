
## 19:0x — B-CCU2-3/1/2 구현 완료(빌드 OK)
- applied plan에 3필드 추가(application_auto_role, application_auto_direction_count, max_planned_queue_id).
- registry 복제 상태 8개 + record 2개(plan_member, plan_maximum_bytes) 전부 삭제. invalidation flag 삭제.
- 힙 할당 제거(고정 배열 2), ctx 경계에 bad_alloc/EFAULT backstop.
- pipe _hwm 적용을 record 앞으로, recalc mutex 안에서 수행.
- 다음: 대상 ctest, 새 unittest 작성.

## 신규 테스트 통과
- unittest_auto_hwm_incremental_plan 7/7 PASS. 대상 suite 31/31.
- 다음: until-fail:5, 전체 ctest, TSan, 벤치.

## 완료
- until-fail:5 32×5 PASS, 전체 ctest 211/211 PASS, TSan 9/9 PASS(report 0), CCU 4000 212.7 kops PASS.
- 보고서 core-rf-CCU-3-report.md, 패치 ~/project/zlink-work/all-artifacts/CCU-3.patch(core/** 누적).
