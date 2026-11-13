# Node direct JSON writer 조사

- Primary: perf/4354-node-json-writer, f723033a5a524af5dcfa4c04fc42677596c5f429, clean. 사용자 지정 branch 확인. internal-docs.sh, GitHub, sub-agent, measure.lock 사용 없음.
- 공통 규칙과 n4json/n4json2 보고서 읽음. 사용자 최신 조건이 측정 lock/CPU 조건보다 우선한다. 모든 build/test/profile은 CPU 6-7,14-15. Profile process lifetime ≤20초, shape 비교만 수행한다.
- Scratch: /root/worktree/zlink-v21-s033/n4w-scratch, ae8a5178b5 archive + primary packages. 다른 worktree/output 사용 없음. local http-client 누락으로 최초 materialize 실패(raw/materialize-http.log), 이 scratch에서 registry binding과 bootstrap compiler로 package를 생성한다. Raw 삭제 없음.
- 후보: single walk, native JSON.stringify 없음, fallback 없음. JSON.stringify의 toJSON → 원본 재조회/사용자 replacer → 정책 → boxed primitive → 직렬화 순서를 그대로 구현한다. Buffer.toJSON 부작용까지 보존하므로 기본 Buffer 숫자 배열은 이 후보에도 남는다. 이를 생략할 때 필요한 예외 판정은 별도로 규칙 수에 포함한다.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before-warm-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result before-warm-L4-send-saturation-4096: source={'ready': True, 'phase': 'idle', 'submitted': 30200, 'completed': 30200, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 8, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 30200, 'received': 30200, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 30200, 'allPhaseMessages': 30200, 'errors': 0, 'meanMicros': 479.4990079801325, 'p50Micros': 239.12, 'p95Micros': 1700.57, 'p99Micros': 4520.489, 'cpuSeconds': 5.430274, 'workingSetMb': 109.03515625}; raw=raw/before-warm-L4-send-saturation-4096.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before-warm-L4-request-serial-1024: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result before-warm-L4-request-serial-1024: source={'ready': True, 'phase': 'idle', 'submitted': 7878, 'completed': 7878, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 1, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 7878, 'received': 7878, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 7878, 'allPhaseMessages': 7878, 'errors': 0, 'meanMicros': 237.4277947448591, 'p50Micros': 208.263, 'p95Micros': 345.005, 'p99Micros': 746.314, 'cpuSeconds': 4.910963, 'workingSetMb': 110.08203125}; raw=raw/before-warm-L4-request-serial-1024.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result before-L4-send-saturation-4096: source={'ready': True, 'phase': 'idle', 'submitted': 32936, 'completed': 32936, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 8, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 32936, 'received': 32936, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 32936, 'allPhaseMessages': 32936, 'errors': 0, 'meanMicros': 423.8867836106388, 'p50Micros': 219.244, 'p95Micros': 1316.573, 'p99Micros': 3949.619, 'cpuSeconds': 5.435914, 'workingSetMb': 109.96875}; raw=raw/before-L4-send-saturation-4096.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before-L4-request-serial-1024: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result before-L4-request-serial-1024: source={'ready': True, 'phase': 'idle', 'submitted': 9224, 'completed': 9224, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 1, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 9224, 'received': 9224, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 9224, 'allPhaseMessages': 9224, 'errors': 0, 'meanMicros': 211.304855160451, 'p50Micros': 186.874, 'p95Micros': 280.606, 'p99Micros': 594.244, 'cpuSeconds': 4.918892, 'workingSetMb': 115.9453125}; raw=raw/before-L4-request-serial-1024.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile writer-warm-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result writer-warm-L4-send-saturation-4096: source={'ready': True, 'phase': 'idle', 'submitted': 31175, 'completed': 31175, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 8, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 31175, 'received': 31175, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 31175, 'allPhaseMessages': 31175, 'errors': 0, 'meanMicros': 497.2630044587009, 'p50Micros': 231.115, 'p95Micros': 1942.775, 'p99Micros': 3792.85, 'cpuSeconds': 5.10609, 'workingSetMb': 109.51171875}; raw=raw/writer-warm-L4-send-saturation-4096.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile writer-warm-L4-request-serial-1024: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result writer-warm-L4-request-serial-1024: source={'ready': True, 'phase': 'idle', 'submitted': 5641, 'completed': 5641, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 1, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 5641, 'received': 5641, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 5641, 'allPhaseMessages': 5641, 'errors': 0, 'meanMicros': 343.73887502215916, 'p50Micros': 274.305, 'p95Micros': 595.756, 'p99Micros': 1798.239, 'cpuSeconds': 4.68697, 'workingSetMb': 110.07421875}; raw=raw/writer-warm-L4-request-serial-1024.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile writer-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

Build/source integrity: [{"source": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts", "source_sha256": "bbb0ef9457795d3047db89237dceb93221dca56945161dd6fff04566086aaca7", "built": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js", "built_sha256": "eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9", "newer": true}, {"source": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/src/runtime/messaging/framework-json-v1.ts", "source_sha256": "b9f656d1a1831e1424ff494f1ed1b75e91bb1f9bf53b462ce5f2d9bcc9ab6ccd", "built": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/messaging/framework-json-v1.js", "built_sha256": "43a8fa4cc20b9b339a75f90d55a47e1e78167c229e7470cca3d5f4a5edc7cf3e", "newer": true}, {"source": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts", "source_sha256": "ba3e26811157d6613bd57d8859d5c15382619ac9b8bd5eb1194687af95f50899", "built": "/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js", "built_sha256": "7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25", "newer": true}]

First result writer-L4-send-saturation-4096: source={'ready': True, 'phase': 'idle', 'submitted': 41382, 'completed': 41382, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 8, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 41382, 'received': 41382, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 41382, 'allPhaseMessages': 41382, 'errors': 0, 'meanMicros': 285.69810651974285, 'p50Micros': 180.542, 'p95Micros': 533.322, 'p99Micros': 3379.97, 'cpuSeconds': 5.540207, 'workingSetMb': 112.921875}; raw=raw/writer-L4-send-saturation-4096.

{"scratch_archive": "ae8a5178b5", "primary_head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "primary_dirty": false, "primary_base": "6350275facc547759c9e143cc2515f3f528c776e", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile writer-L4-request-serial-1024: scratch archive ae8a5178b5, primary base f723033a5a; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4w-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=eb4a6b7a02a1c6fde491e879d9a6e3b653c68017e2a3486831a31871df0f2bc9
/root/worktree/zlink-v21-s033/n4w-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

First result writer-L4-request-serial-1024: source={'ready': True, 'phase': 'idle', 'submitted': 11302, 'completed': 11302, 'errors': 0, 'received': 0, 'inFlight': 0, 'currentInFlight': 0, 'peakInFlight': 1, 'failure': None}, target={'ready': True, 'phase': 'idle', 'submitted': 0, 'completed': 11302, 'received': 11302, 'currentInFlight': 0, 'inFlight': 0, 'peakInFlight': 0, 'activeMessages': 11302, 'allPhaseMessages': 11302, 'errors': 0, 'meanMicros': 168.0288483454256, 'p50Micros': 151.349, 'p95Micros': 229.282, 'p99Micros': 462.421, 'cpuSeconds': 4.837553, 'workingSetMb': 117.66796875}; raw=raw/writer-L4-request-serial-1024.

첫 micro 결과(raw/micro-summary.json): CPU us/message 0: 0.792→0.932, 1K: 12.179→10.480, 4K: 33.961→25.116, nested: 16.132→12.793. 전체 heap B/message 0:257→754, 1K:11225→10414, 4K:44010→39055, nested:13163→14068. `.length` 소비는 writer rope 평탄화 비용을 포함하지 않으므로 UTF-8 소비 형태도 확인한다.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "before", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "before", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "writer", "phase": "warm", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "writer", "phase": "counted", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "before", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "0", "kind": "writer", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "before", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "1024", "kind": "writer", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "before", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "4096", "kind": "writer", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "before", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "nested", "kind": "writer", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "before", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "before", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "writer", "phase": "repeat2", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.

Micro {"head": "f723033a5a524af5dcfa4c04fc42677596c5f429", "dirty": false, "base": "6350275facc547759c9e143cc2515f3f528c776e", "shape": "dto", "kind": "writer", "phase": "repeat3", "writer_sha256": "c6d8e8b5b14ca6505de88d19c8c0df64dce3955c0079e5d889d9254a2667cac4"}; call-shape.md, Buffer.toJSON + native Base64 + output string, no atomics/copies added.
