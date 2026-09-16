한국어 | [English](README.en.md)

[Node 바인딩 스펙](../../spec/node/README.ko.md) · [Node 바인딩 가이드](../../guide/node/index.ko.md)

# Node bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/node/reference/`에 있음)이 아니다. JavaScript
(TypeScript 아닌 순수 JS)는 심링크로 이 bindings 런타임을 그대로 공유한다(별도
`bindings/javascript/` contract 소스가 없다 — `bindings/javascript/samples/`만 존재) —
그래서 이 트리가 JavaScript의 bindings 계층 레퍼런스도 겸한다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. 지금까지의 모든 wrapper binding과 마찬가지로 이 트리도 6개가
아니라 5개 category다 — `zlink/contracts/`엔 `service/` 폴더가 없다. SPOT/Actor는
framework 계층에만 존재한다. 아래 Contract 원본 열은 스펙 산문이 아니라 실제 파일
목록과 대조 확인한 것이다.

아래 Core category에 적용되는 Node 고유 참고 하나: **factory 함수(`createContext`,
`createPairSocket`, ..., `version`, `has`, `proxy`, `sleep`, ...)는 `contracts/core/` 아래에
전혀 선언돼 있지 않다** — static class facade(dotnet의 `Zlink`, java의 `Zlink`, cpp의
`zlink::` 아래 자유 함수)가 아니라 Node/JS의 함수 기반 module export 관례를 따라
package 최상위(`bindings/node/src/index.ts`)에서 export되는 최상위 함수다.
`contracts/core/` 자체는 `Context`/`ContextOptions` interface와 `RoutingId` class만 담고
있다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`zlink/contracts/` + `src/index.ts` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `src/index.ts`(factory 함수 + `version`/`has`/`proxy`/`sleep`/`multipartClose`); `contracts/core/`: `context.ts`, `routing_id.ts`, `buffer_like.ts` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging/`: `message.ts`, `received.ts`, `topic_message.ts`, `subscription_event.ts`, `operations.ts`, `handlers.ts`, `message_parts_envelope.ts` |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets/`: `socket.ts`, `pair_socket.ts`, `dealer_socket.ts`, `router_socket.ts`, `pubsub_sockets.ts`, `stream_socket.ts`, `socket_options.ts`, `socket_constants.ts` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing/`: `monitor.ts`, `poller.ts`, `timer.ts` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors/`: `errors.ts`, `results.ts` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
