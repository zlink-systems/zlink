# zlink Test Suite

`core/tests` is the single test root for zlink.

## Directory Layout

| Path | Purpose |
| --- | --- |
| `core/tests/unittest/` | small internal tests labeled `unittest` |
| `core/tests/integration/` | focused functional tests labeled `integration` |
| `core/tests/e2e/` | executable-level smoke tests labeled `e2e` |
| `core/tests/testutil*` | shared test helpers |
| `core/tests/run_test_lanes.sh` | sequential lane runner |

## Lane Policy

Tests are classified along two axes:

- category: `unittest`, `integration`, `e2e`, `regression`
- execution mode: `parallel-safe`, `serial`

Current policy is intentionally conservative:

- `unittest` => `parallel-safe`
- `integration` => `serial`
- `e2e` => `serial`
- `regression` => `serial`

Important:

- there is no separate `integration-fast` or `integration-heavy` lane today
- the repository keeps a single `integration` lane
- fast/heavy distinction is enforced by registration policy, not by extra lane
- heavy scale, matrix, long-sequence, and historical flake coverage should
  move to `regression`, not stay in the default `integration` lane

`RESOURCE_LOCK` only coordinates tests inside one `ctest` process. Do not run
multiple `ctest` commands concurrently for serial lanes.

## Commands

Quick start:

```bash
./core/build.sh
```

Manual configure/build:

```bash
cmake -S core -B core/build -DZLINK_BUILD_TESTS=ON
cmake --build core/build -j"$(nproc)"
```

Lane execution:

```bash
ctest --test-dir core/build --output-on-failure -L unittest -j"$(nproc)"
ctest --test-dir core/build --output-on-failure -L integration -j1
ctest --test-dir core/build --output-on-failure -L e2e -j1
ctest --test-dir core/build --output-on-failure -L regression -j1
```

Sequential lane runner:

```bash
./core/tests/run_test_lanes.sh
./core/tests/run_test_lanes.sh --include-e2e
./core/tests/run_test_lanes.sh --include-e2e --include-regression
```

Thread-safe contract stress runner:

```bash
./core/tests/run_thread_safe_contract_stress.sh
./core/tests/run_thread_safe_contract_stress.sh --count 10
./core/tools/run_execution_gate_loop.sh --count 10
./core/tools/run_codex_execution_guide_loop.sh
./core/tools/run_codex_execution_guide_loop.sh --stress-count 10
```

- `10`은 thread-safe stress의 기본/최소 반복 횟수다.
- 더 높은 신뢰도나 flake 재현이 필요하면 `--count` 또는 `--stress-count`를 더 크게 줄 수 있다.

Thread-safe contract perf runner:

```bash
./core/tests/run_thread_safe_contract_perf.sh
./core/tests/run_thread_safe_contract_perf.sh --min-ratio 0.85
```

Thread-safe contract TSan runner:

```bash
./core/tests/run_thread_safe_contract_tsan.sh
./core/tests/run_thread_safe_contract_tsan.sh --build-dir core/build-tsan-clang
```

Default runner behavior:

- `unittest` in parallel
- `integration` serially
- `e2e` only when `--include-e2e` is specified
- `regression` only when `--include-regression` is specified

Default time budget:

- `unittest` lane total: target `<= 10s`
- `integration` lane total: target `<= 120s`
- `e2e` lane total: target `<= 180s`

Recommended per-test budget:

- `unittest`: target `<= 1s`, hard review above `3s`
- `integration`: target `<= 5s`, hard review above `10s`
- `e2e` binary: target `<= 30s`, hard review above `60s`
- `regression`: no default budget, but explicit purpose is required

## Writing Tests

- Add new internal logic tests under `core/tests/unittest/`.
- Add new focused behavior tests under `core/tests/integration/`.
- Add new representative executable-level smoke tests under `core/tests/e2e/`.
- Add long-running or historical flake coverage under the `regression` lane
  unless it must remain in the default integration path.
- Every pattern must retain its minimum coverage set:
  `basic`, `invalid`, `teardown`, and `status-or-topology`.
- Start new tests in the safest lane first. Only promote to `parallel-safe`
  after confirming the test does not depend on live socket timing, discovery
  state, global env mutation, or teardown ordering.
- Do not add retry logic or sleep-based retry loops. Use deterministic events
  and hard timeouts.

## Notes

- Some scenario-style e2e tests still live outside `core/tests/` because they
  are tied to benchmark/source-stack fixtures under `bindings/c/bench/`.
- `test_thread_safe_scaling_contract` is a split-case wrapper executable only.
  Its top-level CTest entry is intentionally not registered; use the
  `test_thread_safe_scaling_raw` cases instead.
- heavy process benchmark coverage should not stay in the default lane unless
  it is the single representative smoke for that pattern.
- `run_thread_safe_contract_stress.sh` repeats the selected thread-safe
  contract cases at the CTest layer. It does not add retry logic inside the
  tests themselves.
- `run_execution_gate_loop.sh` is a repo-local wrapper for long-running stress
  gates. It keeps one shell process alive across gate completion, writes
  timestamped logs under `doc/plan/refactor/2nd/logs/`, and automatically runs
  a single-test repro when the stress gate fails.
- `run_codex_execution_guide_loop.sh` is a higher-level Codex supervisor for
  the remaining execution guide. It repeatedly runs `codex exec`, tells Codex
  to continue from the first incomplete guide item, and stops only on exact
  sentinel output (`미적용 사항이 없습니다.` or `사용자 입력 필요: ...`).
- The stress lane currently covers raw monitor contract regressions that are
  registered in CTest.
- `run_thread_safe_contract_perf.sh` executes the raw 1/4/16/64 handle
  scaling contract cases with a configurable acceptance ratio.
- `run_thread_safe_contract_tsan.sh` configures a dedicated TSan build and
  runs the same raw monitor thread-safe regression lane against that build
  tree.
- CURVE/libsodium and GSSAPI are not supported in zlink.
