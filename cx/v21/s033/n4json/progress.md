# Node typed JSON 조사

- 시작 branch perf/4354-node-typed-json, HEAD/base df42a4df4098d543be8dc0b84510cf6d961576d8; tracked/untracked 변경 없음.
- Scratch: ae8a5178b5 git archive. 다른 job worktree/build output을 사용하지 않는다.
- 요청한 lad-node-impl4c/report.md 없음. lad-node-impl4/report.md 및 지정 commit에서 L4 production 호출을 확인했다.
- Node 22.23.3, 모든 명령 taskset -c 6-7,14-15. 성능 판정과 measure.lock 사용 없음.
- 최초 설치: local http-client tarball 없음. 이 worktree에서 registry binding 1.25.0을 내려받고 공식 http-client bootstrap publisher를 실행한다. bootstrap tsc는 quiet guard가 SIGSTOP한 상태다. 강제 재개하지 않는다.

- Primary baseline tsc -b tsconfig.build.json 성공. raw/build-primary-before-status.json.
- raw/before-counts-expanded-deterministic-counts.json: 4096-byte Buffer는 toJSON 1회/숫자 원소 4096개, fromCharCode 인자 4096개, btoa 1회. Replacer 2회/숫자 원소 callback 0회. production encode 내 Buffer.from byte-copy/UTF-8 변환 0회.
- Scratch npm ci 완료. tsc는 quiet guard가 정지/재개하며 진행 중이다. 기존 raw 파일을 삭제하지 않았다.

Profile before-L3-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

Profile before-L3-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

Profile before-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

Profile before-L4-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": false, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before2-L3-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": false, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before2-L3-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": false, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before2-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": false, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile before2-L4-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b940cc34b3b772c7da48723e942fa76eeca0842307d70b8419bf861eac2ae299
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

2026-11-13T21:31:25.898541+00:00 report.md 원인/수정안 최초 기록. runtime 수정 전. sha256=98d9a636dc69779bb11e99302b5bce46984e488ea206e2a6430b3b4c4e07f90d

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": true, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile after-L3-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b4fb99e9106425542874038f51b2cc941b07ed38f5504854e1f9f31e1b841f11
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": true, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile after-L3-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b4fb99e9106425542874038f51b2cc941b07ed38f5504854e1f9f31e1b841f11
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": true, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile after-L4-send-saturation-4096: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b4fb99e9106425542874038f51b2cc941b07ed38f5504854e1f9f31e1b841f11
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

{"scratch_archive": "ae8a5178b5", "primary_head": "df42a4df4098d543be8dc0b84510cf6d961576d8", "primary_dirty": true, "primary_base": "df42a4df4098d543be8dc0b84510cf6d961576d8", "archive_base": "494c5f4827db8d86a5dce716b7a8da6e7e9cf970", "preload_sha256": "532bea4002de826b048217c158d6dfc28dcefc1e02b587a1c64b6c45cb0f3311"}

Profile after-L4-request-serial-1024: scratch archive ae8a5178b5, primary base df42a4df40; CPUs 6-7,14-15, no measure.lock, shape only.
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/client/main.js: sha256=2904aee183638a7c31281e064fc7d69eb988e68d8ce6bba6fc88fe2b8e87a27b
/root/worktree/zlink-v21-s033/n4json-scratch/framework/bench/grpc/node/shared/raw-wire.js: sha256=f70f244cc8e6afff4c35308488c491f1d0b4f11512209791095f1506aebe06bc
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js: sha256=7d637984b3c1beb47c31479de5fdd488e530cd194d9be7548fc4b8ae7853dc25
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/packages/stream-wire/dist/internal/framework-json-v1.js: sha256=b4fb99e9106425542874038f51b2cc941b07ed38f5504854e1f9f31e1b841f11
/root/worktree/zlink-v21-s033/n4json-scratch/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node: sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
Receive/reply and waits/allocations/copies: see call-shape.md; L3/L4 add only restored features.

2026-11-13T22:05:09.371617+00:00 local commit 6e48bdf5fbfe7252b029b405f514693a91f2a92d; clean; tests 19+131, build/format/lint success; no push/PR.
