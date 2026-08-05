# C++ Framework Regression Test Matrix

> This document is an internals document for C++ framework maintainers. The public API contract is
> verified against the `framework/doc/framework/common/spec/server/languages/cpp/` documents and public headers.

## 1. Test Layers

| Layer | Scope |
|------|------|
| `contract` | Public headers, namespaces, builder surface |
| `unit` | Channel, route, dispatch helper, actor/spot runtime units |
| `integration-single-process` | Host/runtime combinations within one process |

## 2. Dispatch Error Observer Regression

| ID | Layer | Test Location | Pass Criteria |
|----|------|-------------|-----------|
| DERR-001, DERR-007, DERR-011, DERR-014 | `contract`, `unit` | `Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp`, `Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp` | The global observer registration surface exists; a missing channel request handler ends in an error reply plus observer event, a missing channel send handler ends in a drop plus observer event, and an observer exception doesn't break the original dispatch result |
| DERR-002, DERR-008 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp` | A missing route request handler ends in an error reply; a missing route send handler ends in a drop plus observer event |
| DERR-003, DERR-004, DERR-009, DERR-010, DERR-016 | `unit`, `integration-single-process` | `Zlink.Framework.UnitTests` SPOT/actor dispatch items | A SPOT route, subscription, or actor dispatch failure ends in an error reply or caller-visible error for a request, or a drop plus observer event for one-way |
| DERR-005, DERR-006, DERR-013, DERR-015 | `unit`, `integration-single-process` | `Zlink.Framework.UnitTests` channel/SPOT dispatch items | Decode failures and handler exceptions end in an error reply or an observable drop, with the default log and counter still recorded even without an observer registered |

## 3. Release Gate

C++ framework changes build the relevant targets, then run according to the `ctest` label scheme.
Dispatch error observer changes must at minimum pass the contract header test and the channel
messaging unit test.

## 4. Spot Yield Dispatch Regression

| ID | Layer | Test Location | Pass Criteria |
|----|------|-------------|-----------|
| SYLD-001 | `contract` | `Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp` | `yield()` exists on request, actor join, bound session send, and worker call, and is not exposed on route request or ordinary send/publish. |
| SYLD-002 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_spot_runtime.cpp` | The default `submit()` of request and worker keeps the serial gate, and `yield()` lets other mailbox work run before returning to the original continuation. Actor join keeps the same gate with `async()`. |
| SYLD-003 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp` | The serial execution queue distinguishes a released turn from normal completion. |
| SYLD-004 | `contract`, `sample` | `test_cpp_framework_sample_parity`, `test_cpp_framework_layout_contract` | The C++ sample layout matches the public contract documents, and the Entry Spot actor handler example doesn't depend on `yield()`. |
| SYLD-005 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_spot_runtime.cpp` | An Entry Spot actor packet runs on its target actor's mailbox. Different actors are not blocked by the Entry Spot's single queue, and consecutive packets of the same actor are processed in order. A `yield()` inside an Entry Spot actor handler becomes a contract error immediately, with no timeout. |

## 5. Application Framework Regression Matrix

Regression tests include both feature-level unit tests and application flow tests.

| Axis | Required Tests |
|----|-------------|
| contract header | `zlink/framework.hpp`, detailed contract header compilation |
| app host | run/stop, signal stop, exit code, startup validation failure |
| DI | singleton/scoped/transient, constructor injection, optional/required service, duplicate service, shutdown resolve failure |
| configuration | JSON/env/CLI merge, environment profile, typed binding, required value missing, validation failure |
| HTTP routing | `GET/POST/PUT/DELETE`, duplicate route, unknown route `404`, method mismatch `405`, route parameter binding, query binding |
| HTTP JSON | valid body, invalid JSON `400`, unsupported content type, response serialization, standard error response |
| HTTP DI | route handler constructor injection, request scope disposal, scoped service isolation across requests |
| HTTP lifecycle | accept loop start/stop, in-flight request drain, request during shutdown `503`, worker join |
| HTTP to zlink | an HTTP handler sends a channel request through `request_client_t` and returns a response DTO |
| zlink channel request/reply | client/server request, typed reply, timeout, handler not found, payload decode failure, reply serialization failure, disconnected peer |
| zlink channel send/event | fire-and-forget send, typed event dispatch, handler exception masking, no-reply path, queue full rejection |
| zlink pub/sub | publisher/subscriber delivery, multiple subscribers, unsubscribe/close cleanup, topic mismatch, disconnected subscriber |
| zlink route channel | manual connection, discovery connection, routing id selection, route handler dispatch, route handler not found, ambiguous route validation |
| zlink backpressure | pending request limit, outbound queue limit, send-ready resume, shutdown while pending, same error kind for coroutines |
| zlink serializer/codec | raw message, JSON DTO, optional MessagePack/Protobuf target off/on, serializer missing startup failure, invalid payload runtime failure |
| zlink lifecycle | channel bind/connect start order, receive loop start/stop, in-flight drain, shutdown after close, reconnect/disconnect event |
| SPOT | activation, destroy, join, leave, actor handler, publish, request_to, route resolver, discovery-backed remote address |
| SPOT ordering | same user Spot packet/timer/subscription ordering, actor packet ordering, Entry Spot timer non-global serialization |
| SPOT timer | CAPI timer projection, tick metadata, skipped tick, overrun policy, cancel, handler exception monitoring |
| STREAM | packet session, connected/disconnected/error callback, header validation, reply, write backpressure, disconnect cleanup |
| STREAM ordering | same session callback serialization, invalid header drop, close during pending write, session-scoped service disposal |
| ActorGateway | session bind, local actor relay, remote actor relay, bound session push, actor lookup, actor generation round-trip |
| ActorGateway failure | duplicate actor, type mismatch, missing actor, disconnected bound session, relay timeout, cleanup after disconnect |
| Registry/discovery | Spot remote address lookup, duplicate resolver rejection, ambiguous route channel validation, snapshot diff interval |
| handler model | sync handler, coroutine handler, exception filter, logging filter, validation filter |
| logging | console/file/category, correlation id, HTTP request log, zlink message log |
| observability | health/readiness/liveness, metrics event, trace/correlation, queue depth |
| error mapping | framework error to HTTP status, zlink error to framework error, internal exception masking |
| auth extension | auth filter registration, token extraction hook, current user context propagation |
| scheduling | hosted service, periodic timer, delayed task, graceful stop |
| developer tooling | CMake presets, vcpkg manifest, install consumer, CLion/Visual Studio configure smoke |
| samples | Bingo e2e, TicTacToe HTTP `POST /games` + zlink channel + STREAM connector e2e, server/client file log assertions |

zlink-related regression tests are based on the expected behavior in the common framework spec. The
C++ server framework uses the `co_await submit()` surface, but error kinds such as timeout, decode
failure, handler not found, shutdown, queue full, and disconnected, and their log/monitoring events,
carry the same fixed meaning. Tests must not just check the process exit code — they must also verify
the request sequence, topic/packet name, correlation id, server-side file log, and client-side result
together.

CTest labels are split at minimum as follows:

- `framework-contract`
- `framework-unit`
- `framework-integration`
- `framework-http`
- `framework-zlink`
- `framework-zlink-channel`
- `framework-zlink-spot`
- `framework-zlink-stream`
- `framework-zlink-actor-gateway`
- `framework-zlink-registry`
- `framework-host`
- `framework-config`
- `framework-observability`
- `framework-sample-smoke`
- `framework-sample-e2e`
- `framework-package`

Sample regression tests must not just check the executable's success — they must confirm the HTTP
request, zlink channel request, STREAM connector request, notification callback, and server-side log
all together.

> This matrix was moved from `01-application-framework.ko.md` when the public contract spec was
> compressed into 3 documents. The contract's meaning is owned by [C++ System Structure](../../common/spec/server/languages/cpp/01-system-structure.en.md)
> and the common spec.
