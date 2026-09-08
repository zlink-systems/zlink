# MP-4 진행

- 2026-09-07 19:55 KST: 공통 규칙 확인. `mp2`의 MP-3 미커밋 diff를 지정 scratchpad의 `mp3-before-mp4.patch`로 보존(4,420줄). detached worktree와 기존 변경 24개 파일 확인.
- 2026-09-07 19:58 KST: runtime 필수 설계 원칙, MP-3 보고서, helper/TLS 구현과 관련 스펙 근거 조사 시작.
- 2026-09-07 20:04 KST: C3 counter와 TLS-owned `shared_ptr` 참조 변경 적용. map 생성·삭제·만료 회수·close swap 아래에서만 counter를 release 발행하고 single FINAL은 relaxed zero에서 mutex/TLS를 생략. dev build 성공.
- 2026-09-07 20:08 KST: 직접 영향 5 target 통과. 관련 정규식 80 target × until-fail:3 실행 중; multipart/helper/request/reply 및 ROUTER handover 구간까지 연속 통과.
- 2026-09-07 20:13 KST: 관련 정규식 80 target × 3회 = 240/240 통과(413.12초). 기존 ASan/TSan 트리를 MP-4 변경으로 증분 갱신한 뒤 MP-3 직접 target 재실행 예정.
- 2026-09-07 20:17 KST: ASan+LSan 6/6 통과, TSan 신규·변경 8/8 통과. sanitizer 오류·신규 race 없음. release lib와 hotpath runner 갱신 단계로 이동.
- 2026-09-07 20:19 KST: Release/LTO hotpath 5/5 PASS. 동일 MP-3 runner의 MP-4 dev 결과는 single 23.897ms, 확인 재측정 23.248ms(평균 23.573ms, main 대비 +4.81%), 4-thread 102.501ms(780,483 record/s).
- 2026-09-07 20:26 KST: 보고서 작성과 교차언어 위임 경로·스펙 불변 감사 완료. 최종 comment 포함 dev 재빌드 성공 및 직접 영향 5/5 재통과. patch 미커밋, 공개 API/ABI diff 없음.
