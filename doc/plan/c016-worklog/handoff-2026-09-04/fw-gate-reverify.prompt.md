# Task: re-verify the {{LANG}} framework gate against LATEST main, root-cause + minimally fix any remaining regression

The 0.17.0 DONTWAIT framework fix is now **COMMITTED on main** (`4d263e66b9`) together with B's
latest binding review-fixes (`bindings/{{LANG}}: review fixes for the 0.17.0 wait-token port`).
Everything is committed — build from the current working tree, no uncommitted patch to apply.
Your job: build {{LANG}} binding + framework fresh against the pre-built Core, run the full gate,
and for any remaining failure produce a precise root-cause and (only if minimally framework-fixable)
a fix.

## The gate (plan line 143 — all three must pass)
1. **{{LANG}} framework unit tests** — cpp: `ctest --test-dir framework/languages/cpp/build`
   (esp `test_cpp_framework_m6a_runtime`, `m6b_runtime`, `channel_messaging`); dotnet:
   `dotnet test tests/Zlink.Framework.UnitTests` + ContractTests under `flock -w7200 /tmp/zlink-dotnet-gate.lock`
   (esp `StatefulServiceRuntimeTests.RemoteActorStaleAuthority*`,
   `RemoteActorRequest_RetriesPreservedNativeReplyAfterBackpressure`); node: `npm test`; java:
   `cd framework/languages/java && flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon :zlink-framework-core:test contractTest`.
2. **7 common samples** — `framework/languages/{{LANG}}/samples/run_samples.sh`.
3. **cross-language E2E** (the ONLY E2E in scope) — `framework/languages/{{LANG}}/cross-language/run_cross_language_smoke.sh`
   (java: `./gradlew --no-daemon -p cross-language :Host:installDist` first).

## Classify every failure into exactly one bucket
- **(A) DONTWAIT-cleared** — was RED before the fix, now GREEN. Confirm these are green.
- **(B) terminal/error-classification cluster** — the KNOWN separate regression to root-cause:
  cpp `m6a records.size()==1` (bound-session bind), cpp `m6b transport_failure.error_kind()==deadline_exceeded`,
  dotnet `RemoteActorStaleAuthority*` returning `TimedOut(101)` where a stale-terminal(107) is expected.
  For each: trace WHY the wrong terminal/record count is produced. Is it (i) the framework mis-mapping a
  binding/Core status, (ii) B's binding review-fix changing a returned code, or (iii) a Core behavior?
  Capture the exact returned code/errno vs expected, and the framework mapping site (`file:line`).
- **(C) pre-existing (NOT a regression, do NOT fix)** — node lint `spot-timer.ts:137`; java 2×M6A +
  1×JavaDocumentationRegression; cpp `common_e2e_inventory` 278 (feature-map inventory gate). Label and skip.

## Fix policy
- If a bucket-(B) failure is **minimally framework-fixable** (a wrong status→terminal mapping, a missing
  case — no logic rewrite): fix it in `framework/languages/{{LANG}}/**`, re-run, confirm green.
- If root cause is in **Core** (`core/**`) or a **binding** (`bindings/**`): **STOP — do NOT edit those.**
  Write the diagnosis (exact symbol, returned vs expected code, the spec clause it violates if any) to
  your summary. Claude fixes Core directly and coordinates bindings with B.
- Minimal only, no logic rewrite. Hot path: no per-message alloc/string/map added.

## BUILD SETUP
- Core is pre-built at `core/build-dev` (dev, no-LTO, 0.17.0 DONTWAIT `50d77800f2`). **Do NOT rebuild Core.**
  Point the {{LANG}} binding build at it (cpp/node: `ZLINK_CPP_CORE_BUILD_DIR=core/build-dev` /
  `ZLINK_CORE_BUILD_DIR=core/build-dev`; dotnet: `core/build-dev/lib`; java: the jni build against it).
- `export TMPDIR=/dev/shm/zlink-tmp-{{LANG}}` (mkdir -p first). Do NOT use default /tmp for builds.
- Do NOT pass an isolated `--artifacts-path` to `dotnet test` (breaks golden-conformance repo-root discovery).
- Rebuild ONLY the {{LANG}} binding native/addon (dev/no-LTO) + framework. These 4 jobs run concurrently and
  are designed to be safe (independent redis, ephemeral ports, per-language gate locks) — do not serialize.

## Hard rules
- Do NOT modify Core (`core/**`) or bindings (`bindings/**`) — flag with evidence. Do NOT modify specs
  (`core/doc/spec/**`) or `framework/doc/framework/**`. Do NOT commit. Leave any fix uncommitted for Claude.

## Deliverable
- `zlink-work/c016/fw-gate-{{LANG}}-summary.md`: the before→after table (unit/samples/E2E with pass/fail
  counts), every failure bucketed (A/B/C) with `file:line` + exact codes, any minimal fix applied (files +
  what), and for bucket-(B) core/binding root causes the precise diagnosis. Exact commands run.
