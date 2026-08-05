한국어 | [English](README.en.md)

[Rust 바인딩 스펙](../../spec/rust/README.ko.md) · [Rust 바인딩 가이드](../../guide/rust/index.ko.md)

# Rust bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/rust/reference/`에 있음)이 아니다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. 지금까지의 모든 wrapper binding과 마찬가지로 이 트리도 6개가
아니라 5개 category다 — `src/contracts/`엔 `service/` 모듈이 없다. SPOT/Actor는
framework 계층에만 존재한다. 아래 Contract 원본 열은 스펙 산문이 아니라 실제 파일
목록과 대조 확인한 것이다.

아래 모든 category에 적용되는 Rust 고유 참고:

- **factory/utility 자유 함수(`version`, `has`, `proxy`, `sleep`, `poll`, ...)는
  `contracts/core/` 아래에 전혀 선언돼 있지 않다** — crate 최상위(`bindings/rust/src/lib.rs`)
  의 순수 함수다, node의 package-root export 스타일에 대응하는 Rust 관용구이지, static
  facade 타입(dotnet의 `Zlink`, java의 `Zlink`)이나 전용 namespace의 자유 함수(cpp의
  `zlink::`)가 아니다.
- **socket type 전체를 아우르는 공유 기반 trait이 없다.** 모든 구체 socket은 자신만의
  inherent `impl` block을 가진 독립 struct다. `bind`/`connect`/`unbind`/`disconnect`/TLS
  메서드는 (PUB/SUB/XPUB/XSUB 4개 타입은 내부 macro를 통해) 독립적으로 재선언되며,
  지금까지 다룬 다른 모든 언어가 제공하는 공유 `Socket`/`ConnectableSocket` trait에서
  상속되는 게 아니다. `Pollable`/`Monitorable`이 유일하게 여러 타입을 가로지르는
  trait이며, 둘 다 `sealed`다 — crate 사용자는 custom 타입에 대해 둘 중 어느 것도
  구현할 수 없다.
- **`ZlinkError`는 각 typed error variant를 감싸는 Rust enum이다**(`Submit(SubmitError)`,
  `Request(RequestError)`, ...) — 상속 기반 class가 아니라 "여러 typed error 중 하나"를
  표현하는 Rust다운 형태다.
- **이 binding의 public contract엔 async/Future를 반환하는 request submit이 없다** —
  지금까지 다룬 다른 모든 언어(dotnet의 `Task`, java의 `CompletionStage`, node의
  `Promise`, cpp의 `async_result_t`)와 달리, `RequestOp::submit`은 callback 전용이다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`src/contracts/` + `src/lib.rs` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `src/lib.rs`(자유 함수); `contracts/core/`: `context.rs`, `routing_id.rs`, `utilities.rs` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging/`: `message.rs`, `received.rs`, `topic_message.rs`, `subscription_event.rs`, `operation_contracts.rs`, `operations.rs` |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets/`: `socket.rs`, `message_socket_contracts.rs`, `routed_socket_contracts.rs`, `pubsub_socket_contracts.rs`, `stream_socket.rs`, `socket_options.rs` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing/`: `poller.rs`(`Timer`도 여기 소유), `monitor.rs` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors/`: `errors.rs`, `results.rs` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
