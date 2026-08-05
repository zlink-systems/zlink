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
