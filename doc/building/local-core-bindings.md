[English](./local-core-bindings.md) | [한국어](./local-core-bindings.ko.md)

# Linking Bindings Against a Local Core — Test and Perf Reference

A reference for running contract tests and perf against the **Core you are working on (your
branch)**, without a release artifact. The cited scripts own the actual behaviour; this document is
the map of *what to call, in what order, with which environment variables*.

The central distinction:

| Purpose | Core source | What it points at | Entry point |
|------|-----------|-----------------|--------|
| **Contract tests** | live `core/build-dev` (current as of every build) | headers `core/include` + `core/build-dev/lib/libzlink` | `scripts/gate/bindings-gate.sh` |
| **Perf measurement** | **fixed** install prefix (unchanged during measurement) | the prefix `ZLINK_CORE_PACKAGE_PREFIX` names | `scripts/perf/perf-ticket.sh` |

The principle is that tests measure "the source as it is now" and perf measures "a frozen
artifact". That is why the two link differently.

## 1. Binding contract tests — the live `core/build-dev`

After changing Core source — say your branch adds a new public symbol — making the bindings see
that symbol needs neither a release package nor `build-wsl.sh`. Each binding's `tests/run_tests.sh`
takes `ZLINK_CORE_SOURCE=local` plus the local Core include/lib paths and **links directly against
the `core/build-dev` tree**.

The procedure:

```bash
# 1) Build the branch Core as dev (new symbols land in core/include + core/build-dev/lib/libzlink)
JOBS=4 scripts/build-core.sh dev

# 2) Every binding contract suite (relative to core/build-dev, serialized behind the samples lock)
scripts/gate/bindings-gate.sh <tag>
```

`bindings-gate.sh` sets up the environment below per language and calls `tests/run_tests.sh`
(`scripts/gate/common.sh`: `CORE_LIB=$Z/core/build-dev/lib`, `CORE_VER` = `LIBZLINK_VERSION` from
`VERSION`). Use this table as written when running a single language.

| Binding | Local Core link environment |
|--------|--------------------------|
| c | `ZLINK_C_CORE_BUILD_DIR=core/build-dev ZLINK_CORE_INCLUDE_DIR=core/include` |
| cpp | `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=core/build-dev ZLINK_BUILD_JOBS=4` |
| dotnet | `ZLINK_LIBRARY_PATH=core/build-dev/lib` |
| java | `ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR=core/include ZLINK_CORE_LIB_DIR=core/build-dev/lib` |
| node | `ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH=core/build-dev/lib/libzlink.so.<CORE_VER> LD_LIBRARY_PATH=core/build-dev/lib` |
| python | `ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH=core/build-dev/lib/libzlink.so PYTHON_EXECUTABLE=<venv python>` |
| go | (no environment — cgo links relative to the tree) |
| rust | `LD_LIBRARY_PATH=core/build-dev/lib` |

A single language, for example:

```bash
JOBS=4 scripts/build-core.sh dev
ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR="$PWD/core/build-dev" ZLINK_BUILD_JOBS=4 \
  bash bindings/cpp/tests/run_tests.sh
```

- `ZLINK_GATE_CORE_LIB` / `ZLINK_GATE_CORE_VERSION` override the target to a release prefix (for
  example `~/.cache/zlink/core/<ver>/<platform>/lib`). Use them only when comparing against a
  release; when testing your branch's new symbols, keep the default (`core/build-dev`).
- `bindings/*/include` is a **raw header mirror** of `core/include`. Adding a symbol to a public
  header means updating the mirror (`python3 scripts/local-package/sync-version.py --check`, then
  `--write` if needed).

## 2. Perf measurement — a fixed Core prefix

Core must not change while perf is measuring. `perf-ticket.sh` **warns against**
`ZLINK_CORE_SOURCE=local`: with it the runner rebuilds `core/build` automatically, so Core changes
mid-measurement and you end up measuring a dev build rather than the fixed prefix (see the comments
in `scripts/perf/perf-ticket.sh`). Build a **fixed install prefix** instead and point
`ZLINK_CORE_PACKAGE_PREFIX` at it.

Measuring the branch Core (the "after" side):

```bash
# 1) Build the branch Core as release (LTO, the shipped library)
JOBS=4 scripts/build-core.sh release

# 2) Materialize it into a fixed prefix — use a separate location so the official release cache
#    is not overwritten
ZLINK_CORE_CACHE_DIR=<branch-only-cache> \
  scripts/gate/materialize-local-core-prefix.sh core/build
#   -> creates the prefix at <branch-only-cache>/<ver>/<platform>

# 3) Start the runner exactly once (this is what makes measurement serial), then submit tickets
nohup scripts/perf/perf-queue-runner.sh >/dev/null 2>&1 &
ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=<branch-only-cache>/<ver>/<platform> \
  scripts/perf/perf-ticket.sh submit -p 1 -d "issue-XX after" -- <measurement command>
```

- `perf-ticket.sh` records the `ZLINK_CORE_SOURCE` and `ZLINK_CORE_PACKAGE_PREFIX` present at
  submission time into the ticket verbatim. **Serialization comes from the fact that there is one
  runner** (no concurrent perf). One measurement at a time.
- Measure the baseline (the "before" side) by pointing at an already-built official or reference
  prefix the same way. The C canonical baseline and the comparison method are owned by the perf
  plans (`doc/perf/perf/**`).
- `materialize-local-core-prefix.sh` writes to `<ver>/<platform>` under the default cache root
  (`~/.cache/zlink/core`). When the branch version equals the release version — before a version
  bump, say — that **overwrites the official cache**, so pass `ZLINK_CORE_CACHE_DIR` to keep a
  branch-only location separate.

## 3. Ownership

- Link behaviour is owned by `scripts/gate/bindings-gate.sh` and `scripts/gate/common.sh` (tests),
  and by `scripts/perf/perf-ticket.sh` and `perf-queue-runner.sh` (perf).
- Build trees and modes (`dev` / `release` / `release-gate`) are owned by `scripts/build-core.sh`
  and [`build-guide.md`](build-guide.md).
- The shared local package cache (§4.1) and version synchronization are owned by
  `scripts/local-package/` and the [development workflow](../principal/dev/development-workflow.md).
- This document collects only what sits on top of those: the two paths for linking bindings against
  a local Core without a release — tests and perf.
