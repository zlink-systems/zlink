# PR #4415 Node L2/L3 A/B 사전 병합 측정

조건: C2·C3 off, SCHED_OTHER, source CPU 0–5 / target CPU 8–13. 셀마다 A-L2 → B-L2 → A-L3 → B-L3를 같은 exclusive hold에서 측정한다. warmup 1초, active 5초, 실행 간격 3초, discarded warm run 없음. 메시지 오류가 0이고 submitted=completed=received인 실행만 판정에 쓴다.

## 빌드 지문

| worktree | HEAD | merge-base | `git diff --stat` (bench only) |
|---|---|---|---|
| A baseline | `6350275facc547759c9e143cc2515f3f528c776e` | 동일 | 6 tracked files, 756 insertions / 68 deletions; 추가 untracked bench file: `framework/bench/grpc/node/zlink-framework-server/bench-handlers.js` |
| B PR | `ec6aa8ea5c4e6f59f8b8fc494febc8d928b9dba8` | `6350275facc547759c9e143cc2515f3f528c776e` | 6 tracked files, 756 insertions / 68 deletions; 추가 untracked bench file: `framework/bench/grpc/node/zlink-framework-server/bench-handlers.js` |

두 빌드 모두 종료 코드 0. 각자 worktree에서 Core binding 1.25.0 tarball로 local HTTP client 패키지를 만들고 `framework/languages/node` 및 `framework/bench/grpc/node/build.sh`를 빌드했다. 대상 dist 파일은 각 대응 `.ts` source보다 최신이다.

| dist 파일 | A SHA256 | B SHA256 |
|---|---|---|
| `service-wire-m6a-codec.js` | `673b86196575f387af08aaf80c7c839da94cb1c77e0a262182bbeebd5318e0df` | `5fe71e54ce0335c61c12143b1ec832285f0d327c1b3a6b13f31803fa8cddf764` |
| `raw-service-mesh-runtime.js` | `ea30c2646a56f24db6f2dffb2e3119b712fb3d4f24476178bfb486fcf097ad02` | `8965084d01ff51413c5a3a2bf116018dd60914a061a0fbc5d3be649b90e517b7` |
| `node-raw-binding-port.js` | `8ba92e1cc4dad85faccdd9757e0a255b52312cb08ff6818061d86b755ee3b73a` | `5abcca5f5bde2dedf99f11c4c1eb9464bfff9775b99c1d2e7a01d46348acfdaa` |
| Core addon `zlink.node` | `ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05` | 동일 |

A/B framework `dist`는 각 1,059개 파일이다. 위 세 JS 외 차이: `node-backend-adapter-support.{d.ts,js}`, `node-raw-binding-port.d.ts`, `node-raw-mesh-backend.js`, `raw-binding-port.d.ts`, `durable-service-request.{d.ts,js}`, `raw-service-mesh-runtime.d.ts`, `service-mailbox.d.ts`, `service-stateful-runtime.js`, `service-wire-m6a-codec.d.ts`, `host/service-relocation-host-runtime.js`. 빌드 메타데이터 차이: `framework`, `framework-locations-redis`, `nestjs`의 `.tsbuildinfo`.

## 처리량과 단계 손실

| 셀 | A-L2 msg/s | B-L2 msg/s | A-L3 msg/s | B-L3 msg/s | A L3 손실 vs L2 | B L3 손실 vs L2 | B-L3 / A-L3 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| send-saturation-1024 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |
| send-saturation-4096 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |
| request-serial-1024 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |
| request-serial-4096 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |
| request-backpressure-1024 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |
| request-backpressure-4096 | 측정 전 | 측정 전 | 측정 전 | 측정 전 | — | — | — | — |

각 수치는 해당 raw `results.json`에 연결한다. 판정 경계: B-L3/A-L3가 ±3% 이내면 neutral, 그보다 크면 PR helps, 작으면 PR hurts. 3–9% 차이 셀은 A-L3/B-L3 추가 2쌍의 중앙값을 사용한다.

## 측정 근거

각 counted run raw 폴더의 `scheduling.txt`에 source·target 모든 thread의 시작 및 mid-active SCHED policy와 `Cpus_allowed_list`를 보존한다. raw 파일은 삭제하지 않는다.
