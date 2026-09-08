# MERGE-2a 진행 (MAC-4 적용 + macOS build.sh ctest 옵션)

- 시작 23:10. worktree ~/project/zlink-work/rel174 (wip/0.17.4, head b27f25dea1).
- 23:25 MAC-4 --3way 적용(충돌 0), public iface diff 0, dev 빌드 성공, 대상 suite 22개 until-fail:5 PASS(58.43s).
- 23:35 전체 ctest 211/211 PASS(232.91s). TSan(build-tsan 재빌드) -R 'stream|ctx|close|term' 29/29 PASS, 경고 0(23.54s).
- 23:40 커밋 6832d55e43 push. core/builds/macos/build.sh는 이미 CTEST_COMMON=(--output-on-failure)로 두 ctest 호출 모두 적용돼 있어 변경 없음(중복 플래그 추가 안 함).
- 23:42 코디네이터 요청으로 이후 빌드·측정 중단, 대기 상태. (요청 도착 시점에 TSan은 이미 완료)

## MERGE-2b
- 23:55 CCU-5.patch --3way 적용: 충돌 0(13파일, untracked 테스트 포함). MAC-4가 건드린 socket_base.cpp/hpp도 양쪽 hunk 모두 유지 확인.
- 스펙 2파일은 patch에 포함되지 않음(grep 'doc/spec' = 0). 확인 결과 **우리 브랜치가 main c55da9c6c0의 D-H1 스펙 문장보다 뒤처져 있다**(브랜치는 514906a682 기준 rebase). 스펙은 편집하지 않았고 최종 rebase 때 main 문장이 들어온다.
- W-CCU5-3 주석 정정 1곳: ctx_auto_hwm_recalc.cpp:141-145 (주석만).
- 커밋 e215322ea5 push. 빌드·테스트는 보류 중(SD-2 측정 창).
- (재개) MERGE-2b 검증 완료: dev 빌드 PASS, 대상 suite 37개 until-fail:5 PASS(85.49s), 전체 ctest 212/212 PASS(236.39s), TSan(stream|ctx|auto_hwm) 27/27 PASS·경고 0(19.51s). head e215322ea5.
