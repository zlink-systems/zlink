# PR #4415 Node L2/L3 A/B 측정

조건: warmup 10초, active 5초, source CPU 0–5 / target CPU 8–13, 모든 thread SCHED_OTHER. A=`6350275facc547759c9e143cc2515f3f528c776e`, B=`afd3a0cea4eb732c9130a131f449bc7806600448`. 각 값은 counted raw 결과를 가리킨다.

| 셀 | A-L2 msg/s | B-L2 msg/s | A-L3 msg/s | B-L3 msg/s | L3 손실 A / B | B-L3 ÷ A-L3 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---|
| send-saturation-1K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |
| send-saturation-4K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |
| request-serial-1K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |
| request-serial-4K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |
| request-backpressure-1K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |
| request-backpressure-4K | 측정 대기 | 측정 대기 | 측정 대기 | 측정 대기 | — | — | — |

## Build 지문

| Worktree | HEAD | `git diff --stat` | service-wire-m6a-codec.js | raw-service-mesh-runtime.js | node-raw-binding-port.js |
|---|---|---|---|---|---|
| A | `6350275facc547759c9e143cc2515f3f528c776e` | bench patch 6 files, +756/−68 | `673b86196575f387af08aaf80c7c839da94cb1c77e0a262182bbeebd5318e0df` | `ea30c2646a56f24db6f2dffb2e3119b712fb3d4f24476178bfb486fcf097ad02` | `8ba92e1cc4dad85faccdd9757e0a255b52312cb08ff6818061d86b755ee3b73a` |
| B | `afd3a0cea4eb732c9130a131f449bc7806600448` | bench patch 6 files, +756/−68 | `5fe71e54ce0335c61c12143b1ec832285f0d327c1b3a6b13f31803fa8cddf764` | `8965084d01ff51413c5a3a2bf116018dd60914a061a0fbc5d3be649b90e517b7` | `5abcca5f5bde2dedf99f11c4c1eb9464bfff9775b99c1d2e7a01d46348acfdaa` |

A/B dist contained 803 files each; 15 generated output paths differed (including build metadata), reflecting the PR's 9 changed framework source files. Both `npm ci` and production `npm run build` completed with rc=0. All three listed outputs were newer than framework sources.

## Affinity, scheduling, raw

Per counted run: result JSON and `scheduling.txt` links will be listed with the cell values. Raw files, including warm and invalid runs, are retained under `raw/`.

## 진행 상태

측정 시작 대기: 다른 job의 build/test 및 shared measure lock 때문에 첫 exclusive hold를 아직 획득하지 못함.
