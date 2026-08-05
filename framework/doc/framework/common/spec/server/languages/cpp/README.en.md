# ZLink Framework C++ Public Contract

This directory owns the **formal public contract** the C++ framework
must provide. Public headers and contract tests must follow this
contract.

C++ Framework doesn't delegate host capability to a separate
application framework. Unlike .NET, which uses ASP.NET Core, Node.js,
which uses NestJS, and Java, which uses Spring Boot, it directly
provides host, DI, configuration, logging, and HTTP capability. So
C++'s HTTP public contract is defined as a separate document in this
directory.

| No. | Document | Scope |
|---|------|------|
| `01` | [System Structure](01-system-structure.en.md) | Package/build target, application host, **DI container**, **configuration**, **logging**, lifecycle, registration surface |
| `02` | [Exact Interface Per Capability](interfaces/README.en.md) | Server package's per-capability C++ public type and member |
| `03` | [Location · Relocation Store · Redis Relocation Notice](03-location-store.en.md) | Links to the per-capability exact interface's Store/Redis document |
| `60` | [HTTP Hosting](60-http-hosting.en.md) | HTTP hosting contract |
| `61` | [Embedded HTTP Server](61-embedded-http-server.en.md) | Embedded server |

**The meaning and behavior rule of a capability is owned by the
[common spec](../../../README.en.md).** This directory fixes the
**exact public surface** that meaning has in C++.

**The internal runtime structure isn't a public contract** — it's
owned by [internals/runtime-architecture](../../../../internals/README.en.md).

The client connector is owned by the
[C++ Stream Connector guide](../../../../../cpp/guide/stream-connector/INDEX.en.md)
and the
[Stream Connector common spec](../../../stream-connector/32-stream-connector.en.md).

## Cancellation Argument

The C++ public interface **doesn't put a custom cancellation token
copying the `.NET` shape as a default callback argument.** If a
cancellable long-running operation needs explicit cancellation
delivery, it uses **standard C++ lifetime and cancellation
convention**. Timeout, host shutdown, RAII cleanup, and coroutine
lifetime follow each capability contract.
