# zlink Rust Binding API Reference

This reference is generated from the Rust source in `bindings/rust/src/`.

## Generate

```bash
cd bindings/rust
cargo doc --no-deps
```

Generated HTML entrypoint:

```text
bindings/rust/target/doc/zlink/index.html
```

## Scope

- Public API of the `zlink` crate
- Public socket and monitor types re-exported at the crate root
- Domain objects (`Message`, error types, enums)
- FFI internals (`zlink::ffi`) are private and excluded

The current approved crate payload contains the Core 11.2.0 Linux x86_64
runtime. Other target triples fail during build until a matching native
runtime has passed the Core package provenance and clean-consumer checks.

## Context Thread Safety

`Context` is `Send` and `Sync`. Applications may share it across threads, for
example with `std::sync::Arc`, and those threads may create sockets from the
same context. The context is terminated when the last owning `Context` value is
dropped. Keep an owning reference alive while another thread is creating or
using sockets.

## Monitor Contract Note

- `*_READY_CHANGED` monitor events are readiness edge/state notifications.
- Rust bindings must not interpret monitor `value` as an aggregate ready count.
- Monitor snapshots are for state/queue inspection, not ready-count gates.
- Perf or readiness verification in Rust bindings must follow the shared perf
  policy.
- raw sockets: `CONNECTION_READY` event counting
- SPOT: explicit benchmark barrier protocol; no separate service-event gate
