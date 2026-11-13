# Node L5~L7 diagnosis progress

- 2026-11-14 21:25:13Z no-switch protobuf control : submitted=138013, completed=138013, received=0, errors=0; invalid count mismatch, raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/control/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json; scheduling=/root/jobs/fx/cx/v21/s033/node-l57g/raw/control/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/scheduling.txt.
- Build: HEAD=7d1afa98a9f53c952daf0fd3437691b0c5b123ce dirty=clean merge_base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970;addon=framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05;framework_artifact=framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f;; call-shape=call-shape.md.
- 2026-11-14 control `request-serial-1024` without L5_7 switches: target started; source stats endpoint did not become ready after route-ready probe; no result was produced. Raw target/source logs: `/root/jobs/fx/cx/v21/s033/node-l57g/raw/control/request-serial-1024/run1/zlink-framework-node-request-serial-1024/`.
- Target message-flow diagnostic, L5_7 send: arrived at `BenchPayload` send handler, then `handler_exception`; `TypeError: Cannot read properties of undefined (reading 'record')`. Raw target log: `/root/jobs/fx/cx/v21/s033/node-l57g/raw/diagnostic-logged/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/target.log`.
- Root cause: `providers.ts:562-604` builds handler instances from constructor dependency metadata; bench JS handlers had no parameter injection token, while their `useFactory` providers were bypassed by the Framework scope activator. Fixed bench to register and inject a metrics token.
- npm test: 22 passed, 0 failed.
- Post-fix control send-saturation 1024: submitted=completed=received=217543, errors=0; raw=`/root/jobs/fx/cx/v21/s033/node-l57g/raw/control-after-fix/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json`.
- Post-fix control request-serial 1024: submitted=completed=received=21309, errors=0; raw=`/root/jobs/fx/cx/v21/s033/node-l57g/raw/control-after-fix/request-serial-1024/run1/zlink-framework-node-request-serial-1024/results.json`.
- L5_7 final smoke send-saturation 1024 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/send-saturation-1024
- L5_7 final smoke send-saturation 1024 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/send-saturation-1024
- L5_7 final smoke send-saturation 4096 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/send-saturation-4096
- L5_7 final smoke send-saturation 4096 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/send-saturation-4096
- L5_7 final smoke request-serial 1024 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/request-serial-1024
- L5_7 final smoke request-serial 1024 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/request-serial-1024
- L5_7 final smoke request-serial 4096 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/request-serial-4096
- L5_7 final smoke request-serial 4096 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/request-serial-4096
- L5_7 final smoke request-backpressure 1024 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/request-backpressure-1024
- L5_7 final smoke request-backpressure 1024 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/request-backpressure-1024
- L5_7 final smoke request-backpressure 4096 phase=warm raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/warm/request-backpressure-4096
- L5_7 final smoke request-backpressure 4096 phase=counted raw=/root/jobs/fx/cx/v21/s033/node-l57g/raw/final-smoke/counted/request-backpressure-4096
addon=framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework_artifact=framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
main_js=framework/bench/grpc/node/zlink-framework-server/main.js sha256=cecdde7fcd291efbaac89a177bafec61acee3bd401a082d430e197d9c400b483
handlers_js=framework/bench/grpc/node/zlink-framework-server/bench-handlers.js sha256=26f1595eb7ce96d00def311d1f9c17fbf3efbb844222a37d259bed525c625ff6

- Final smoke pre-commit snapshot: results metadata HEAD=7d1afa98a9f53c952daf0fd3437691b0c5b123ce, dirty=dirty; runner used live JavaScript files from this worktree. Artifact hashes are recorded above.
- Local commit: c019495a82c40247f0d3e5d9a16b5c6673b11495
