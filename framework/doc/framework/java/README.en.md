# ZLink Framework for Java/Kotlin -- Documentation

> This set is the ZLink Framework documentation for `Java`, `Kotlin`, and `Spring Boot`.
> This directory holds `internals/`'s implementation and verification criteria, and the
> public contract lives in the [central Java spec](../common/spec/server/languages/java/README.ko.md).
> Common meaning follows the [common spec](../common/README.ko.md); here we give that
> meaning concrete shape on the Java/Kotlin surface.

The common meaning of async execution, `CompletionStage`, and the Kotlin coroutine wrapper
follows the [Async Execution And Coroutine Policy](../common/spec/05-async-execution-policy.ko.md).

Sample and E2E config files, the ban on environment variables, and
`@ConfigurationProperties`-binding criteria follow the
[Sample/E2E Configuration Policy](../common/sample-e2e-configuration-policy.en.md).

> **Kotlin users** should see the [Kotlin-specific guide](../kotlin/README.ko.md).
> `zlink-framework-kotlin` is a thin coroutine-idiom layer that shares this runtime. The
> contract used directly from Java follows the Java spec, and Kotlin-specific
> `suspend`/`Flow` contracts are fixed separately in the [Kotlin spec](../common/spec/server/languages/kotlin/README.ko.md).
> The Java usage guide will be rewritten at this location once the 11.0 public interface
> and samples are finalized.

Usage of the client libraries used separately from the server framework is found in the
[HTTP client guide](guide/http-client/README.ko.md) and the
[Stream connector guide](guide/stream-connector/README.ko.md).

## 2. Public Contract Spec

The formal spec fixes the framework's target public contract first. Only
`90-implementation-gap.ko.md` records the current gap against the Java/Kotlin
implementation. The public contract for every framework language is managed together under
`spec/<package>/languages/`.

| Document | Scope |
|------|------|
| [Spec table of contents](../common/spec/server/languages/java/README.ko.md) | The list of Java/Kotlin public-contract documents |
| [Java interfaces](../common/spec/server/languages/java/interfaces/README.ko.md) | The exact public signature per feature and the Spring host lifecycle |
| [stream-connector](../common/spec/stream-connector/languages/java/03-stream-connector.en.md) | The client connector |

**The meaning and behavioral rules of a feature are owned by the [common spec](../common/spec/README.ko.md).**
Language-specific documents only fix what shape that meaning takes in Java/Kotlin.

## 3. Internal Criteria

`internals/` explains implementation structure, lifecycle, and regression criteria for
maintainers.

| Document | Scope |
|------|------|
| [backend-dependency-policy](internals/backend-dependency-policy.en.md) | Java binding dependency isolation |
| [Common Internals](../common/internals/README.en.md) | Runtime architecture decisions shared across all four languages |
| [regression-test-matrix](internals/regression-test-matrix.en.md) | JVM contract, E2E, and performance smoke criteria |

## 4. Samples

Samples provide the same scenario set for both Java and Kotlin. The 6 canonical ones are
per-app documents; feature-axis samples are kept as separate documents.

The 6 canonical samples' server roles, message contracts, state transitions, and
completion criteria are owned by the [common sample](../common/sample/README.en.md). The
Java documents don't restate this contract.

| Document | Scope |
|------|------|
| [samples README](../../../languages/java/samples/README.md) | Java/Kotlin sample structure and how to run them |
