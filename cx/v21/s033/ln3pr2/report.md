# PR #4415 Node L2/L3 A/B 사전 병합 측정

조건: C2·C3 off, SCHED_OTHER, source CPU 0–5 / target CPU 8–13. 셀마다 A-L2 → B-L2 → A-L3 → B-L3를 같은 exclusive hold에서 측정한다. warmup 1초, active 5초, 실행 간격 3초, discarded warm run 없음. 메시지 오류가 0이고 submitted=completed=received인 실행만 판정에 쓴다.

## 빌드 지문

| worktree | HEAD | merge-base | `git diff --stat` (bench only) |
|---|---|---|---|
| A baseline | `6350275facc547759c9e143cc2515f3f528c776e` | 동일 | 6 tracked files, 756 insertions / 68 deletions; 추가 untracked bench file: `framework/bench/grpc/node/zlink-framework-server/bench-handlers.js` |
| B PR | `ec6aa8ea5c4e6f59f8b8fc494febc8d928b9dba8` | `6350275facc547759c9e143cc2515f3f528c776e` | 6 tracked files, 756 insertions / 68 deletions; 추가 untracked bench file: `framework/bench/grpc/node/zlink-framework-server/bench-handlers.js` |

두 worktree의 bench-only `git diff --stat`은 동일하다: `client/framework-transport.js` 33, `client/main.js` 96, `run_local.sh` 14, `shared/raw-wire.js` 220, `shared/raw-wire.test.js` 94, `zlink-framework-server/main.js` 367 lines changed. `bench-handlers.js`는 untracked bench 파일이다.

두 빌드 모두 종료 코드 0. 각자 worktree에서 Core binding 1.25.0 tarball로 local HTTP client 패키지를 만들고 `framework/languages/node` 및 `framework/bench/grpc/node/build.sh`를 빌드했다. 대상 dist 파일은 각 대응 `.ts` source보다 최신이다.

| dist 파일 | A SHA256 | B SHA256 |
|---|---|---|
| `service-wire-m6a-codec.js` | `673b86196575f387af08aaf80c7c839da94cb1c77e0a262182bbeebd5318e0df` | `5fe71e54ce0335c61c12143b1ec832285f0d327c1b3a6b13f31803fa8cddf764` |
| `raw-service-mesh-runtime.js` | `ea30c2646a56f24db6f2dffb2e3119b712fb3d4f24476178bfb486fcf097ad02` | `8965084d01ff51413c5a3a2bf116018dd60914a061a0fbc5d3be649b90e517b7` |
| `node-raw-binding-port.js` | `8ba92e1cc4dad85faccdd9757e0a255b52312cb08ff6818061d86b755ee3b73a` | `5abcca5f5bde2dedf99f11c4c1eb9464bfff9775b99c1d2e7a01d46348acfdaa` |
| Core addon `zlink.node` | `ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05` | 동일 |

A/B framework `dist`는 각 1,059개 파일이다. 위 세 JS 외 차이: `node-backend-adapter-support.{d.ts,js}`, `node-raw-binding-port.d.ts`, `node-raw-mesh-backend.js`, `raw-binding-port.d.ts`, `durable-service-request.{d.ts,js}`, `raw-service-mesh-runtime.d.ts`, `service-mailbox.d.ts`, `service-stateful-runtime.js`, `service-wire-m6a-codec.d.ts`, `host/service-relocation-host-runtime.js`. 빌드 메타데이터 차이: `framework`, `framework-locations-redis`, `nestjs`의 `.tsbuildinfo`.

## 처리량과 단계 손실

처리량 링크는 raw `results.json`이며, 같은 디렉터리의 `scheduling.txt`에 thread별 affinity·정책 증거가 있다. 3회 셀은 각 L3 열의 값이 중앙값이고 그 중앙값을 만든 raw 실행에 연결했다.

| 셀 | A-L2 msg/s | B-L2 msg/s | A-L3 msg/s | B-L3 msg/s | A 손실 vs L2 | B 손실 vs L2 | B-L3 / A-L3 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| send-saturation-1024 | [122,918.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/A-L2/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/results.json>) | [119,359.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/B-L2/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/results.json>) | [82,520.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/A-L3/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/results.json>) | [74,331.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/B-L3/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/results.json>) | 32.87% | 37.73% | -9.92% | PR hurts |
| send-saturation-4096 | [82,036.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-4096/A-L2/counted/attempt1/run1/zlink-framework-node-send-saturation-4096/results.json>) | [83,329.2](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-4096/B-L2/counted/attempt1/run1/zlink-framework-node-send-saturation-4096/results.json>) | [54,115.2](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-4096/A-L3/counted/attempt2/run1/zlink-framework-node-send-saturation-4096/results.json>) | [52,988.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-4096/B-L3/counted/attempt1/run1/zlink-framework-node-send-saturation-4096/results.json>) | 34.04% | 36.41% | -2.08% | neutral |
| request-serial-1024 | [7,525.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-1024/A-L2/counted/attempt1/run1/zlink-framework-node-request-serial-1024/results.json>) | [7,532.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-1024/B-L2/counted/attempt1/run1/zlink-framework-node-request-serial-1024/results.json>) | [6,567.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-1024/A-L3/counted/attempt2/run1/zlink-framework-node-request-serial-1024/results.json>) | [6,478.2](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-1024/B-L3/counted/attempt1/run1/zlink-framework-node-request-serial-1024/results.json>) | 12.73% | 13.99% | -1.36% | neutral |
| request-serial-4096 | [6,738.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-4096/A-L2/counted/attempt1/run1/zlink-framework-node-request-serial-4096/results.json>) | [6,970.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-4096/B-L2/counted/attempt1/run1/zlink-framework-node-request-serial-4096/results.json>) | [5,753.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-4096/A-L3/counted/attempt1/run1/zlink-framework-node-request-serial-4096/results.json>) | [5,837.6](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-serial-4096/B-L3/counted/attempt1/run1/zlink-framework-node-request-serial-4096/results.json>) | 14.63% | 16.25% | +1.47% | neutral |
| request-backpressure-1024 | [20,167.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-1024/A-L2/counted/attempt1/run1/zlink-framework-node-request-backpressure-1024/results.json>) | [20,400.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-1024/B-L2/counted/attempt1/run1/zlink-framework-node-request-backpressure-1024/results.json>) | [16,491.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-1024/A-L3/counted/attempt1/run1/zlink-framework-node-request-backpressure-1024/results.json>) | [16,476.0](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-1024/B-L3/counted/attempt1/run1/zlink-framework-node-request-backpressure-1024/results.json>) | 18.23% | 19.24% | -0.10% | neutral |
| request-backpressure-4096 | [18,763.6](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-4096/A-L2/counted/attempt1/run1/zlink-framework-node-request-backpressure-4096/results.json>) | [18,298.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-4096/B-L2/counted/attempt1/run1/zlink-framework-node-request-backpressure-4096/results.json>) | [13,266.8](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-4096/A-L3/counted/attempt1/run1/zlink-framework-node-request-backpressure-4096/results.json>) | [13,491.4](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/request-backpressure-4096/B-L3/counted/attempt1/run1/zlink-framework-node-request-backpressure-4096/results.json>) | 29.30% | 26.27% | +1.69% | neutral |

3–9% 경계로 3회 측정한 셀은 `send-saturation-4096`과 `request-serial-1024`이며, 각 A-L3/B-L3 `attempt1`–`attempt3` raw를 보존하고 중앙값으로 판정했다. 그 밖의 셀은 1회다.

## 측정 근거

총 32 counted run 모두 errors=0 및 submitted=completed=received였다. 모든 run의 `scheduling.txt`에서 시작·mid-active source/target thread 정책이 SCHED_OTHER, affinity가 각각 0–5 / 8–13임을 확인했고, C2/C3 비활성 기록도 확인했다. 각 스케줄 기록에는 source thread 13–14개, target thread 13개가 두 시점에 기록돼 있다. 예: [send-saturation-1K A-L2 scheduling](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/A-L2/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/scheduling.txt>), [send-saturation-1K B-L3 scheduling](</root/jobs/fx/cx/v21/s033/ln3pr2/raw/send-saturation-1024/B-L3/counted/attempt1/run1/zlink-framework-node-send-saturation-1024/scheduling.txt>). 나머지 각 run의 `scheduling.txt`도 해당 `results.json`과 같은 폴더에 있다. warmup 1초, active 5초, 실행 간 3초, discarded warm run 없음. 측정은 한 exclusive hold(375초)에서 셀별 A-L2 → B-L2 → A-L3 → B-L3 순서로 실행했다.
