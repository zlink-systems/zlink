# Node ladder 측정 — fix branches

조건: L2→L3→L4, 직전 단계 대비; 6개 셀 × 1회, 10초 warmup·5초 측정, C2/C3 off, SCHED_OTHER, source 0–5 / target 8–13. 허용 손실 7%; 5–9% 셀은 양 단계 2회 추가해 중앙값 판정.

## 빌드 지문

- HEAD `50f84349c69503082774760b05834e513bdae785`, branch `exp/4350-node-fixes`, worktree clean; merge-base `6350275facc547759c9e143cc2515f3f528c776e`.
- 빌드 성공: Node HTTP client local package, `npm ci`, cached binding selection, `npm run build`; production TS 출력 424개 최신.
- `zlink.node` SHA-256 `ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05`; Framework `channel-dispatchers.js` SHA-256 `2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f`.

## 단계 결과

측정 전.

Raw 및 thread별 affinity/scheduling 증거는 각 수치 링크에 연결한다. 진행 기록: [progress.md](progress.md), 호출 형태와 메시지 비용: [call-shape.md](call-shape.md).
