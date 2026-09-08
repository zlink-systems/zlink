# G-11b-2 진행

- 2026-09-07: 공통 규칙, synchronization spec, G-11b 1차 보고서와 이전 브리프 확인. 메인 worktree의 기존 변경은 보존하고 detached worktree 준비 중.
- 2026-09-07: `g11b2` detached worktree를 `origin/main`에 생성. Monitoring §6.3의 pipe 합계 field 군 일관성 때문에 msgs/bytes 독립 atomic만으로는 부족함을 확인; full-width 두 값은 seqlock snapshot으로 묶는 설계를 선택.
- 2026-09-07: pristine dev 빌드 진행 중(JOBS=4). Auto-HWM §4의 charge 시작(frame write)·종료(complete-message dequeue)와 Connection memory §3.1의 queue 보유 기간 회계 문장을 재확인; 값/경계는 유지하고 publication만 변경한다.
- 2026-09-07: pristine dev build PASS. 수동 TSan 네 target baseline은 20건(10+1+9+0)으로 1차와 동일. 축소 stream_tcp baseline은 314,895,063 Ir/20,000=15,744.753 Ir/msg, pthread_mutex_lock 481,055/20,000=24.053/msg.
- 2026-09-07: 구현 적용 중: session I/O writer의 write/flush만 `_out_sync` 생략, written/peer-credit/out-active C3 release/acquire, false→true CAS, full-width msgs/bytes seqlock snapshot과 monitor 합계 단일 snapshot 경로를 추가. 직접 접근 전수 치환 완료, 증분 컴파일 단계.
- 2026-09-07: 구현 후 dev build와 `git diff --check` PASS. monitor 합계는 `_out_sync`로 topology/peer-credit field 군을 고정하고, session writer ledger만 C3 seqlock acquire snapshot으로 읽도록 경계를 정리. 관련 테스트 반복을 시작.
- 2026-09-07: 관련 정규식 suite 77개를 5회, 총 385/385 PASS. 1차에서 간헐 실패했던 stream multiclient regression도 5회 모두 PASS. lost-wake 5종 `until-fail:20` 검증 시작.
- 2026-09-07: lost-wake 5종 100/100 PASS(647.51 s), close completion poller 50/50 PASS(46.33 s). 수동 TSan 4 target은 pristine과 동일한 20건(10+1+9+0), signature도 ypipe 17 + 기존 receive guard 3으로 정확히 동일하며 신규 pipe ledger race 0.
- 2026-09-07: monitor가 읽는 같은 성질의 oversize count/max도 전수 확인해 C3 atomic에 포함. 최종 dev build 및 monitor/wake/accounting 24/24 PASS, 최종 TSan도 pristine과 같은 20건. Release/LTO hotpath 5셀 PASS, 최종 축소 stream_tcp 15,673.668 Ir/msg·22.057 lock/msg(기준 대비 -0.45% Ir, -1.996 lock/msg).
- 2026-09-07: 최종 with_stream(local Core, zlink/asio, CCU 1000, all size) 6/6 완료·mismatch 0. zlink throughput은 64/1024/65536B에서 271.74/244.63/31.57 kops. 결과 보고서 작성 단계.
- 2026-09-07: `core-rf-G-11b2-summary.md` 작성 후 독립 검토 완료. `_out_active` CAS 범위를 credit recovery로 한정하고 lifecycle·flow release store와 구분해 보고서 표현을 코드와 일치시켰다. Auto-HWM §4와 Connection memory §3.1의 회계 경계 문장도 직접 인용했다. 변경은 worktree의 Core 내부 3개 파일과 메인 저장소의 progress/summary에만 있으며 공개 header·vers·spec·test 변경 없음.
