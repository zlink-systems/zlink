한국어 | [English](README.en.md)

[Go 바인딩 스펙](../../spec/go/README.ko.md) · [Go 바인딩 가이드](../../guide/go/index.ko.md)

# Go bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/go/reference/`에 있음)이 아니다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. 지금까지의 모든 wrapper binding과 마찬가지로 이 트리도 6개가
아니라 5개 category다 — `contracts/`엔 `service.go`가 없다. SPOT/Actor는 framework
계층에만 존재한다.

**`contracts/` 자체가 re-export shim이다** — `contracts/*.go`의 모든 타입은 `type X =
impl.X` alias이고 모든 함수/상수는 `internal/native`(export되지 않은 패키지)를
가리키는 `var`/`const`다. 이 트리에 문서화된 실제 method signature는 `contracts/`가
아니라 `internal/native/*.go`에서 읽은 것이다, alias 파일 자체엔 member가 없기
때문이다.

아래 모든 category에 적용되는 Go 고유 참고:

- **socket 생성이 `Context`의 method다**(`ctx.PairSocket()`, `ctx.DealerSocket()`, ...),
  package 최상위 자유 함수가 아니다 — 지금까지 다룬 wrapper binding 중 유일하게
  `Context` method이지 top-level factory나 static-facade method가 아니다.
- **Dealer/Router/Stream/Sub용 타입별 option facade가 없다** — 이들의 특정
  option(`SetProbe`, `SetWeight`, `RequestTimeout`, `SetMandatory`, `SetHandover`,
  `SetConnectRoutingID`, `SetNotify`/`Notify`, `TopicsCount`)은 socket type 자체에
  직접 method로 선언돼 있다. `CommonSocketOptions`(`.CommonOptions()`로)와
  `PubSocketOptions`(`.PubOptions()`로)만 별도 accessor 객체로 존재한다 — 지금까지
  다룬 다른 모든 언어는 모든 socket type에 자신만의 명명된 option facade를 준다.
- **`RequestOp`엔 Go-channel async 경로와 callback 경로가 둘 다 있다** —
  `SubmitAsync(ctx) (<-chan RequestReplyCompletion, error)`와 `Submit(ctx, callback)
  (bool, error)`가 나란히 있다 — callback 전용인 rust나, Task/CompletionStage/Promise/
  `async_result_t`를 쓰는 dotnet/java/node/cpp와 다르다. 모든 builder의 terminal
  `Submit`/`SubmitAsync`는 취소를 위해 첫 인자로 `context.Context`를 받는다, 다른
  어떤 언어의 operation builder도 이러지 않는다.
- **`ZlinkError`는 Go `interface`다**, struct나 enum이 아니다 — 각 typed error는
  `Error() string`, `Code() int`, `InternalErrno() int`, `Unwrap() error`(표준
  라이브러리 `errors.Is`/`errors.As` 연동 지점)를 구현한다, 자신의 doc comment에
  따르면.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`contracts/*.go` alias → `internal/native/*.go` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `contracts/core.go` → `internal/native/context.go`, `utility.go` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging.go` → `internal/native/message.go`, `received.go`, `topic_message.go`, `subscription_event.go`, `operations.go`, `request_reply_types.go` |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets.go` → `internal/native/socket_core.go`, `socket_types.go`, `socket_options.go`, `connection_socket.go`, `socket_direct.go`, `socket_routed.go`, `socket_publish.go`, `socket_subscribe.go`, `socket_completion_control.go` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing.go` → `internal/native/monitor.go`, `poller_timer.go` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors.go` → `internal/native/error.go`, `result_codes.go` |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
