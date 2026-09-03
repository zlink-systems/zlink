# 0.16.0 캠페인 작업 기록 (c016)

이 폴더는 `doc/plan/core-send-dontwait-completion-0.16.0-plan.ko.md` 실행 중 감독관이 남긴 기록의 저장소 사본이다.
원본은 작업 머신의 `~/project/zlink-work/c016/`이며, 다른 머신에서 재개할 때는 이 폴더를 그 경로로 복사한다.

- `decisions.ko.md` — 감독 판정 D-021~ (재개 시 가장 먼저 읽는다)
- `perf-diagnosis.ko.md` — Phase 5 성능 회귀 진단(3자 bisect·callgrind)
- `briefs/*.prompt` — codex job 브리프 원문(재투입 시 그대로 사용)
- `tools/sweep.sh`(1024 단일 size), `tools/sweep2.sh`(4 size + 집계 판정), `tools/phase7-smoke.sh` — 드라이버.
  경로는 `${ZLINK_WORK}/c016`로 치환돼 있으니 `export ZLINK_WORK=~/project/zlink-work` 후 실행한다.
- `*-summary.md`, `spec-audit-*.md`, `phase8-inventory.md` — job 산출 요약
