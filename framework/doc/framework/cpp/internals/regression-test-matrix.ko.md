# C++ Framework Regression Test Matrix

> 이 문서는 C++ framework 유지보수자를 위한 internals 문서다. 공개 API 계약은
> `framework/doc/framework/common/spec/server/languages/cpp/` 문서와 public header를 기준으로 확인한다.

## 1. 테스트 계층

| 계층 | 범위 |
|------|------|
| `contract` | public header, namespace, builder 표면 |
| `unit` | channel, route, dispatch helper, actor/spot runtime 단위 |
| `integration-single-process` | 한 process 안의 host/runtime 조합 |

## 2. Dispatch Error Observer Regression

| ID | 계층 | 테스트 위치 | 통과 기준 |
|----|------|-------------|-----------|
| DERR-001, DERR-007, DERR-011, DERR-014 | `contract`, `unit` | `Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp`, `Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp` | 전역 observer 등록 표면이 존재하고, channel request handler 없음은 error reply와 observer event, channel send handler 없음은 drop과 observer event로 끝나며 observer 예외는 원래 dispatch 결과를 깨지 않음 |
| DERR-002, DERR-008 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp` | route request handler 없음은 error reply, route send handler 없음은 drop과 observer event로 끝남 |
| DERR-003, DERR-004, DERR-009, DERR-010, DERR-016 | `unit`, `integration-single-process` | `Zlink.Framework.UnitTests` SPOT/actor dispatch 항목 | SPOT route, subscription, actor dispatch 실패가 request면 error reply 또는 caller-visible error, one-way면 drop과 observer event로 끝남 |
| DERR-005, DERR-006, DERR-013, DERR-015 | `unit`, `integration-single-process` | `Zlink.Framework.UnitTests` channel/SPOT dispatch 항목 | decode 실패와 handler 예외는 error reply 또는 관측 가능한 drop으로 끝나며, observer 미등록 시에도 기본 로그와 counter가 남음 |

## 3. Release Gate

C++ framework 변경은 관련 target 을 빌드한 뒤 `ctest` label 체계에 맞춰 실행한다. dispatch error
observer 변경은 최소한 contract header test 와 channel messaging unit test 를 통과해야 한다.

## 4. Spot yield dispatch regression

| ID | 계층 | 테스트 위치 | 통과 기준 |
|----|------|-------------|-----------|
| SYLD-001 | `contract` | `Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp` | request, actor join, bound session send, worker call에 `yield()`가 있고, route request와 일반 send/publish에는 노출되지 않는다. |
| SYLD-002 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_spot_runtime.cpp` | request와 worker의 기본 `submit()`은 serial gate를 유지하고, `yield()`는 다른 mailbox 작업을 실행하게 한 뒤 원래 continuation으로 돌아온다. Actor join은 `async()`로 같은 gate를 유지한다. |
| SYLD-003 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp` | serial execution queue가 released turn과 normal completion을 구분한다. |
| SYLD-004 | `contract`, `sample` | `test_cpp_framework_sample_parity`, `test_cpp_framework_layout_contract` | C++ sample layout과 public contract 문서가 일치하고, Entry Spot actor handler 예제는 `yield()`에 의존하지 않는다. |
| SYLD-005 | `unit` | `Zlink.Framework.UnitTests/test_cpp_framework_spot_runtime.cpp` | Entry Spot actor packet은 대상 actor mailbox에서 실행된다. 서로 다른 actor는 Entry Spot 단일 큐에 막히지 않고, 같은 actor의 연속 packet은 순서대로 처리된다. Entry Spot actor handler 안의 `yield()`는 timeout 없이 즉시 계약 오류가 된다. |

## 5. Application framework 회귀 매트릭스

회귀 테스트는 기능별 단위 테스트와 application flow 테스트를 모두 포함한다.

| 축 | 필수 테스트 |
|----|-------------|
| contract header | `zlink/framework.hpp`, 세부 contract header compile |
| app host | run/stop, signal stop, exit code, startup validation failure |
| DI | singleton/scoped/transient, constructor injection, optional/required service, duplicate service, shutdown resolve failure |
| configuration | JSON/env/CLI merge, environment profile, typed binding, required value missing, validation failure |
| HTTP routing | `GET/POST/PUT/DELETE`, duplicate route, unknown route `404`, method mismatch `405`, route parameter binding, query binding |
| HTTP JSON | valid body, invalid JSON `400`, unsupported content type, response serialization, standard error response |
| HTTP DI | route handler constructor injection, request scope disposal, scoped service isolation across requests |
| HTTP lifecycle | accept loop start/stop, in-flight request drain, request during shutdown `503`, worker join |
| HTTP to zlink | HTTP handler가 `request_client_t`로 channel request를 보내고 response DTO를 반환 |
| zlink channel request/reply | client/server request, typed reply, timeout, handler not found, payload decode failure, reply serialization failure, disconnected peer |
| zlink channel send/event | fire-and-forget send, typed event dispatch, handler exception masking, no-reply path, queue full rejection |
| zlink pub/sub | publisher/subscriber delivery, multiple subscribers, unsubscribe/close cleanup, topic mismatch, disconnected subscriber |
| zlink route channel | manual connection, discovery connection, routing id selection, route handler dispatch, route handler not found, ambiguous route validation |
| zlink backpressure | pending request limit, outbound queue limit, send-ready resume, shutdown while pending, coroutine 동일 error kind |
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

zlink 관련 회귀 테스트는 framework 공통 스펙의 기능 기대값을 기준으로 한다. C++ 서버
framework는 `co_await submit()` 표면을 사용하지만, timeout, decode failure,
handler not found, shutdown, queue full, disconnected 같은 error kind와 로그/monitoring event는
같은 의미로 고정한다. 테스트는 단순히 process exit code만 확인하지 않고, request sequence,
topic/packet name, correlation id, server-side file log, client-side 결과를 함께 검증해야 한다.

CTest label은 최소 아래처럼 나눈다.

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

샘플 회귀 테스트는 실행 파일 성공만 보면 안 된다. HTTP request, zlink channel request,
STREAM connector request, notification callback, server-side log를 모두 확인해야 한다.

> 이 매트릭스는 공개 계약 스펙을 3문서로 압축하면서 `01-application-framework.ko.md`에서
> 옮겨 왔다. 계약의 의미는 [C++ 시스템 구조](../../common/spec/server/languages/cpp/01-system-structure.ko.md)와
> 공통 스펙이 소유한다.
