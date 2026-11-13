# Node Framework request path, issue #3872

## Result

The requested >=0.65 four-round-median framework/binding ratio was not reached. I did not retain a runtime change: the only measured payload-copy candidate regressed three cells and was effectively flat on send-saturation. The final commit contains a contract-test strengthening only.

## Changes and measured effect

- `framework/languages/node/test/contract/channel-envelope-error.test.js`: assert that mutating the first `data()` result leaves a second `data()` result unchanged. This pins independent ownership across separate calls; it has no runtime performance effect.
- Experimented with implementing `data()` using `ArrayBuffer.slice` and a new `Uint8Array`. This preserved the `Uint8Array` return type and passed the focused contracts, but the measured throughput was worse on send-saturation 4096 (-0.9%), request-serial 4096 (-3.4%), and request-serial 1024 (-4.0%). Request-backpressure 1024 improved +1.6%. The candidate implementation was reverted, as it did not provide a net improvement.
- Temporary CPU pinning in `framework/bench/grpc/node/run_local.sh` was reverted before commit.

## A/B framework measurements

Throughput in messages/s. Each array lists the four run results in order; medians are the middle-pair average. “Candidate” is the experimental copy implementation, which is not in the committed tree. `ratio040` is the prebuilt main reference. Each cell used `WARMUP_SECONDS=30`, `DURATION_SECONDS=5`, four rounds, framework only.

| Cell | Baseline 3872 runs (median) | Candidate 3872 runs (median) | Baseline ratio040 runs (median) | Candidate-time ratio040 runs (median) | Same-day binding runs (median) | Final candidate ratio |
|---|---:|---:|---:|---:|---:|---:|
| send-saturation 4096 | 52343, 57271.6, 56174.2, 58346.8 (56722.9) | 56678.2, 55660.4, 55731.6, 57425.2 (56204.9) | 56794.8, 56908.6, 54302.8, 56386.2 (56590.5) | 55976.2, 57398, 54711.4, 55215 (55595.6) | 90258.6, 91044.6 (90651.6) | 0.620 |
| request-serial 4096 | 4789.8, 4988.6, 4556.8, 4854.4 (4822.1) | 4777.2, 4718.8, 4347.6, 4598.4 (4658.6) | 4578.6, 4526.4, 4986.6, 5043.2 (4782.6) | 4648, 5045.8, 4862.4, 4685.6 (4774.0) | 7801.6, 7621.6 (7711.6) | 0.604 |
| request-serial 1024 | 5171.2, 5273.4, 5282.2, 5195.2 (5234.3) | 4972.2, 5403, 5079, 4919.2 (5025.6) | 5092.4, 5229.8, 5451.6, 5441.8 (5335.8) | 5258.2, 5232.8, 5031.6, 5190.6 (5211.7) | 9304.4, 9110 (9207.2) | 0.546 |
| request-backpressure 1024 | 16921.6, 16691.2, 16179.2, 17203.2 (16806.4) | 17075.2, 17331.2, 17075.2, 16230.4 (17075.2) | 17177.6, 17433.6, 16742.4, 17203.2 (17190.4) | 17152, 16998.4, 17228.8, 17612.8 (17190.4) | 27060.6, 26327.6 (26694.1) | 0.640 |

The final candidate ratio is the candidate 3872 median divided by the same-day binding median. The four cells remain below 0.65. All benchmark rounds reported matching completed and received counts. `MemAvailable` stayed above 12 GiB (approximately 21–22 GiB during the runs).

## Decisions and contracts

- E3/E4 pending-operation registration: one registration site remains, through `RawServiceMeshRuntime.registerRequest` into `OperationRegistry`; before/after count: 1/1.
- E5 completion turn: one completion-turn decision remains in `RawServiceMeshRuntime.completeRequest` via `queueMicrotask`; before/after count: 1/1.
- No runtime decision, flag, counter, mode, or fallback path was added or changed. The experiment retained the public `data()` copy contract and was reverted after measurement.

## Verification

- Node build: passed (`framework/bench/grpc/node/build.sh`; candidate source before it was reverted).
- Focused contract tests: 82/82 passed, covering encoded-payload copy ownership, envelope validation, pending-operation admission, and completion-turn behavior.
- Full Node suite on the final tree: `flock -s /root/jobs/fx/cx/measure.lock npm test` passed (exit 0; log: `/root/jobs/fx/cx/v21/s033/nfix/final-node-tests.log`).
- `bash scripts/format/format.sh --check node`: passed.
- `git diff --check`: passed.

## Commit and remaining work

Commit: `8d0ce1278fb4953f9debd78ca32043b2fa32e696` (`node: assert encoded payload copies stay independent`). Nothing was pushed.

The performance target remains unfinished: no runtime optimization met the requested gain. The committed change strengthens the copy contract test only.
