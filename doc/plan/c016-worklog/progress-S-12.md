# S-12 진행

완료. 결과: stress 5/50 → 0/50, until-fail:20 통과.
근본 원인: lost wake 아님. poller readiness 샘플의 public-API in-flight 토큰 때문에
`begin_close_or_fail_busy()`가 EBUSY로 조기 실패 → closing bit/POLLERR 미발행.
수정: 해당 gate가 기존 `public_api_sync_backoff` 스케줄로 유한 대기 후 EBUSY.
보고서: doc/plan/c016-worklog/core-rf-S-12-summary.md
