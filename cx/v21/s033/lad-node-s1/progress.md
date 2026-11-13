# Progress — Node ladder current-condition quick measurement

- No measurement started yet. Current worktree: `HEAD=7d1afa98a9f53c952daf0fd3437691b0c5b123ce`, dirty=clean; merge-base=`494c5f4827db8d86a5dce716b7a8da6e7e9cf970`.
- Mandatory call shape: `call-shape.md`; binding and L-step receive both use `DontWait` and treat `NoData` as idle (`zlink-raw-server/main.js:47-53,64-75`; `zlink-framework-server/main.js:142-155`).
- Existing binding reference `binding-daily/node-2026-11-13.json` is not reusable: conditions say guard v7 and affinity is explicitly unverified. It will be remeasured under v8 if measurement proceeds.
- Measurement runner copy: `tools/node/`; runner-owned CPU pinning will be 0-5 source and 8-13 target, with per-thread SCHED_OTHER/affinity snapshots at start and mid-active.
