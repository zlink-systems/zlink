# Java Framework request-path header optimization

## Changes

- `ZLinkChannelEnvelope.decodeDispatchHeader` now hands envelope recognition to the same JSON parser pass that validates and decodes the complete header. The root token distinguishes ordinary raw frames from envelope objects. The strict public `decodeHeader` path, malformed-envelope errors, buffer positioning, and decoded-header cache remain intact.
- `jsonString` writes ordinary printable ASCII directly into the existing output buffer. Quote, backslash, control, and Unicode characters continue through the existing escape logic. Correlation and deadline values remain dynamic; encoded bytes are unchanged.
- Added tests for exact dynamic header bytes, dispatch/strict decode equality, malformed-header error equality, and raw-frame handling.
- Completion hand-off was inspected but not changed: the profile did not establish a clear removable cost that could be changed within completion-turn ownership rules.
- Send-saturation was left untouched as requested; its limiting Core IO thread is outside this Java task.

## Ownership and decision locations

- Owner/spec: Java channel-envelope codec; `framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md`, envelope header grammar and strict validation (§ lines 397–420).
- Envelope classification/validation decision sites: 2 before (`decodeDispatchHeader`'s leading-byte classification and `decodeHeader`'s parser validation); 1 after (the parser root-token check plus the same parser's full validation). Existing cache lookup/registration remains in the decoder.
- Encoding escape decision remains in `jsonString`; the ASCII fast path uses that same location and does not add an independent encoding rule.
- Cross-language: Kotlin uses this Java runtime. No other language runtime was changed; wire and validation behavior are covered by golden/equivalence tests.
- Classification: B, existing Java hot-path inefficiency. No API or protocol change.
- Rule count: envelope dispatch classification/validation decision locations 2 → 1; encoding decision locations 1 → 1. No state, flags, counters, queues, or locks added.

## Tests and formatting

- `flock -s /root/jobs/fx/cx/measure.lock ./gradlew test` in `framework/languages/java`: passed (58 actionable tasks; Java and Kotlin tests).
- `flock -s /root/jobs/fx/cx/measure.lock ./gradlew :zlink-framework-core:integrationTest --tests systems.zlink.framework.runtime.ChannelMessagingTest`: passed.
- Java benchmark build: `(cd framework/bench/grpc/java && flock -s /root/jobs/fx/cx/measure.lock ./build.sh)`: passed.
- `bash scripts/format/format.sh --check java`: passed. The formatter emitted a JVM `Unsafe` deprecation warning from its bundled Kotlin tooling.
- `git diff --check`: passed. Temporary benchmark CPU pinning was reverted before commit.
- Available memory remained above 12 GiB throughout measurement (lowest observed about 20.9 GiB).

## A/B measurements

Each Framework cell has four rounds, alternating the changed worktree and prebuilt main (`ee11390257`); binding has two same-day rounds. Each round used `WARMUP_SECONDS=30`, `DURATION_SECONDS=5`, and the shared exclusive measurement lock. Values are completions/second. Ratios use the median Framework throughput divided by the median binding throughput; Framework A/B effect is changed-worktree median divided by main median.

| Cell | Changed Framework rounds; median | Main Framework rounds; median | Binding rounds; median | Changed/main | Changed/binding |
| --- | --- | --- | --- | ---: | ---: |
| request-backpressure 1024 | 59071.4, 56876.4, 58849.2, 59839.4; **58960.3** | 59611.6, 58028.4, 58561.2, 57557.4; **58294.8** | 100533.2, 119906.2; **110219.7** | **1.011 (+1.1%)** | **0.535** |
| request-serial 1024 | 8803.0, 8947.6, 8945.0, 8787.6; **8874.0** | 8778.8, 8363.2, 8762.6, 8381.4; **8572.0** | 9377.2, 11028.8; **10203.0** | **1.035 (+3.5%)** | **0.870** |
| request-serial 4096 | 8728.2, 8063.8, 8159.2, 5158.6; **8111.5** | 8216.0, 7945.0, 8546.8, 5267.4; **8080.5** | 11184.2, 12978.0; **12081.1** | **1.004 (+0.4%)** | **0.671** |

The change improved all three Framework medians over the same-day main runs, with modest gains. Same-day binding ratios clear 0.6 for serial requests, but request-backpressure 1024 remains at 0.535. The 4096 serial results had a low fourth round in both Framework variants; the median is reported without dropping that round. Binding round-to-round spread is material, so these ratios are the requested same-day comparison rather than a claim of stable long-term throughput.

## Commit and remaining work

- Commit: `5da76a632ab62137fb3b01bed7a17cdd4045cd12` (`perf(java): reduce channel envelope header overhead`), branch `perf/3870-java-header`; not pushed.
- The request-backpressure 1024 ratio remains below the 0.6 target. Further optimization is unfinished; this commit does not claim the target is met.
