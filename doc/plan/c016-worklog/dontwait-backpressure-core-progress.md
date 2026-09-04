# DONTWAIT backpressure Core progress

- 2026-09-04 시작: `main` branch 확인. 기존 `doc/plan/c016-worklog/**` 수정·untracked 파일은 사용자 작업으로 식별해 보호.
- 2026-09-04 규칙 확인: `doc/**`, `core/doc/**`, `framework/**`, `bindings/**`는 수정하지 않으며 raw header mirror 동기화만 예외. commit/push/branch 조작 없이 Core 구현·테스트·gate를 진행.
- 2026-09-04 정찰 시작: 판정 D-B79 계약, bb66e85376 도입 경로, SEND/REQUEST 공유 pending 코드, POLLOUT wake invariant 및 영향 호출부를 병렬 조사.
- 2026-09-04 1차 구현: 일반 DONTWAIT SEND를 complete-record 직접 admission 경로로 분리. 실패 시 pending submit·completion reservation으로 내려가지 않고 ID 0을 유지하도록 변경했으며, multipart helper의 EAGAIN handoff도 제거. REQUEST와 blocking NONE 경로는 아직 그대로 유지.
- 2026-09-04 REQUEST 분리: 공유 `send_pending_submit`을 `request_pending_submit`으로 좁히고 SEND completion reservation/owner/record 분기와 completion-capacity wake를 제거. `ZLINK_OPT_PENDING_MAX_*` accounting은 REQUEST queue에만 남김.
- 2026-09-04 multipart 보정: staged FINAL은 보관 없는 scoped one-shot으로 기존 multipart marker 안에서 전체 record를 판정. rollback 뒤 EAGAIN recovery를 재무장하고 lifecycle errno는 ordinary SEND에서 보존. blocking NONE은 새 경로를 건너뛰어 기존 park/SNDTIMEO 경로 유지.
- 2026-09-04 회귀 테스트 갱신: completion contract, REQUEST-only pending/cap, STREAM/TLS, single-lane, completion queue unit 및 wake-invariant 100-client 테스트를 새 계약에 맞춤. HWM/미연결 즉시 거절, ID 0, completion 부재, POLLOUT level, 호출자 버퍼 재생성 재전송을 포함.
- 2026-09-04 dev build: `ulimit -v 16777216; bash scripts/build-core.sh dev` 최종 성공 (`core/build-dev`). 앞선 두 회는 병렬 테스트 편집 중간 상태의 compile 오류를 고쳐 재실행함.
- 2026-09-04 집중 회귀 1차: wake-invariant 전체, TLS, single-lane, completion queue unit은 통과. STREAM의 순간 POLLOUT 상태를 고정해 기대하던 테스트와 admission 전 REQUEST timeout을 기대하던 capacity 테스트를 각각 level/advisory POLLOUT 및 admission 후 timeout 계약에 맞게 보정.
