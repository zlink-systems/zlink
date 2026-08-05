# ZLink Framework C++ — Documentation

The documentation hub for the `zlink::framework` C++ artifact.

| Document | Scope |
|------|------|
| [01 System Structure](../common/spec/server/languages/cpp/01-system-structure.ko.md) | Packages/build targets, application host, **DI container**, configuration, **logging**, HTTP scope/middleware order, feature registration |
| [Exact Interface Per Feature](../common/spec/server/languages/cpp/interfaces/README.ko.md) | App/Host, builder, messaging, Spot, Actor, STREAM, Location, and monitoring's exact public contract |
| [60 HTTP Hosting](../common/spec/server/languages/cpp/60-http-hosting.en.md) | The HTTP hosting contract |
| [61 Embedded HTTP Server](../common/spec/server/languages/cpp/61-embedded-http-server.en.md) | The embedded server |

**The meaning and behavioral rules of a feature are owned by the [common spec](../common/spec/README.ko.md).**
The C++ documents fix the **exact public surface** that meaning takes in C++.

Sample and E2E JSON config files, the ban on environment variables, and typed-binding
criteria follow the [Sample/E2E Configuration Policy](../common/sample-e2e-configuration-policy.en.md).

**Why C++ has more documentation than the other languages** — `.NET` borrows ASP.NET Core,
Node borrows NestJS, and Java borrows Spring Boot, but **C++ has no host to borrow, so the
framework provides host, DI, configuration, logging, and HTTP directly.**

## Documentation For Separate Artifacts

| Artifact | Documentation |
|--------|------|
| HTTP client (`zlink::http_client`) | [Guide](guide/http-client/README.ko.md) · [Spec](../common/spec/http-client/languages/cpp/cpp-http-client.en.md) |
| Stream connector (`zlink::stream_connector`) | [User guide](guide/stream-connector/INDEX.ko.md) |

## Internals List

[Common Internals](../common/internals/README.en.md) ·
[Backend Dependency Policy](internals/backend-dependency-policy.en.md) ·
[Regression Test Matrix](internals/regression-test-matrix.en.md)

The role, DTOs, and verification criteria of the 6 common samples are found in the
[common sample](../common/sample/README.en.md).

For the framework's top-level common documents, see the [guide home](../index.en.md).
