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
  runtime of that tree; the perf runners call it when `core/build` is stale (no test relinks). Integration,
  contract and C test executables link the shared `libzlink` and unit tests link one non-LTO `test-core`
  archive, so a Core change no longer relinks the library per test; `release` still builds without tests
  because the LTO library link itself is the slow part.
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
| `doc/plan/` | Campaign plans and decision logs (not public contract) | §8 |
| `doc/principal/` | Design principles (`dev/`), comment principles, technical-writing principles and guides (`documentation/`) | §3, §4 |
| `scripts/local-package/` | Local Core/binding packaging and version sync (`sync-version.py`) | `scripts/local-package/README.ko.md` |
| `scripts/gate/` | Machine-local integrated gates (bindings, framework, cross-language) | [`scripts/gate/README.md`](scripts/gate/README.md) |
| `scripts/perf/` | Performance measurement ticket queue (`perf-ticket.sh`, `perf-queue-runner.sh`) | §7 |
| `.github/workflows/`, `.github/actions/`, `scripts/ci/` | Build, release and CI workflows with their helpers | [`doc/building/release-pipeline.md`](doc/building/release-pipeline.md) |
| `doc/building/` | Build guide, packaging, release pipeline, accounts, release notes and preparation records | [`doc/building/release-pipeline.md`](doc/building/release-pipeline.md) |

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

## 4. Documentation rules

- Every technical document follows the [technical writing principles](doc/principal/documentation/documentation-principles.ko.md)
  (reader and scope, narration and verification, prose style, present-state description, post-writing
  review). Pick the per-kind procedure from [`doc/principal/documentation/README.ko.md`](doc/principal/documentation/README.ko.md):
  [spec](doc/principal/documentation/spec-writing-guide.ko.md), [user guide](doc/principal/documentation/guide-writing-guide.ko.md),
  [reference](doc/principal/documentation/reference-writing-guide.ko.md), [E2E scenario](doc/principal/documentation/e2e-scenario-writing-guide.ko.md),
  [sample](doc/principal/documentation/sample-writing-guide.ko.md), [diagram](doc/principal/documentation/diagram-authoring-guide.ko.md).
  Read the principles first, then the guide for the kind you are writing, and finish with that guide's
  completion checklist. (These guides are currently Korean-only.)
- Placement, ownership and linking rules (one fact owned by one document, `doc/plan/**` never linked
  from public documents, Korean/English pairs kept together) are owned by [`doc/AGENTS.md`](doc/AGENTS.md).
- Specs (`core/doc/spec/**`, `bindings/doc/spec/**`, `framework/doc/**/spec/**`) and plan/policy
  documents are edited by the supervisor only; agent jobs report needed changes as BLOCKERS (§10).

## 5. Test rules

- Categories and labels (`unittest` / `integration` / `e2e` / `regression`,
  `parallel-safe` / `serial`) are owned by [`core/tests/README.md`](core/tests/README.md).
- Integration tests use **the public C API only**. Tests that rely on internal symbols,
  failpoints or synthetic harnesses break on every refactor and drift from what users
  observe. Races are reproduced deterministically through publicly observable
  synchronisation: monitor events, the poller's `ZLINK_CONFIG_BUSY`, flow state
  (`core/tests/integration/test_wake_invariants.cpp` is the reference). A race that cannot be
  reproduced through the public API is reported as a spec gap, not forced by a test.
- Unit tests link the single non-LTO `test-core` archive and may assert on private Core state;
  a test that observes library behaviour is an integration test and uses only the public C API
  against the shared `libzlink` (`core/tests/README.md`, "Interface Boundary").
- No test estimates timing with `sleep`. A new test is repeated five times before it counts
  as green.
- Known load flakes: `test_single_lane_flow_snapshot_accounting` (rare immediate failure in
  the parallel suite, judged by one standalone rerun) and C++ binding test exit 86/134 right
  after a relink (one rerun).

## 6. Gate before every commit

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
- The release comparison of §7 runs during release preparation (the per-change obligation is
  `hotpath_gate` alone — [`10-hot-path.en.md` §5](core/doc/spec/core/systems/10-hot-path.en.md)).
- Run the full binding/framework scope with `scripts/gate/{bindings-gate,framework-gate,cross-language-e2e}.sh <tag>`
  and read `zlink-work/gates/<tag>/results.txt`. One gate at a time, started below load average 10
  (timing asserts are load-sensitive). A test broken by load is judged by a solo rerun; tolerances
  are never widened.
- The framework's default builds, solutions and CI include only the `cross-language` e2e. Per-language
  scenario e2e (`framework/languages/<lang>/e2e/*`) and the seven samples (`samples/*`) run only through
  their `run_e2e.sh`, `run_samples.sh` and Node `npm run test:samples`, and are never added to the sln,
  the default CMake targets, the Gradle root build, CI or releases.

### Interface boundary of Core tests

- Integration, contract, and end-to-end tests use only the public C API headers under `core/include` and link the shared `libzlink`.
- Assertions about private Core structures, injected failures, and implementation-owned state live under `core/tests/unittest` and link the single non-LTO `test-core` archive compiled once.
- Test code and shared helpers never expose private Core headers or symbols, and no test executable LTO-links the production Core archive.
- When splitting a test, record where each original assertion moved, which public observation replaced it, and which duplicates were removed, together with its CTest lane; production LTO settings and the hotpath reference stay untouched.

## 7. Performance judgement

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

## 8. Plans and decision logs

- A campaign lives in `doc/plan/<campaign>.ko.md` (plan) and `doc/plan/<campaign>-worklog/`
  (briefs, summaries, drivers, `decisions.ko.md`). `doc/plan/**` is temporary and is never
  linked from public documents ([`doc/AGENTS.md`](doc/AGENTS.md)).
- Decisions are appended to `decisions.ko.md` as `## D-NNN (when, who) title`. When two
  machines work the same campaign in parallel, one side prefixes its numbers (e.g. `D-B54`);
  numbers are never reassigned on merge.
- Agent job briefs (`briefs/*.prompt`) and summaries (`*-summary.md`) are kept verbatim. A
  summary lists changed files, evidence, gate results and BLOCKERS.

## 9. Branches, commits, PRs, releases

- Branches, commits, pushes and merges happen only on an explicit request (`AGENTS.md` §1);
  otherwise work on `main`.
- Commit message: `<module>: <one line>` plus a body with cause, fix, evidence numbers and
  gate results. Refactors keep their items (dead code removal / responsibility split /
  naming) distinguishable in the diff.
- Number rules (Core `MAJOR.MINOR`, binding `CORE_MAJOR.CORE_MINOR.N`, framework `MAJOR.MINOR.HOTFIX`) are
  owned by [`doc/building/versioning.md`](doc/building/versioning.md).
- Version bump checklist (one commit):
  1. Edit only the root `VERSION` (Core) and `BINDINGS_VERSION` (binding and framework pins).
  2. Run `python3 scripts/local-package/sync-version.py --write`; it updates `core/CMakeLists.txt`,
     the public headers, the raw header mirrors (`bindings/{c,cpp,go,rust}/include`), binding
     manifests, framework pins, the first debian changelog stanza and contract snapshots at once.
     Never hunt for pins by hand.
  3. Add a section to `core/CHANGELOG.md` (the Core release notes are extracted from it).
  4. Check for missing pins with `scripts/local-package/build-wsl.sh --verify-versions`.
- Release tag preconditions: §6 gate green, `hotpath_gate` PASS, §7 release comparison PASS
  (or a user decision recorded in the decision log), package verification with
  `scripts/local-package/core/verify-package.sh`.
- Every publish happens **in GitHub Actions**. Never run `npm publish`, `dotnet nuget push` or a
  Central upload locally, and never create API tokens (npm and nuget use Trusted Publishing, Maven
  Central uses repository secrets). The order is Core (`core/vX.Y.Z` tag + `build.yml` dispatch) →
  the four bindings (`cpp/`, `node/`, `java/`, `dotnet/v*` tags) → the four frameworks (one
  `framework/vA.B.C` tag). Workflows, triggers, channels and verification commands are owned by
  [`doc/building/release-pipeline.md`](doc/building/release-pipeline.md); accounts and secrets by
  [`doc/building/release-accounts.md`](doc/building/release-accounts.md); ConanCenter and vcpkg go
  through PRs ([`doc/building/pr-drafts/`](doc/building/pr-drafts/)).
- Supported platforms are linux-x64, linux-arm64, macos-arm64, windows-x64 and windows-arm64. Intel
  Mac is unsupported from Core up and never appears in CI matrices or prebuilds.
- After a release, move the baseline worktree to the new tag, and record workflow or procedure fixes
  made during release preparation under `doc/building/release-prep/<date>-<topic>.ko.md`.

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
