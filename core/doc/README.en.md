# ZLink Core Documentation

ZLink Core is the C runtime that provides the context, raw sockets, messages,
poller, timer, monitor, and transport. Application topology and stateful
object runtime are covered in the Framework documentation.

| Area | Location | Contents |
|---|---|---|
| User guide | [guide/](guide/README.en.md) | Raw socket patterns, transport, TLS, monitoring, and performance |
| Public contract | [spec/](spec/README.en.md) | The exact contract of the Core C API |
| Internal implementation | [internals/](internals/architecture.en.md) | The context, socket, engine, protocol, and transport structure |

For per-language usage, see [`bindings/doc/`](../../bindings/doc/README.ko.md) (Korean-only);
for application runtime, see [`framework/doc/`](../../framework/doc/README.ko.md)
(Korean-only).
