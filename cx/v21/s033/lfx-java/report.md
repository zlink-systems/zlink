# Java fix branch ladder — 2026-11-14 UTC

측정 조건: source CPU 0–5, target CPU 8–13, warmup 10초, active 5초; `SCHED_OTHER`, C2·C3 off. 각 step은 직전 단계와 동일 hold 안에서 6 셀을 비교한다. 손실 `1 − new/previous`, 기준 7%; 경계 5–9%면 3 paired runs 중앙값.

[호출 비교](call-shape.md) · [측정/affinity/build 기록](progress.md)

## Build fingerprints

| Artifact | SHA-256 |
|---|---|
| client.jar | 측정 전 기입 |
| zlink-framework-server.jar | 측정 전 기입 |
| client launcher | 측정 전 기입 |
| server launcher | 측정 전 기입 |

## Step results

미측정.
