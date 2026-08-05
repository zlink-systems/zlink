[한국어](01-overview.ko.md)

<!-- zlink-nav:start -->
[Core API →](02-core-api.en.md)
<!-- zlink-nav:end -->

# zlink overview

zlink Core is a raw messaging runtime. It provides contexts, typed raw sockets,
multipart messages, pollers, generic timers, socket monitors, TLS, and network
transports. Application topology and stateful-object behavior belong to the
Framework packages.

## Runtime layers

```text
+------------------------------------------------------+
| Application and language bindings                    |
+------------------------------------------------------+
| Public C API: context, socket, message, eventing      |
+------------------------------------------------------+
| Socket patterns: PAIR, PUB/SUB, XPUB/XSUB,           |
| DEALER/ROUTER, STREAM                                 |
+------------------------------------------------------+
| Protocol engines and transports                      |
+------------------------------------------------------+
| Context, I/O threads, pipes, mailboxes, messages      |
+------------------------------------------------------+
```

The public API validates handles and arguments before delegating to the socket
runtime. Socket implementations own pattern-specific behavior. Protocol engines
encode frames, and transports perform operating-system I/O.

## Socket patterns

| Socket | Use |
|---|---|
| PAIR | One-to-one communication |
| PUB/SUB | Topic-based distribution |
| XPUB/XSUB | Subscription-aware proxying |
| DEALER/ROUTER | Asynchronous routed request/reply |
| STREAM | Raw byte-stream integration |

Supported endpoint schemes depend on the build and include `tcp`, `ipc`,
`inproc`, `ws`, `wss`, and `tls`.

## Start here

Build Core and its tests:

```bash
cmake -S core -B core/build -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build
```

Choose a socket pattern in [Socket Patterns](03-0-socket-patterns.en.md), then use
the [Core API](02-core-api.en.md) and [Message API](09-message-api.en.md) guides. The
[Monitoring](06-monitoring.en.md) guide covers raw socket events and snapshots.

<!-- zlink-nav:bottom:start -->
[Core API →](02-core-api.en.md)
<!-- zlink-nav:bottom:end -->
