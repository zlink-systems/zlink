# review-SD-2 진행

- 상태: 완료(정적 리뷰만 수행, 빌드·테스트·benchmark 실행 없음)
- 기준: base `84d25131a6`, 고정 artifact `~/project/zlink-work/all-artifacts/SD-2.patch`; `sd1` worktree는 읽지 않음
- 확인 완료: 공통 규칙, SD-2 보고서, cppserver 대조 §6, 지정 스펙, B2 상태/성장 경로, WS/WSS Beast write 경로, 계측 app, D-f 검색, 0.17.3 WS 수신 경로
- 최종 판정:
  - B2 1-hit 2배 성장과 기존 max/shrink 규칙은 코드상 유지; async/speculative의 read byte 상태에서 stale-short 반례 없음
  - 1 MiB는 benchmark의 `rcvbuf=1 MiB`에서 유도된 상한이지 런타임 기본값이 아님
  - ZMP 스펙은 WebSocket message 경계와 ZMP frame 경계를 동일시하지 않고 양방향 split/coalesce를 허용
  - WS 기본 gather는 단일 message만 꺼내 bounded multi-frame batch를 우회하고 128 KiB max 검사도 없이 Beast write로 제출하므로 B-SD2-1 차단
  - B1은 B2-only artifact 구조상 분리 가능하나 누적 patch hunk가 겹쳐 선택 적용 필요
  - TSan/관련 suite/hotpath 보존 로그는 작성자 결과와 일치하나 B1 제거 후 정확한 채택 diff 검증은 SD-3가 필요
  - public header·`libzlink.vers` 변경 없음, 채택 부분에 새 플랫폼/Boost API 없음
- 산출물: `doc/plan/c016-worklog/review-sd2.md`
- 최종: 차단 1 / 현재 채택 부분 채택 불가
