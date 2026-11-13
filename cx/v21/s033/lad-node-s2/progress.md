# Node L4 / L5~L7 측정 진행

- 시작 전 상태: HEAD `c019495a82c40247f0d3e5d9a16b5c6673b11495`, clean, branch `exp/4350-node-ladder`; 기준 merge-base는 최초 측정 직전 기록.
- call-shape: [call-shape.md](call-shape.md). 각 run 전에 receive/reply 대비, per-message 비용, HEAD/dirty/base, worktree 산출물 경로와 SHA-256을 아래에 기록한다.
- 기준 binding: lad-node-s1/report.md의 2026-11-13b binding 표. 사용자 지정에 따라 같은 일자·동일 binding build 기준을 재사용한다.
- 관련 Framework dist 산출물 전수 mtime 확인: `.ts` 대비 오래된 `.js` 0개. contract 전용 TS 1개는 dist 출력 대상이 아니다. stale build가 없어 재빌드하지 않았다. 실행은 `SKIP_BUILD=1`이다.
2026-11-13T22:15:48Z pre-hold build_test_wait_seconds=600 still_active=11
2026-11-13T22:15:48Z lock occupied; waiting outside the lock
2026-11-13T22:15:58Z lock occupied; waiting outside the lock
2026-11-13T22:16:08Z lock occupied; waiting outside the lock
2026-11-13T22:16:18Z lock occupied; waiting outside the lock
2026-11-13T22:16:28Z lock occupied; waiting outside the lock
2026-11-13T22:16:38Z lock occupied; waiting outside the lock
2026-11-13T22:16:48Z lock occupied; waiting outside the lock
2026-11-13T22:16:58Z lock occupied; waiting outside the lock
2026-11-13T22:17:08Z lock occupied; waiting outside the lock
2026-11-13T22:17:18Z lock occupied; waiting outside the lock
2026-11-13T22:17:28Z lock occupied; waiting outside the lock
