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
