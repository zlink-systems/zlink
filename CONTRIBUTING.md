# zlink contributor and operations handbook

This is the entry point for people and agents working in the zlink repository. The rules
themselves live in their owning documents; this page says **where things are and in which
order to do them**. Rules are not duplicated here: one fact has one owning document.

Agents (Claude, Codex) read [`AGENTS.md`](AGENTS.md) first and use this page to find
procedures and locations. Korean version: [`CONTRIBUTING.ko.md`](CONTRIBUTING.ko.md).

## 1. Five-minute start

```bash
git clone git@github.com:zlink-systems/zlink.git && cd zlink

# Core (with tests, Release)
cmake -S core -B core/build -DZLINK_BUILD_TESTS=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
ctest --test-dir core/build -j2

# Binding smoke (uses core/build above)
bash bindings/cpp/tests/run_tests.sh
ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh
```

- Build details, platforms and CMake options: [`doc/building/build-guide.md`](doc/building/build-guide.md),
  [`doc/building/cmake-options.md`](doc/building/cmake-options.md).
- Build trees are fixed by `scripts/build-core.sh`: `dev` (`core/build-dev`, no LTO, tests ON)
  for daily work and ctest; `release` (`core/build`, LTO, tests OFF) for the shipped library and
  perf measurement (one LTO link, about two minutes); `release-gate` (`core/build`, LTO, tests
  ON) only right before a release for `hotpath_gate` and a final LTO ctest. `<mode> --lib-only` rebuilds only the `libzlink`
  runtime of that tree; the perf runners call it when `core/build` is stale (no test relinks). Core test
  executables link the static archive, so enabling tests on the LTO tree re-optimises the whole
  library once per test executable after every Core change (hence tests OFF in `release`).
- If the system Python has no pytest, pass `PYTHON_EXECUTABLE=<venv>/bin/python` as an
  absolute path to the Python binding tests.

## 2. Repository map

| Path | Owns | Entry document |
|---|---|---|
| `core/` | C++ Core runtime and the public C API (`core/include`) | [`core/doc/spec/`](core/doc/spec/README.en.md) (formal contract) |
| `core/tests/` | Core tests (unittest / integration / perf gate) | [`core/tests/README.md`](core/tests/README.md) |
| `bindings/<lang>/` | Language bindings; `bindings/*/include` are **raw header mirrors** of `core/include` | each binding README |
| `bindings/c/perf/` | C benchmarks and the release comparison gate | [`bindings/c/perf/README.md`](bindings/c/perf/README.md) |
| `framework/` | Per-language Framework (actors, DI, codecs) | [`framework/AGENTS.md`](framework/AGENTS.md) |
| `doc/` | User docs, design principles, building, plans | [`doc/README.md`](doc/README.md) |
| `doc/plan/` | Campaign plans and decision logs (not public contract) | §7 |
| `scripts/local-package/` | Local Core/binding packaging and version sync | `scripts/local-package/README.ko.md` |

## 3. Code rules

- Design principles: [`doc/principal/dev/zlink-system-design-principles.md`](doc/principal/dev/zlink-system-design-principles.md),
  POSDDD [`doc/principal/dev/posddd.md`](doc/principal/dev/posddd.md),
  domain map [`doc/principal/dev/zlink-core-domain-map.ko.md`](doc/principal/dev/zlink-core-domain-map.ko.md).
- The Core hot path (code that runs per message) is governed by the formal spec
  [`core/doc/spec/core/systems/10-hot-path.en.md`](core/doc/spec/core/systems/10-hot-path.en.md).
  A change that breaks one of the seven §3 prohibitions (heap allocation, string identity,
  per-socket table lookups, unconditional side work, reader-sleeping previews, fixed sleeps,
  missed temporary-owner signals) is rewritten in the §4 cache/fallback form.
- Comments: [`doc/principal/source-comment-principles.md`](doc/principal/source-comment-principles.md).
  Explain why, never restate the implementation.
- Public API/ABI/enum changes ship with the spec change in a separate commit. An API without
  a contract is split out as a design change and reported before implementation
  (`AGENTS.md` §3).

## 4. Test rules

- Categories and labels (`unittest` / `integration` / `e2e` / `regression`,
  `parallel-safe` / `serial`) are owned by [`core/tests/README.md`](core/tests/README.md).
- Integration tests use **the public C API only**. Tests that rely on internal symbols,
  failpoints or synthetic harnesses break on every refactor and drift from what users
  observe. Races are reproduced deterministically through publicly observable
  synchronisation: monitor events, the poller's `ZLINK_CONFIG_BUSY`, flow state
  (`core/tests/integration/test_wake_invariants.cpp` is the reference). A race that cannot be
  reproduced through the public API is reported as a spec gap, not forced by a test.
- Unit tests compile the sources of the component under test directly (no library link).
  A test that observes library behaviour is an integration test. (Today both kinds still link
  `libzlink-static`; the transition is in §9.)
- No test estimates timing with `sleep`. A new test is repeated five times before it counts
  as green.
- Known load flakes: `test_single_lane_flow_snapshot_accounting` (rare immediate failure in
  the parallel suite, judged by one standalone rerun) and C++ binding test exit 86/134 right
  after a relink (one rerun).

## 5. Gate before every commit

Every Core commit turns the following green and records the result in the commit message.

```bash
ulimit -v 16777216
cmake --build core/build -j
ctest --test-dir core/build -j2                          # full (includes hotpath_gate, wake-invariant)
ctest --test-dir core/build -R '^test_single_lane_' -j2  # x2
for l in c cpp go rust; do for h in zlink_enum.h zlink/socket/api.h zlink/eventing/api.h; do
  cmp -s core/include/$h bindings/$l/include/$h || echo "MIRROR DIFF $l/$h"; done; done
git diff --check
bash bindings/cpp/tests/run_tests.sh
ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh
```

- `hotpath_gate` (callgrind instructions per message, ±5%) is registered only when valgrind
  is found. Without it, the report says "gate not run" and it does not count as green. Only
  a supervisor updates `core/tests/perf/hotpath_reference.json` (`--update-reference`), and an
  intended cost increase is recorded in the decision log with its reason.
- A change that touches performance also runs the release comparison of §6.

## 6. Performance judgement

- The criteria are owned by the spec [`10-hot-path.en.md` §5](core/doc/spec/core/systems/10-hot-path.en.md):
  the 5% per cell (pattern, transport, size, metric) is measurement tolerance; a
  (pattern, transport) whose geometric mean over sizes 64, 256, 1024, 65536 is below the
  baseline is an improvement target.
- Tools: `bindings/c/perf/run_benchmarks.sh` (single), `run_benchmarks_multi.sh` (multi),
  `bindings/c/perf/perf_regression_gate.py`. The baseline is the previous release tag built
  on the same machine in its own worktree (never `--core-version`).
- Nothing else builds or tests on the machine while measuring. Judge cell by cell; when an
  improvement target appears, fix it and re-measure instead of waiting for the whole sweep.
  Confirm noisy single runs with the runner's `--runs 3` (median per size).
- If the bench measures the wrong thing (queue depth reported as latency in the saturated
  interval, rounding coarser than the gate), fix the bench, not the gate, and copy the same
  sources into the baseline worktree.

## 7. Plans and decision logs

- A campaign lives in `doc/plan/<campaign>.ko.md` (plan) and `doc/plan/<campaign>-worklog/`
  (briefs, summaries, drivers, `decisions.ko.md`). `doc/plan/**` is temporary and is never
  linked from public documents ([`doc/AGENTS.md`](doc/AGENTS.md)).
- Decisions are appended to `decisions.ko.md` as `## D-NNN (when, who) title`. When two
  machines work the same campaign in parallel, one side prefixes its numbers (e.g. `D-B54`);
  numbers are never reassigned on merge.
- Agent job briefs (`briefs/*.prompt`) and summaries (`*-summary.md`) are kept verbatim. A
  summary lists changed files, evidence, gate results and BLOCKERS.

## 8. Branches, commits, PRs, releases

- Branches, commits, pushes and merges happen only on an explicit request (`AGENTS.md` §1);
  otherwise work on `main`.
- Commit message: `<module>: <one line>` plus a body with cause, fix, evidence numbers and
  gate results. Refactors keep their items (dead code removal / responsibility split /
  naming) distinguishable in the diff.
- Version bump checklist (one commit):
  1. `VERSION`, `core/CMakeLists.txt`, `core/include/zlink/common.h`, `core/include/zlink.h`.
  2. Raw header mirrors: copy `zlink.h` and `zlink/common.h` from `core/include` into
     `bindings/{c,cpp,go,rust}/include` verbatim (`contract_c_header_mirror` checks this).
  3. Contract tests that hard-code the version:
     `bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp`.
  4. Binding manifests and `scripts/local-package/build-wsl.sh --sync-versions`.
- Release tag preconditions: §5 gate green, `hotpath_gate` PASS, §6 release comparison PASS
  (or a user decision recorded in the decision log), package verification with
  `scripts/local-package/core/verify-package.sh`.
- After a release, move the baseline worktree to the new tag.

## 9. Known debt and planned work

- Test link structure: integration tests will link the release `libzlink.so` dynamically
  (enforcing the public API), unit tests will compile their sources directly. Seventeen
  integration tests that use internal hooks are to be rewritten against the public API, and
  twelve unit tests that observe library behaviour move to integration. Until then, develop on the
  `scripts/build-core.sh dev` tree (§1).
- Carry-overs of the 0.16.0 campaign (the full 70-cell four-size sweep, POSDDD refactor
  BLOCKERS) are in the last entries of
  [`doc/plan/c016-worklog/decisions.ko.md`](doc/plan/c016-worklog/decisions.ko.md).

## 10. Agent operating conventions

- Rule text: [`AGENTS.md`](AGENTS.md) (global) and per-directory `AGENTS.md`. Documentation
  rules: [`doc/AGENTS.md`](doc/AGENTS.md).
- Split large work along module boundaries and run it in parallel. One job is **one cause,
  capped at about 1.5 hours**; if no root fix lands within the cap, the job records what it
  proved and its candidates in the summary and stops. Do not bundle several causes and a
  refactor into one job.
- Jobs do not loop on gates or perf measurements. The supervisor runs the gate once after the
  job exits, reads the diff, and commits with explicit file names (never `git add -A`).
- Jobs do not modify or run `doc/**`, `core/doc/**`, `hotpath_reference.json` or
  `scripts/local-package/**`. A needed spec change is reported as a BLOCKER and committed
  separately by the supervisor.
