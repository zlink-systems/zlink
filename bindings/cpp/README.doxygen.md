# zlink C++ Binding API Reference

This reference is generated from the C++20 public contract headers in
`bindings/cpp/include/zlink`.

## Generate

```bash
cd bindings/cpp
doxygen Doxyfile
```

Generated HTML entrypoint:

```text
bindings/cpp/doxygen/html/index.html
```

## Scope

- Public C++20 contract headers in `include/zlink/`
- Contract projections for core, messaging, sockets, eventing, and errors
- Runtime-backed public types (`context_t`, `socket_t`, `message_t`, `poller_t`, etc.)
- `context_t::options()` exposes the typed `context_options_t` facade
- `message_t` diagnostics expose `ref_count()`
- `socket_monitor_t` is the public monitoring wrapper for socket-level events and snapshots
