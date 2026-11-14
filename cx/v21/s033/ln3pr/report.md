# PR #4415 Node L2/L3 A/B 측정

조건 설정: warmup 10초, active 5초, source CPU 0–5 / target CPU 8–13. 단, exclusive measure lock을 얻지 못해 실제 측정 run은 시작하지 않았다. 모든 처리량·손실·비율은 미측정이며 PR 판정은 불가하다.

| 셀 | A-L2 msg/s | B-L2 msg/s | A-L3 msg/s | B-L3 msg/s | L3 손실 A / B | B-L3 ÷ A-L3 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---|
| send-saturation-1K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |
| send-saturation-4K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |
| request-serial-1K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |
| request-serial-4K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |
| request-backpressure-1K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |
| request-backpressure-4K | 미측정 | 미측정 | 미측정 | 미측정 | — | — | 판정 불가 |

Raw 및 affinity/scheduling 기록: 측정 run이 없어 생성되지 않았다. warm/count 파일도 없다. C2·C3 off, SCHED_OTHER 여부와 per-thread affinity를 측정 증거로 확인하지 못했다.

## Build 지문

| Worktree | HEAD | `git diff --stat` | service-wire-m6a-codec.js | raw-service-mesh-runtime.js | node-raw-binding-port.js |
|---|---|---|---|---|---|
| A | `6350275facc547759c9e143cc2515f3f528c776e` | bench 6 files, +756/−68 | `673b86196575f387af08aaf80c7c839da94cb1c77e0a262182bbeebd5318e0df` | `ea30c2646a56f24db6f2dffb2e3119b712fb3d4f24476178bfb486fcf097ad02` | `8ba92e1cc4dad85faccdd9757e0a255b52312cb08ff6818061d86b755ee3b73a` |
| B | `afd3a0cea4eb732c9130a131f449bc7806600448` | bench 6 files, +756/−68 | `5fe71e54ce0335c61c12143b1ec832285f0d327c1b3a6b13f31803fa8cddf764` | `8965084d01ff51413c5a3a2bf116018dd60914a061a0fbc5d3be649b90e517b7` | `5abcca5f5bde2dedf99f11c4c1eb9464bfff9775b99c1d2e7a01d46348acfdaa` |

두 worktree 모두 `npm ci`와 production `npm run build`가 rc=0으로 완료됐고, 산출물은 해당 worktree 안에서 빌드했다. 지정한 세 JS 파일은 각 worktree의 framework source보다 새로 빌드됐으며 A/B 간 모두 달랐다. 각 framework dist는 803파일이고 전체 비교에서 15개 생성 산출물이 달랐다(빌드 metadata 포함). 따라서 차이는 지정한 세 파일에만 한정되지 않았다. PR은 framework source 9개 파일을 변경한다.

## 미완료 사유

다른 job의 build/test와 shared measure lock이 계속되어 10분 build 대기 후 nonblocking exclusive 시도들이 거절됐다. 1.5시간 제한에 도달해 측정을 시작하지 않고 종료했다. 원시 파일은 삭제하지 않았다.
