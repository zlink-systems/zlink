# review-ALL-2 진행

- 시작: 2026-09-08 13:21 KST. 상한 60분.
- 대상: `/home/hep7hep7/project/zlink-work/all`, `wip/0.17.3-all2`, HEAD `1a79625d3db197431dd08876bca9af483b403ed2`.
- 범위: 부모 대비 63개 파일 정적 리뷰. 소스·스펙·테스트 수정, 빌드·실행, 커밋 없음.
- 13:22 KST: 공통 규칙·문서 지침 확인. ALL-1/2 및 ST 리뷰의 주장과 재현 순서 수집 중.
- 중간 확인: ST-1/2/3 반례와 lifecycle turn 전이 대조 중. 별도 receive 모드 제거와 RAII 진입 확인. ALL-2 보고서는 현재 두 저장소에서 검색되지 않아 경로 요청, 나머지 리뷰 계속. WS 독립 범위는 읽기 전용 하위 리뷰에 배정(소스 직접 재검증 예정).
- 중간 확인: command drain 전체 turn·async detach/reschedule·progress seq_cst 등록 확인. command 지속 유입의 기아/종료 지연은 별도 잔여 후보. pipe HWM/max release/acquire와 snapshot 회계 대조 중. WIN-1 EAGAIN 재대기와 RtlGenRandom은 부모 대비 보존됨.
- 중간 확인: D-ALL-1은 route map 부재를 확인한 뒤 NOT_FOUND/ENOENT를 검사하므로 정적 해소. HWM/max C3 발행·기존 stamp 기반 stale record 폐기와 cold transport gate 확인. lock/msg 원시 로그 537372/64079 및 Release/기준 dev 차이를 확인. 예외·lock 순서·foreign pipe 변경 경계 추가 추적 중.
- 중간 확인: ALL-2 보고서를 메인 Git HEAD에서 확보해 충돌 4파일·대조표·최종 TSan 209/210 기록 확인. 종료 경계에 남은 foreign socket turn 취득과 endpoint wait를 추적 중(부모에도 있던 경계와 이번 확대 효과 구분). 정적 리뷰 외 실행 없음.
- 13:34 KST: 시스템 시각으로 진행 기록 시각 정정. PAIR complete-record 사전 검증 후 peer max atomic 변경으로 assertion에 도달하는 신규 차단 경로 확인. WS mask·executor 정적 검토 완료, 메모리/복수 보고서 판정 근거 분리 중.
- 13:45 KST: 보고서 초안 작성. 신규 B-ALL2-1을 독립 대조에서도 확인; release build의 assertion도 활성. WS report provenance는 W로 분류(단일 report 결과와 구분). 파일·행과 판정표 최종 확인 중.
- 13:49 KST: 최종 리뷰 완료. review-all2.md에 10개 요청 범위의 판정표·전이표·B 1/W 6/S 2·사양 문장 초안 기록. 신규 차단 1건으로 채택 불가. 대상 branch/HEAD 유지 및 worktree clean 확인. 작성 파일은 요청된 보고서/진행 파일뿐이며 소스·스펙·테스트 수정, 빌드·실행 검증, commit 없음.
