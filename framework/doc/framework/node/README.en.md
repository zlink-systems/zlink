# ZLink Framework for Node.js -- Documentation

> This set is the formal ZLink Framework documentation for `Node.js` and `NestJS`. This
> directory holds `internals/` (implementation and verification criteria), and the public
> contract lives in the [central Node.js spec](../common/spec/server/languages/node/README.ko.md).
> Common meaning follows the [common spec](../common/README.ko.md); here we give that
> meaning concrete shape only on the Node.js and NestJS surface. The public contract is
> owned by the central per-language spec and the common framework spec; other-language
> implementations are used only as reference material for comparing contract
> interpretation.

The common meaning of async execution, `Promise`, and helper synchronous functions
follows the [Async Execution And Coroutine Policy](../common/spec/05-async-execution-policy.ko.md).
The Node framework's server and client network APIs are projected as `Promise`-based
async functions. The `Async` suffix isn't carried over -- the async contract is expressed
through the action name and a `Promise<T>` return type, as in `connect()`, `close()`,
`submit()`, `waitFor()`, `start()`, `stop()`, `handle()`. Pure helpers that don't do
network I/O -- codec conversion, packet-name calculation, value-object construction -- can
be synchronous functions.

Sample and E2E config files, the ban on environment variables, and NestJS typed
configuration provider criteria follow the
[Sample/E2E Configuration Policy](../common/sample-e2e-configuration-policy.en.md).

## 1. Usage Guidance

Check the Node framework's public API and behavior in the formal spec below. Runnable
usage examples are found in the [common sample](../common/sample/README.en.md) and the
[Node sample](../../../languages/node/samples/README.ko.md).

Usage of the client libraries used separately from the server framework is found in the
[HTTP client guide](guide/http-client/README.ko.md) and the
[Stream connector guide](guide/stream-connector/README.ko.md).

## 2. Public Contract Spec

The **formal contract** for the NestJS surface. It describes only the public API that
actually exists in the current Node code and regression tests.

| Document | Scope |
|------|------|
| [system-structure](../common/spec/server/languages/node/01-system-structure.en.md) | Package structure, NestJS registration, lifecycle, and startup validation |
| [Interface table of contents](../common/spec/server/languages/node/interfaces/README.ko.md) | The interface/decorator/context/options catalog by category |

**The meaning and behavioral rules of a feature are owned by the [common spec](../common/spec/README.ko.md).**
Language-specific documents only fix what shape that meaning takes in Node/NestJS.

## 3. Internal Criteria (`internals/`)

`internals/` defines backend dependencies, internal lifecycle, and regression criteria for
maintainers. Check the spec for the public API and allowed combinations.

| Document | Scope |
|------|------|
| [backend-dependency-policy](internals/backend-dependency-policy.en.md) | Backend replaceability, public-surface isolation |
| [Common Internals](../common/internals/README.en.md) | Runtime architecture decisions shared across all four languages |
| [regression-test-matrix](internals/regression-test-matrix.en.md) | Regression test criteria |

## 4. Common Samples

The 6 canonical samples' server roles, message contracts, state transitions, and
completion criteria are owned by the [common sample](../common/sample/README.en.md). The
Node.js documents don't restate this contract.

## 5. Regression Tests

This README is maintained together with the following document regression tests.

| Test | Verification criteria |
|--------|-----------|
| `documentation-regression.test.js › node README does not link removed legacy guide chapters` | Never re-adds a link to a removed old guide chapter. |
| `documentation-regression.test.js › node documentation relative markdown links resolve` | Relative links between documents never break. |
| `documentation-regression.test.js › node interface specification documents the current execution-turn APIs` | Checks that the execution-turn API matches the formal interface spec. |
| `sample-regression.test.js › node samples define required files and use only common sample documents` | Checks that the sample implementation only references the common sample documents and never recreates a removed per-language sample README. |
