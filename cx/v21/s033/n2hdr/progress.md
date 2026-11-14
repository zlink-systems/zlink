
- build/검증 fingerprint: before-L1 7ff6e1ede8b13aca46cf9843ca7ca77f3b25fad6 dirty=False merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-L1-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile before-L1 L1 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.

- build/검증 fingerprint: before-L2 7ff6e1ede8b13aca46cf9843ca7ca77f3b25fad6 dirty=False merge-base=df42a4df4098d543be8dc0b84510cf6d961576d8; raw/before-L2-fingerprint.json (source/output fresh, own worktree paths·sha256).

- profile before-L2 L2 request-backpressure 1024: binding recv zlink-raw-server/main.js:47-53,64-81 ↔ ladder zlink-framework-server/main.js:166-205 (same DontWait drain/metrics; L1 permit). reply raw :82-86 ↔ ladder :206-212/shared/raw-wire.js:114-116 (new protobuf/body, L2 header only). Wait/atomics/body copies unchanged; header allocations report.md. CPU 6-7,14-15, ≤19s/child, no measure.lock.
