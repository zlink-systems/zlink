# MP-9 진행

- 2026-09-08 시작: 대상 worktree 상태와 공통 규칙 확인, MP-8 이전 누적 diff를 지정 scratchpad의 `mp8-before-mp9.patch`로 보존했다.
- 입력 확인 중: 3차 리뷰 B301/W301/W302/W303/S301/S302, MP-8 보고서, D-B212·D-B213, 확정 계약과 현재 구현을 대조한다.
- 진단 완료: B301은 `complete_reply_from_transport()`의 pending-miss 및 payload-export OOM 원본 해제가 registry/physical/owner 경계 안에서 실행되는 경로다. 폐기 레코드 하나를 owner turn의 로컬 저장소로 분리하고 owner 경계 밖 공용 정리로 해제하는 방식으로 수정한다.
- 정리 방향: SEND DONTWAIT staging admission은 helper record detach 직후 complete scope로 인계해 중첩을 없애고, `submit_buffered_request_step()`의 도달 불가 MORE 분기를 제거한다.
- 1차 구현: pending-miss와 payload-export OOM 레코드를 owner turn 로컬 `completion_discard_t`로 분리하고, poller/NONE/async/send 보조 drain 모두 owner gate 밖의 `release_completion_discard()`를 사용하도록 연결했다. SEND admission 인계와 REQUEST dead MORE 제거도 반영했다.
- B301 단독 검증: 늦은 zero-copy REPLY의 무등록 NONE/poller 두 변형과 payload-export OOM 변형이 모두 PASS했다.
- 테스트 강화: W303은 `B MORE open → A thread exit → B FINAL reclaim` 순서를 고정했다. W301은 150~400 ms RCVTIMEO, 750~1000 ms 독립 watchdog, monitor mailbox waiter count로 registration 1→0·0→1 양쪽 실제 sleep을 관찰한다.
- W301/W303 단독 검증: 두 lost-wake case와 helper ownership 17개가 PASS했다. 임시 진단 출력은 제거했다.
- W302 준비: 보존 `mp7-before-mp8.patch` SHA-256 `96f4c07e0e38811a1e440649f7f7c9148f54fad62e632019cbb0c92cefa74221` 확인. clean HEAD 별도 build와 최종 build를 같은 Release+LTO/callgrind 조건으로 비교한다.
- W302 측정 완료: 동일 5,000회 구간에서 MP-7 `18,443.1536`, 최종 `16,352.5132 Ir/msg`로 `-2,090.6404 Ir/msg`(`-11.335%`)를 확인했다. async mailbox handler 호출은 4,996→70, completion 처리/완료는 각각 정확히 5,000회를 유지했다. raw/annotate/top20 자료를 scratchpad에 보존했다.
- 최종 검증 시작: 최신 소스로 dev build를 갱신한 뒤 전체·관련 반복·lost-wake 반복, ASan+LSan/TSan, Release hotpath 5셀 순서로 실행한다.
- 최신 dev 전체 build PASS, dev CTest `209/209` PASS(`hotpath_gate` 제외, 244.34초). 이제 관련군 반복과 lost-wake 전용 반복을 실행한다.
- 반복 검증 PASS: 관련 97 target `until-fail:3`, 변경 12 target `until-fail:10`, lost-wake 두 case 각각 `until-fail:20`.
- sanitizer PASS: 최신 소스로 ASan target 12개를 재빌드해 `ASAN_OPTIONS=detect_leaks=1`에서 12/12, TSan target 12개를 재빌드해 `setarch x86_64 -R`와 기존 suppression에서 12/12 통과했다.
- Release+LTO lib/static hotpath runner 갱신 PASS. `PERF_LOCK` 아래 load average `0.90/1.75/1.25`에서 5셀을 1회 측정했고 모두 MP-8 대비 ±1% 안이다(reqrep `16,347.604 Ir/msg`, MP-8 대비 +0.656%). 공식 reference는 의도된 개선 때문에 reqrep 셀만 하한 FAIL이다.
- 최종 정적 점검: `git diff --check` PASS, `core/include`·`core/src/libzlink.vers` diff 없음. 보고서에 항목별 파일:행, 검증, hotpath, Callgrind top20·귀속을 정리한다.
- CMake가 single-lane executable을 29개 CTest case로 펼치는 범위도 별도로 `until-fail:10` 실행해 290/290 PASS했다. 신규·변경 반복 합계는 41 case, 410/410 PASS다.
- 완료: B301/W301/W303/S301/S302 수정, 전체·반복·ASan+LSan·TSan·hotpath 검증과 MP-7/최종 Callgrind 귀속을 모두 마쳤다. 보고서 `core-rf-MP-9-report.md` 작성, patch는 미커밋 상태로 유지했다.
