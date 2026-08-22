# Stage 8: language binding flow-state parity (plan §7.3 / §8.1.1 / checklist §12.4)

Plan: `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §5.1, §7.3, §8.1.1.
C layer base commit: `f1ec634656` (autohwm stage7 worklog for the public C API,
events and metrics). Each language section below is appended independently by
its own worker; append additively and do not remove another language's
section.

## cpp

Base commit: `f1ec634656`. Changed only under `bindings/cpp/`, plus one shared
`core/src/libzlink.vers` fix (see "Shared C-layer defect found" below).

### Surface added

`bindings/cpp/include/zlink/Contracts/Sockets/socket_options.hpp`:

```cpp
enum class receive_flow_state_t : int
{
    running = 0,
    paused = 1
};
```

`bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp` (on the
common `socket_t` base, same placement as `set_tls_server`/`set_tls_client`,
so every socket type gets a runtime not-supported mapping instead of a
compile-time-restricted facade):

```cpp
void socket_t::set_receive_flow_state (receive_flow_state_t state_);
```

Implementation in `bindings/cpp/src/Runtime/Sockets/socket.cpp` follows the
existing direct-`zlink_config_result_t` pattern used by
`router_socket_options_t::connect_routing_id()` (`throw_if_failed<config_error_t>`
around the raw C call), not the errno-mapped pattern used by
`set_tls_server`/`set_tls_client` — `zlink_socket_set_receive_flow_state`
already returns `zlink_config_result_t` directly, so no separate
`config_result_from_errno()` step is needed:

```cpp
void socket_t::set_receive_flow_state (receive_flow_state_t state_)
{
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_socket_set_receive_flow_state (detail::native_handle (*this),
                                           static_cast<zlink_receive_flow_state_t> (state_))));
}
```

`zlink::config_result_t` values already match `zlink_config_result_t` 1:1
(`invalid_handle=701`, `invalid_argument=702`, `not_supported=703`,
`invalid_state=705`), so the `static_cast` is a direct, lossless mapping —
no new binding-specific error-code table was needed.

### Not exposed (plan §5.1 forbidden list)

No flow-frame encode/decode/receive API and no PAUSE-bypass send variant were
added anywhere in the public surface. Verified both by a `static_assert`
SFINAE trait check in the new test (`has_flow_frame_api_t`,
`has_pause_bypass_send_t`) and by not adding any such method to `socket_t` or
any socket subclass.

### Test: `bindings/cpp/tests/contract/test_cpp_contract_flow_state.cpp`

Registered in `bindings/cpp/CMakeLists.txt`'s `ZLINK_CPP_CONTRACT_TEST_SOURCES`.
Checklist §8.1.1 coverage:

| §8.1.1 item | Test |
|---|---|
| enum value parity with C ABI | `static_assert` on `running==0`, `paused==1` |
| DEALER/ROUTER success + idempotent repeat | `test_dealer_router_set_succeeds_and_is_idempotent` |
| PAIR/PUB/SUB/STREAM → not-supported | `test_unsupported_socket_types_report_not_supported` |
| unsupported-socket send/recv unaffected | `test_unsupported_socket_send_recv_is_unchanged` |
| invalid handle/argument mapping | `test_invalid_handle_reports_invalid_handle` |
| close race → one observable outcome | `test_close_then_set_reports_invalid_handle_or_invalid_state` (mirrors the two observable close-race shapes documented in `worklog/stage7-c-api.md` §1.2: a live race reports `invalid_state`, a fully-torn-down handle reports `invalid_handle`) |
| no flow-frame/PAUSE-bypass API on the public surface | `static_assert` SFINAE checks (`has_flow_frame_api_t`, `has_pause_bypass_send_t`) |
| existing HWM/EAGAIN-equivalent behavior unchanged | `test_existing_hwm_backpressure_is_unchanged` (PAIR socket smoke check — DEALER/ROUTER's public `send()` only exposes an `async()` terminal, so PAIR's synchronous `flags()+submit()` builder is used to observe backpressure directly, matching the existing pattern in `test_cpp_contract_behavior.cpp`) |

### Shared C-layer defect found (fixed, one line, additive only)

`core/src/libzlink.vers` (the linker version-script export allowlist for
`libzlink.so`) was never updated by stage7 to export
`zlink_socket_set_receive_flow_state`. The function exists in
`core/include/zlink/socket/api.h` and is implemented in
`core/src/api/core/zlink_flow_state_api.cpp`, but without a version-script
entry the linker hides it from the shared library's dynamic symbol table, so
any binding dynamically linking against `libzlink.so` fails at build/link
time (`undefined reference to zlink_socket_set_receive_flow_state`). This
blocks the cpp binding (and appears to affect every other language binding
that declares `extern "C" zlink_socket_set_receive_flow_state` for direct
linking — confirmed present at the time of this change in
`bindings/rust/src/runtime/native/ffi.rs`, `bindings/go/internal/native/*`).

Reproduced independently with a minimal C program before touching binding
source:

```text
$ gcc t.c -I core/include -L core/build/lib -lzlink -o t
undefined reference to `zlink_socket_set_receive_flow_state'
```

Fix (added to the `global:` list, immediately after `zlink_socket_monitor_recv;`):

```text
zlink_socket_set_receive_flow_state;
```

This is a single additive allowlist entry with no logic change — it does not
touch any binding-owned source and does not change the ABI itself (the
already-implemented function is simply now exported as intended). Given every
language binding in §7.3 needs this fixed to link at all, and the fix carries
no semantic risk, it was applied directly rather than reported as a blocker.

### Build + test evidence

Local core build source used: `ZLINK_CORE_SOURCE=local` (the default
`run_tests.sh` behavior fetches a pinned `0.11.1` release package instead of
the local `core/build`, which would not contain the new API — this is not a
defect in the release flow, just the wrong mode for validating a local core
change).

```bash
cmake --build core/build --parallel "$(nproc)"   # rebuilds libzlink.so with
                                                  # the flow-state symbol now exported
ZLINK_CORE_SOURCE=local bash bindings/cpp/tests/run_tests.sh
```

Result: 13/14 contract tests pass, all 7 sample smoke tests pass. The single
failure is pre-existing and unrelated to this change:

```text
test_cpp_contract_socket: test_received_lifetime_retains_and_releases_core_hwm_credit
Assertion `released.current_accounted_bytes () == 0u' failed.
```

Verified pre-existing by re-running the identical `ZLINK_CORE_SOURCE=local`
suite with all of this stage's changes stashed out (bare `f1ec634656` worktree
state plus a rebuilt `core/build`): the same test fails with the same
assertion, and it is additionally the binding-level reflection of the
already-known, already-tracked core defect recorded in the plan's own
checklist — `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §12.2 row
1 records `test_retained_hwm_credit` as a pre-existing 20/20 deterministic
failure in this worktree, since the byte-HWM restoration (checklist §12.2) is
not yet complete. `test_cpp_contract_monitor` also failed at the stashed
baseline for an unrelated reason — its compiled-in `ZLINK_MONITOR_STATUS_ABI_VERSION`
came from the (until this change) stale `bindings/cpp/include/zlink_enum.h`
mirror (ABI 3) while the rebuilt runtime reports ABI 4 — and passes once this
stage's header-mirror sync is in place.

### Files changed

- `bindings/cpp/include/zlink/Contracts/Sockets/socket_options.hpp` — new `receive_flow_state_t` enum
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp` — new `socket_t::set_receive_flow_state()` declaration
- `bindings/cpp/src/Runtime/Sockets/socket.cpp` — implementation + `<zlink.h>` include
- `bindings/cpp/include/zlink_enum.h`, `bindings/cpp/include/zlink/socket/api.h`, `bindings/cpp/include/zlink/eventing/api.h` — synced from `core/include` (stale C header mirror; also fixes the pre-existing `test_cpp_contract_monitor` ABI-version failure noted above)
- `bindings/cpp/tests/contract/test_cpp_contract_flow_state.cpp` — new focused contract test
- `bindings/cpp/CMakeLists.txt` — registers the new test
- `core/src/libzlink.vers` — adds the missing export (shared fix, see above)

### Result

```text
Result: cpp flow-state binding parity implemented and tested; 13/14 contract
  tests + 7/7 sample smoke tests pass; the 1 failure is pre-existing and
  reproduced identically without this change.
Changed source: see "Files changed" above.
Changed public contract: bindings/cpp public C++ surface only (no core/doc/spec
  or bindings/doc/spec changes); core/src/libzlink.vers export-list fix (no ABI
  change, only makes an already-declared/implemented symbol linkable).
Focused tests: bindings/cpp/tests/run_tests.sh (ZLINK_CORE_SOURCE=local) — see
  "Build + test evidence" above.
Paired perf reports: none (out of scope per plan §8.1.1 — binding perf is not
  this stage's gate).
First remaining failure: test_cpp_contract_socket::test_received_lifetime_retains_and_releases_core_hwm_credit,
  pre-existing (plan §12.2 row 1, unrelated to flow-state work).
Framework work started: no
```

Commit: `cpp: expose receive-flow-state parity` (this repository, branch
`codex/bindings-0.11.1-performance`).

## rust

Base commit: `f1ec634656`. Changed only under `bindings/rust/` (source, tests,
and the `bindings/rust/include` C header mirror).

### Surface added

`bindings/rust/src/contracts/sockets/socket_options.rs`:

```rust
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ReceiveFlowState {
    Running = 0,
    Paused = 1,
}
```

On `CommonSocketOptions` (same facade as `set_rid_duplicate_policy`, so every
socket type gets it and Core's own not-supported mapping applies at runtime):

```rust
pub fn set_receive_flow_state(&self, value: ReceiveFlowState) -> Result<(), ConfigError> {
    self.inner.set_receive_flow_state(value as i32)
}
```

`bindings/rust/src/runtime/native/ffi.rs` declares the raw entry point as a
plain `c_int`, not a `#[repr(C)]` enum, deliberately:

```rust
pub fn zlink_socket_set_receive_flow_state(handle: *mut c_void, state: c_int) -> c_int;
```

`bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs` follows the
existing direct-function pattern used by `set_routing_id` (a dedicated
extern "C" call routed through `check_config_rc`, not the generic
`zlink_set_option` path used by simple scalar options):

```rust
pub(crate) fn set_receive_flow_state(&self, value: i32) -> Result<(), ConfigError> {
    check_config_rc(unsafe { ffi::zlink_socket_set_receive_flow_state(self.handle, value) })
}
```

`check_config_rc`/`config_result_from_errno` (`src/runtime/errors/native_errors.rs`,
unchanged) already map every `zlink_config_result_t` value used here
(`EFAULT→InvalidHandle`, `EINVAL→InvalidArgument`, `ENOTSUP/EOPNOTSUPP→NotSupported`,
`EBUSY→InvalidState`), so no new error-mapping table was needed.

### Not exposed (plan §5.1 forbidden list)

No flow-frame encode/decode/receive method and no PAUSE-bypass send variant
were added to any public socket type. `tests/flow_state_tests.rs` documents
this by construction: the file only imports `ReceiveFlowState` and
`CommonSocketOptions::set_receive_flow_state`, and the crate's public surface
(`src/lib.rs` re-exports) has no flow-frame type or method to reach.

### Why the raw FFI parameter is `c_int`, not the `zlink_receive_flow_state_t` enum

A `#[repr(C)]` Rust enum can only ever legally hold one of its declared
discriminants; constructing one from an arbitrary out-of-range `i32` (e.g. via
`transmute`) is undefined behavior in Rust even though the same bit pattern is
a perfectly ordinary `int` on the C side. Declaring the extern function's
parameter as `c_int` sidesteps that: `ReceiveFlowState` still only offers
`Running`/`Paused` to callers (cast to `i32` at the call site), but nothing
about the FFI declaration itself requires materializing an invalid enum value.
The crate keeps a `#[repr(C)]` `zlink_receive_flow_state_t` mirror in `ffi.rs`
purely as ABI documentation; it is not used as a function-argument type.

### Test: `bindings/rust/tests/flow_state_tests.rs`

New file, added to `tests/run_tests.sh`'s `run_test_file` list. Checklist
§8.1.1 coverage:

| §8.1.1 item | Test |
|---|---|
| enum value parity with C ABI | `receive_flow_state_enum_matches_c_abi_values` |
| DEALER/ROUTER success + idempotent repeat | `dealer_accepts_receive_flow_state_and_is_idempotent`, `router_accepts_receive_flow_state_and_is_idempotent` |
| PAIR/PUB/SUB/XPUB/XSUB/STREAM → not-supported | `pair_reports_not_supported`, `pub_reports_not_supported`, `sub_reports_not_supported`, `xpub_reports_not_supported`, `xsub_reports_not_supported`, `stream_reports_not_supported` |
| invalid handle mapping | `closed_socket_reports_invalid_handle` |
| invalid argument mapping | `receive_flow_state_type_cannot_represent_an_out_of_range_value` — `ReceiveFlowState` only has two variants, so the Rust type system itself is this binding's mapping of `ZLINK_CONFIG_INVALID_ARGUMENT`: there is no public-API path that can construct a third state. Documented in the test rather than exercised as a runtime error (see cpp's section above for a language where the analogous case is instead a runtime-checked cast). |
| close race → one observable outcome | `set_before_close_then_close_yields_only_expected_results`, `close_then_set_yields_only_invalid_handle`. `SocketStorage` is `Send` but not `Sync` (`src/internal/handle_storage.rs`), so the public API gives no way for two threads to hold the same socket handle at once — the borrow checker forces every `set_receive_flow_state`/`close` pair onto one of exactly two sequential orderings. Both orderings are exercised 20× each; both only ever produce `Ok`, `InvalidState`, or `InvalidHandle`. |
| no flow-frame/PAUSE-bypass API on the public surface | verified by absence, see "Not exposed" above |
| existing HWM behavior unchanged | `existing_high_water_mark_options_are_unaffected` (set/get round-trip on `CommonSocketOptions`, unchanged since before this stage) |

### Header mirror sync

`bindings/rust/include/{zlink_enum.h, zlink/socket/api.h, zlink/eventing/api.h}`
had not yet picked up the stage7 C additions (`zlink_receive_flow_state_t`,
`zlink_socket_set_receive_flow_state`, the three `ZLINK_EVENT_SEND_FLOW_*`/
`FLOW_STATE_STALE` monitor bits, `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`).
Copied verbatim from `core/include` (the same byte-identical mirror convention
already used by `bindings/c/include`, per
`scripts/local-package/native/sync-local-core-libs.sh`'s `copy_public_headers`).
`diff -rq core/include bindings/rust/include` now reports no differences
(other than files core-only headers don't have, i.e. none).

### Shared C-layer defect (found already fixed)

While validating the link, `bindings/rust` initially failed with
`undefined symbol: zlink_socket_set_receive_flow_state` against a freshly
rebuilt `core/build/lib/libzlink.so` — `core/src/libzlink.vers` had not been
updated by stage7 to export the new symbol from the shared library's version
script, even though the symbol exists in the static lib and the header. This
is the same defect the cpp worker recorded above and fixed with a single
additive `global:` entry; by the time this rust work reached its own build
step, that fix was already present in the shared `core/src/libzlink.vers` (not
made by this rust change — no `core/` files are part of this commit).

### Shared-build hazard observed (not fixed here, worked around)

The repository-shared `core/build/` directory is used by more than one
concurrent language worker. During this work `core/build/lib/libzlink.so.0.11.1`
was briefly observed as a 0-byte file (another worker's in-progress relink),
which produced spurious "every symbol undefined" failures unrelated to this
change. Also, at one point every tracked-file edit in this stage's working
tree (across `bindings/rust/`, `core/src/libzlink.vers`, `tests/run_tests.sh`)
was reverted back to the pre-edit committed state by an external event (all
new *untracked* files, including this stage's own `flow_state_tests.rs` and
other languages' new test files, were unaffected) — the edits were redone
identically and committed immediately afterward to avoid losing them again.
Neither issue is a defect in this stage's own change; both were worked around
by building Core in a private, non-shared directory
(`cmake -S core -B <scratch>/core-build-rust -G Ninja -DCMAKE_BUILD_TYPE=Release
-DZLINK_BUILD_TESTS=OFF && cmake --build <scratch>/core-build-rust -j`) and
pointing `ZLINK_RUST_NATIVE_DIR` at its `lib/` output for all reported test
runs below, so the results are not subject to either race.

### Build + test evidence

`tests/run_tests.sh`'s default (`ZLINK_CORE_SOURCE` unset) fetches a pinned,
already-released `0.11.1` Core package that predates this stage's C API and
therefore correctly fails to link `flow_state_tests` — this is expected, not a
regression; the plan's own C-layer worklog (`worklog/stage7-c-api.md`) is
validated the same way, against a local Core build, not the release channel.

```bash
source "$HOME/.cargo/env"
cmake -S core -B /tmp/.../core-build-rust -G Ninja -DCMAKE_BUILD_TYPE=Release -DZLINK_BUILD_TESTS=OFF
cmake --build /tmp/.../core-build-rust -j"$(nproc)"
cd bindings/rust
ZLINK_RUST_NATIVE_DIR=/tmp/.../core-build-rust/lib \
LD_LIBRARY_PATH=/tmp/.../core-build-rust/lib \
CARGO_TARGET_DIR=/tmp/zlink-rust-target/manual \
bash tests/run_tests.sh
```

Result: 12/12 suites pass (`surface_tests`, `contract_tests`, `behavior_tests`,
`send_failure_tests`, `receive_failure_tests`, `boundary_tests`,
`option_tests`, `flow_state_tests`, `ownership_tests`, `monitor_tests`,
`optimization_guard_tests`, `samples`), 0 failures. `flow_state_tests` alone:
14/14 tests pass (`cargo test --test flow_state_tests -- --test-threads=1`).

Pre-existing baseline verified separately, without this stage's rust source
changes staged, same local Core build, same 11 suites that existed before this
change (i.e. the list above minus `flow_state_tests`): 11/11 pass, 0 failures
— this stage introduces no regression in any pre-existing rust suite.
`cargo test --lib` (crate-internal unit tests, not part of `run_tests.sh`'s
list): 1/1 passes, unaffected.

### Files changed

- `bindings/rust/src/runtime/native/ffi.rs` — `zlink_receive_flow_state_t` ABI-documentation mirror, `zlink_socket_set_receive_flow_state` extern declaration
- `bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs` — `SocketStorage::set_receive_flow_state`
- `bindings/rust/src/contracts/sockets/socket_options.rs` — `ReceiveFlowState` enum, `CommonSocketOptions::set_receive_flow_state`
- `bindings/rust/src/lib.rs` — re-exports `ReceiveFlowState`
- `bindings/rust/include/zlink_enum.h`, `bindings/rust/include/zlink/socket/api.h`, `bindings/rust/include/zlink/eventing/api.h` — synced verbatim from `core/include` (stale mirror)
- `bindings/rust/tests/flow_state_tests.rs` — new, §8.1.1 focused test
- `bindings/rust/tests/run_tests.sh` — registers `flow_state_tests` in the run list

### Result

```text
Result: rust flow-state binding parity implemented and tested; 12/12 test
  suites pass (14/14 tests in the new flow_state_tests file); no regression in
  the 11 pre-existing suites, verified against the same local Core build with
  this stage's rust changes unstaged.
Changed source: see "Files changed" above. No core/ files are part of this
  commit (the shared core/src/libzlink.vers export fix was already present,
  made by another concurrent worker; see "Shared C-layer defect" above).
Changed public contract: bindings/rust public surface only (no core/doc/spec
  or bindings/doc/spec changes).
Focused tests: bindings/rust/tests/run_tests.sh with ZLINK_RUST_NATIVE_DIR
  pointed at a local Core build — see "Build + test evidence" above.
Paired perf reports: none (out of scope per plan §8.1.1 — binding perf is not
  this stage's gate; no code was added to any binding hot path, only to the
  existing typed-option facade already used by set_rid_duplicate_policy and
  friends).
First remaining failure: none found in this stage's own suite; run_tests.sh's
  default release-package mode fails only because that pinned package
  predates this unreleased Core API (expected, see "Build + test evidence").
Framework work started: no
```

Commit: `rust: expose receive-flow-state parity` (this repository, branch
`codex/bindings-0.11.1-performance`, commit `c000ca6e74`).
