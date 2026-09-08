# review-CCU-4 진행

- 2026-09-08 20:58 KST: 리뷰 시작. 공통 규칙과 `doc/AGENTS.md` 확인.
- 범위: `ccu2` 미커밋 diff, CCU-3 지적 4건, CCU-4 보고서, Auto-HWM 및 mailbox/timer 스펙의 정적 대조.
- 제약: 읽기·리뷰 전용. 빌드·테스트·실행·소스/스펙/테스트 수정·커밋 없음.
- 현재: 입력 문서와 실제 patch 범위를 대조 중.
- 2026-09-08 21:07 KST: 상태/타이머 전이 대조 완료. deferred shrink에서 `recalc_due()`가 계속 참인 반례와, 증분 publish가 deadline을 0으로 지워 연속 attach마다 runtime timer를 다시 건드리는 반례를 확인.
- 현재: 신규 테스트가 두 반례와 CCU-3 경고를 실제로 고정하는지, inproc mailbox 구동 및 플랫폼/공개 API 범위를 확인 중.
- 2026-09-08 21:09 KST: 정적 리뷰 완료. 신규 차단 2건(B-CCU4-1 deferred shrink 오인, B-CCU4-2 attach별 timer 재무장), 신규 경고 3건을 확정.
- 보고서: `/home/hep7hep7/project/zlink/doc/plan/c016-worklog/review-ccu4.md`.
- 최종 판정: 차단 2건, 채택 불가. 빌드·테스트·실행 및 소스·스펙·테스트 수정은 수행하지 않음.
