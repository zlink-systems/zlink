# Java fix branch ladder — 2026-11-14 UTC

측정 조건: source CPU 0–5, target CPU 8–13, warmup 10초, active 5초; `SCHED_OTHER`, C2·C3 off. 각 step은 직전 단계와 동일 hold 안에서 6 셀을 비교한다. 손실 `1 − new/previous`, 기준 7%; 경계 5–9%면 3 paired runs 중앙값.

[호출 비교](call-shape.md) · [측정/affinity/build 기록](progress.md)

## Build fingerprints

| Artifact | SHA-256 |
|---|---|
| client.jar | `f0a0177bd21d0dc5dcc0a86d181fdbca5ac4de7b240dd160995dcad87dfce595` |
| zlink-framework-server.jar | `c90dc0c083b396e91221824e67acbb60421bc72eef50c012b67f7fef8df84026` |
| client launcher | `9b254974a9e0f0c4b2f98966feb7bf883ce78a6d479d48406dd08ef97107626e` |
| server launcher | `1c400b230fb8504743de97bcb37e0ee245c39540a3ceff7a1087bda8f530bd86` |

## Framework production JAR SHA-256

- Core: `2d88eb79bc9b4a4f2c7327e060a2b3e77bf9d51b81243f4501d5b0d6928b3841`
- Protobuf codec: `b2bfb6cee1448a66c3ad5b3e3020124c41869011d274a5d9d45cba4b4ae000d1`
- Spring Boot starter: `1eabaa7b8e588a3a78e362cb5a0119393d6abd5c5ad7ec3b43e53e3878003dbc`
- JSON internal: `f2b8311a640080bf1babd373e3792825d057d8c5050d33cc90acb99fc9aac134`
- HTTP client: `b623370cad1c8dbe2675c9e2e4ec3dc2cb6ad56a527290360c7448f6c047c964`

HEAD `52c1761ab06430817da639395aca6ba7650b0fa0`; merge-base `6350275facc547759c9e143cc2515f3f528c776e`; worktree clean after builds. Java binding package `zlink-1.25.0.jar` SHA-256 `db545f3d834592d39b5a80b4ed10b724555511a00bc248c4cfcaa3e62e6cd530`.

## Step results

측정 진행 중.
