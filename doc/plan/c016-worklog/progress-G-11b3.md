# G-11b-3 진행

- 2026-09-07: 공통 규칙과 독립 리뷰 확인. 기존 `g11b2` worktree의 미커밋 3-file patch를 보존한 채 관련 보고서·스펙·원칙과 구현을 재검증 중.
- 2026-09-07: 리뷰 기준 detached HEAD `dfe6ec2829`로 2차 diff를 별도 patch에 보존하고 pristine 상태를 복원. Release/LTO local Core 빌드 PASS; `ninja`가 없는 상태에서 지정 `PERF_LOCK`으로 with_stream zlink/asio all-size 3회 기준 측정 중.
- 2026-09-07: pristine 3회 측정 완료(64/1024/65536 B zlink 297.85/273.14/33.56, asio 366.67/335.83/40.95 kops; mismatch 0). 2차 patch를 복원하고 공통 seqlock helper로 reader UB를 제거했으며 local read pair의 완료/reset 발행과 monitor snapshot을 같은 sequence로 묶음.
- 2026-09-07: 3차 dev build PASS. 현재 configure에서 지정 정규식은 105개를 선택(2차 77개 이후 테스트 증가); 각 test `until-fail:5` suite 실행 중.
- 2026-09-07: suite 장시간 router/handover 항목을 포함해 진행 중이며 현재까지 첫 실패 없음. 별도 wake/close/monitor 반복은 suite 종료 뒤 실행 예정.
- 2026-09-07: 정규식 suite 83/105까지 전부 PASS; single-lane monitor status/accounting 반복도 통과. 현재 30초짜리 wake invariant 항목 반복 중.
- 2026-09-07: 지정 suite 105개 ×5 = 525/525 PASS(633.41 s). lost-wake 5종 `until-fail:20` 실행 중이며 현재까지 실패 없음.
- 2026-09-07: lost-wake 장시간 target 반복 계속 PASS. 별도 테스트의 횟수·timeout은 변경하지 않음.
- 2026-09-07: lost-wake 5종 100/100 PASS(619.62 s). close ×50은 후반 1회에서 기존 알려진 간헐과 같은 10.02 s CTest timeout(앞의 with-monitor PASS 후 without-monitor 미완료); timeout/반복 추가 없이 원인 기록. monitor/accounting 18종 ×10 실행 중.
- 2026-09-07: monitor/accounting은 17 target 10/10 PASS, 기지 간헐 `test_single_lane_flow_snapshot_accounting` 8 PASS 뒤 1 FAIL(5 s wait)이며 즉시 단독 재실행 PASS; close도 단독 재실행 PASS. pristine TSan 4 target은 기존 기준과 같은 10+1+9+0.
- 2026-09-07: 최종 TSan도 10+1+9+0, signature delta 0(ypipe 17 + 기존 receive guard 3, ledger 신규 0). 축소 stream_tcp 15,628.428 Ir/msg·21.7794 lock/msg; pristine 대비 -0.74% Ir·-2.2734 lock/msg. Release/LTO hotpath 5셀 모두 PASS.
- 2026-09-07: with_stream 3회 최종 완료(mismatch 0). zlink/asio 비율은 64 B 0.8123→0.8971 개선, 1024 B 0.8133→0.7088(-12.85%), 64 KiB 0.8196→0.6839(-16.55%) 악화. 최종 run의 system CPU가 pristine 약 37% 대비 52~61%였으나 재측정하지 않고 감독관 채택 판단으로 넘김.
- 2026-09-07: `core-rf-G-11b3-summary.md` 작성. correctness 구현·검증 완료, 보호 경로 무변경·commit/stash 없음. 성능 비율 악화 2셀 때문에 최종 채택 결정은 감독관에게 인계.
