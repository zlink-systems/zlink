# zlink Java Binding API Reference

This reference is generated from the public Java API in
`bindings/java/src/main/java/systems/zlink/contracts/`.

## Generate

```bash
cd bindings/java
./gradlew javadoc
```

Generated HTML entrypoint:

```text
bindings/java/build/docs/javadoc/index.html
```

## Scope

- Public contract packages under `systems.zlink.contracts`
- Socket option facades under `systems.zlink.contracts.sockets`
- Internal package (`systems.zlink.internal`) is excluded
- Runtime packages (`systems.zlink.runtime`) are excluded

## DONTWAIT send and completions

`SendSubmitOperation.submit()` is the asynchronous convenience path. Each
native attempt uses DONTWAIT. Immediate admission completes the returned stage
without a SEND completion. If Core reports `BACKPRESSURED` with `EAGAIN`, Core
keeps no payload; it returns a nonzero wait token associated with the target and
user context. The Java binding retains the packet, waits for `POLLOUT`, drains
the completion queue through `NO_DATA`, and retries that packet only after the
matching `CompletionKind.WRITABLE` record arrives.

Asynchronous REQUEST uses the same wait-token admission path. On
`BACKPRESSURED`, the Java binding retains the request until its matching
`WRITABLE`, resubmits it, and then awaits the ordinary REQUEST reply or timeout
completion. `ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` keep
their ABI values but are ignored.

## Native Library Loading

The Java binding loads the native zlink runtime from, in order:

1. `ZLINK_LIBRARY_PATH`, when set.
2. A native directory next to the local binding checkout.
3. Packaged native resources copied to a temporary directory.
4. The JVM `System.loadLibrary("zlink")` fallback.

These override and fallback paths assume a trusted process environment. Do not
allow untrusted users to control `ZLINK_LIBRARY_PATH`, the JVM library search
path, the working directory, or the temporary directory used by a privileged
process.

On Windows, optional OpenSSL/runtime dependency lookup may also consult
`ZLINK_OPENSSL_BIN`, `ZLINK_WINDOWS_RUNTIME_BIN`, known development install
paths, and `PATH`. Security-sensitive deployments should place required DLLs in
the same trusted directory as the zlink native library and control ownership and
write permissions for that directory.
