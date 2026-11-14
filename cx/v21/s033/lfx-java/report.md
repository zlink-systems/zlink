# Java fix branch ladder — 부분 결과 (2026-11-14 UTC)

조건: source 0–5 / target 8–13, active 5초, warmup 10초, `SCHED_OTHER`, C2/C3 off. 6셀 전부의 이전 단계·신규 단계 비교를 끝내지 못해 L2/L3/L4 어느 단계도 판정하지 않았다.

## L2 vs L1

| 셀 | L1 msg/s | L2 msg/s | 손실 | 판정 | raw |
|---|---:|---:|---:|---|---|
| send-saturation 1K | [453,906.4](raw/attempt2/L1/send-saturation-1024/counted-1/1/run1/zlink-framework-java-send-saturation-1024/results.json) | 중단 (카운트 결과 없음) | — | 미판정 | [L1 scheduling](raw/attempt2/L1/send-saturation-1024/counted-1/1/run1/zlink-framework-java-send-saturation-1024/scheduling.txt) · [L2 부분 raw](raw/attempt2/L2/send-saturation-1024/counted-1/1/) |
| send-saturation 4K | — | — | — | 미측정 | — |
| request-serial 1K | — | — | — | 미측정 | — |
| request-serial 4K | — | — | — | 미측정 | — |
| request-backpressure 1K | — | — | — | 미측정 | — |
| request-backpressure 4K | — | — | — | 미측정 | — |

L1 send-saturation 1K는 `errors=0`, `submitted=completed=received=2,269,532`였다. 시작·중간의 source 37/41개, target 34/34개 thread가 모두 `SCHED_OTHER`였고 affinity는 각각 0–5 / 8–13이었다. 시작 시 CPU 0·8의 C2/C3 disable 값은 모두 1이었다. 해당 raw는 위 링크에 있다. 이 run을 포함한 첫 hold가 10분 32초로 상한을 넘겨, step verdict에는 사용하지 않았다. L2 1K warm raw는 [결과](raw/attempt2/L2/send-saturation-1024/warm/1/run1/zlink-framework-java-send-saturation-1024/results.json)로 보존했으나 counted run은 hold 상한에서 중단했다.

L2 이후 단계는 실행하지 않았다. 다음 시도는 빌드 대기 후 lock을 요청했으나 다른 worktree의 Gradle test가 shared measure.lock을 23분 넘게 보유했고, exclusive 요청은 대기열에 남았다. 80분 작업 상한에 따라 대기 요청을 종료했다. 별도 preflight에서는 guard가 C2/C3를 끄지 않아 측정을 시작하지 않고 raw에 오류를 보존했다.

## 빌드 지문

- HEAD `52c1761ab06430817da639395aca6ba7650b0fa0`, branch `exp/4350-java-fixes`, clean, merge-base `6350275facc547759c9e143cc2515f3f528c776e`.
- Client JAR SHA-256 `f0a0177bd21d0dc5dcc0a86d181fdbca5ac4de7b240dd160995dcad87dfce595`; server JAR `c90dc0c083b396e91221824e67acbb60421bc72eef50c012b67f7fef8df84026`.
- Client launcher `9b254974a9e0f0c4b2f98966feb7bf883ce78a6d479d48406dd08ef97107626e`; server launcher `1c400b230fb8504743de97bcb37e0ee245c39540a3ceff7a1087bda8f530bd86`.
- Framework JAR SHA-256 — core `2d88eb79bc9b4a4f2c7327e060a2b3e77bf9d51b81243f4501d5b0d6928b3841`; protobuf codec `b2bfb6cee1448a66c3ad5b3e3020124c41869011d274a5d9d45cba4b4ae000d1`; Spring starter `1eabaa7b8e588a3a78e362cb5a0119393d6abd5c5ad7ec3b43e53e3878003dbc`; JSON internal `f2b8311a640080bf1babd373e3792825d057d8c5050d33cc90acb99fc9aac134`; HTTP client `b623370cad1c8dbe2675c9e2e4ec3dc2cb6ad56a527290360c7448f6c047c964`.
- Java binding package `zlink-1.25.0.jar` SHA-256 `db545f3d834592d39b5a80b4ed10b724555511a00bc248c4cfcaa3e62e6cd530`; bench build and Framework composite source build succeeded. App and Framework JARs were newer than their Java sources.
