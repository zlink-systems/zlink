---
title: "Go 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Python](../python/README.ko.md) | [다음: Rust](../rust/README.ko.md)
<!-- bindings-nav:end -->

# Go binding Core 11 공개 계약

> **이 장이 정의하는 것** — 현재 구현된 Go binding이 Core 11 raw C API 위에
> 제공하는 공개 type·ownership·오류 계약.

이 문서는 현재 구현된 Go binding의 공개 계약만 정의한다. 구현 전 설계나 다른
언어에만 있는 기능은 이 문서에 추가하지 않는다. 정확한 Go 식별자와 method
signature는 `bindings/go/contracts/`와 module root의 동일한 projection을 기준으로
확인한다.

| 절 | 다루는 내용 |
|---|---|
| [Module과 공개 package](#module과-공개-package) | import path, internal 경계, Core 11 raw 범위 |
| [공개 계약 범주](#공개-계약-범주) | 범주별 공개 개념 표 |
| [Context와 resource 수명](#context와-resource-수명) | Context/socket/monitor/poller/timer 소유·해제 규칙 |
| [Message와 ownership](#message와-ownership) | native storage, builder 경로별 ownership |
| [Socket operation](#socket-operation) | builder terminal signature, socket별 operation, ROUTER completion control |
| [Receive와 eventing](#receive와-eventing) | caller-provided receive 반환값, monitor·poller·timer |
| [Error contract](#error-contract) | `ZlinkError` interface와 concrete error type |
| [FFI와 package 경계](#ffi와-package-경계) | cgo include 경계, module proxy layout |
| [공개 계약에 포함하지 않는 것](#공개-계약에-포함하지-않는-것) | 범위 밖 기능 목록 |

## Module과 공개 package

Go module의 import path는 `zlink.systems/zlink/v11`이다. 일반적인 consumer는 module
root의 `zlink` package를 import한다. `zlink.systems/zlink/v11/contracts`는 같은
계약을 분류해 선언하는 public projection이며 root package가 이를 다시 export한다.

Runtime handle, cgo declaration, native struct, callback trampoline, request progress
pump와 buffer marshalling은 `internal/native`의 구현 세부사항이다. 이 타입과
package는 consumer 계약이 아니다.

- 현재 package 계약은 Core 11 raw C API만 투영한다.
- Context, Message, raw socket, monitor, poller, timer와 utility는 포함하지만 Spot, Actor, MeshNode와 service operation은 포함하지 않는다.
- Go module에는 message별 codec 등록 API도 없다.
- Message와 byte payload의 기본 경로는 binding이 제공하는 typed API를 사용한다.

## 공개 계약 범주

| 범주 | 주요 공개 개념 |
|------|----------------|
| Core | `Context`, `ContextOptions`, version/capability, `RoutingID`, utility |
| Messaging | `Message`, `Received`, `TopicMessage`, `SubscriptionEvent`, multipart helper |
| Sockets | Pair, PUB, SUB, DEALER, ROUTER, XPUB, XSUB, STREAM, typed options와 operation builder |
| Eventing | `SocketMonitor`, `MonitorEvent`, `MonitorStatus`, `Poller`, `PollEvent`, `Timer` |
| Errors | 함수군별 error type, result와 result code |

Socket 기능은 concrete socket type에 귀속한다. 모든 socket에 같은 method를 억지로
추가하지 않으며, raw Core capability가 없는 socket에는 해당 method를 제공하지
않는다.

## Context와 resource 수명

- `NewContext`가 만든 Context는 socket과 context-wide option의 owner다.
- Context를 `Close`하면 아직 열린 socket에도 종료가 전달된다.
- Context, socket, monitor, poller, timer와 utility resource는 호출자가 소유하며 사용이 끝나면 `Close` 또는 해당 종료 method를 호출한다.
- Close는 같은 resource에 반복해서 호출해도 이미 종료된 상태를 다시 해제하지 않는다.

Context option은 I/O thread와 socket default를 설정한다. Auto-HWM message unit은
Core contract가 요구하는 `uint64` storage로 전달된다. Go 호출자는 public `int`
method를 사용하지만 음수와 platform `uint64` 범위를 벗어난 값은 설정 전에
거부된다.

Poller가 등록한 socket과 timer는 해당 resource의 handle을 빌려 사용한다. 따라서
source를 `Close`하기 전에 poller에서 제거해야 하며, 하나의 poller에 대한 add,
modify, remove와 wait 호출은 호출자가 직렬화한다.

## Message와 ownership

`NewMessage`와 `NewMessageWithSize`는 Core가 소유하는 native message storage를
만든다. `NewMessage`의 입력 byte는 native storage로 복사된다. `Message.Data`는
message가 열려 있는 동안에만 유효한 native payload view를 반환한다. 수명을
message 밖으로 연장해야 할 때는 `Message.Bytes`가 snapshot을 만든다.

| Builder 경로 | ownership 규칙 |
|---|---|
| `Message` 추가 | submit 실패 시 caller message 보존, 성공 시 소비 |
| `MoveMessage` | submit 시점에 ownership을 명시적으로 이전 — 반환 뒤 caller가 원래 message를 재사용할 수 있다는 보장 없음 |
| `Bytes` | submit 중 caller slice를 읽고, submit 반환 뒤 slice를 보관하지 않음 |

수신 결과의 `Message` parts는 Go wrapper가 소유한다. `Received`, `TopicMessage`,
`SubscriptionEvent`와 request completion callback으로 전달된 parts는 사용 후
명시적으로 close한다. `Recv` 계열이 caller-provided output을 받는 경우 output
객체의 기존 parts를 정리한 뒤 새 native parts와 metadata를 채운다.

## Socket operation

### Builder terminal signature

Send, publish, request와 reply는 multipart builder를 사용한다. Builder는 payload와
flags를 모은 뒤 terminal `Submit`에서 한 번 실행된다. 같은 builder의 terminal
method를 두 번 호출하는 동작은 보장하지 않는다.

현재 구현의 terminal signature는 다음과 같다.

```go
// Message, MoveMessage와 Bytes는 payload part를 추가한다.
type SendSubmitOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
    Flags(SendFlags) SendSubmitOp
    Submit(context.Context) (bool, error)
}

// Request submit은 callback 또는 completion channel 중 하나를 선택한다.
type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Flags(SendFlags) RequestCallbackSubmitOp
    SubmitAsync(context.Context) (<-chan RequestReplyCompletion, error)
    Submit(context.Context, RequestReplyCallback) (bool, error)
}

// Received.Reply()가 만드는 reply builder는 성공 시 값 없이 error만 반환한다.
type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Flags(SendFlags) ReplySubmitOp
    Submit(context.Context) error
}
```

### DontWait와 오류 분류

- `SendFlagsDontWait`는 blocking을 피한다.
- temporary backpressure의 정상 결과는 `false, nil`이고, 연결 단절·invalid argument·Core termination 같은 실제 실패는 함수군별 error로 반환한다.
- Non-blocking receive의 no-data만 `false, nil`로 표현한다.
- 그 밖의 receive 실패는 error다.

### Socket별 operation

| Socket | 제공 operation |
|---|---|
| PAIR, DEALER | `Send` |
| PUB, XPUB | `Publish` |
| ROUTER, STREAM | 대상 routing id를 받는 send operation |
| DEALER, ROUTER | request operation — ROUTER는 수신한 request metadata가 있으면 그 metadata로 reply operation을 만든다 |
| STREAM | raw TCP packet callback과 caller-provided receive |

| Socket | 수신 표면 |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | `Received` 저장소를 채우는 `Recv` |
| SUB, XSUB | `TopicMessage` 저장소를 채우는 `Subscribe` |

Core의 part 함수는 이 aggregate 표면을 구현하기 위한 internal substrate이며 Go
public method로 노출하지 않는다.

### ROUTER completion control

ROUTER는 Core completion connection의 opaque multipart control record도 제공한다.
`OnCompletionControl` handler에는 source routing id와 payload parts를 담은
`Received`가 전달되며 handler가 parts를 닫거나 소비해야 한다.
`CompletionControl(peerRID)` builder는 지정한 peer에 record를 보내고
`SendFlagsNone` 이외의 flag는 거부한다.

## Receive와 eventing

Caller-provided receive method는 `(bool, error)`를 반환한다. `bool`이 `false`이면
`RecvFlagsDontWait`에서 읽을 데이터가 없었다는 뜻이며 error는 nil이다. `bool`이
`true`이면 output에 하나 이상의 결과가 채워졌다. 실제 실패는 `*RecvError`다.

Socket monitor는 typed event mask로 열고 `MonitorEvent`, `MonitorStatus`를 제공한다.
Core 11의 각 monitor event mask와 delivered event value는 대응하는 typed constant로
제공한다. `MonitorEventMask`는 monitor를 열 때 사용하고 `MonitorEventType`은
수신한 `MonitorEvent.Event`를 검사할 때 사용한다.
Poller는 socket, file descriptor와 timer source의 readiness를 `PollEvent`로 보고한다.
Timer는 interval event를 poller 또는 직접 receive하는 데 사용한다. Monitor, poller와
timer의 callback 또는 event result는 native callback thread를 public consumer callback
실행 위치로 노출하지 않는다.

## Error contract

모든 함수군별 error는 `error`를 구현하고 다음 public interface를 만족한다.

```go
type ZlinkError interface {
    error
    Code() int
    InternalErrno() int
}
```

- 현재 concrete error type은 `SubmitError`, `RequestError`, `RecvError`, `HandlerError`, `CloseError`, `BindError`, `ConnectError`와 `ConfigError`다.
- `Code()`는 함수군의 Core result code를 반환하고 `InternalErrno()`는 native 실패 원인을 반환한다.
- `Unwrap()`을 통한 `errors.Is`도 지원한다.
- `NativeErrno` field나 `NativeErrno()` alias는 공개 계약이 아니다.

- Context가 terminal method 호출 전에 이미 취소되었거나 deadline을 넘긴 경우에는 `context.Canceled` 또는 `context.DeadlineExceeded`를 반환한다.
- 이 표준 error를 함수군별 Core error로 변환하지 않는다.
- Native submit이 접수된 뒤의 request completion 결과는 `RequestReplyCompletion` 또는 callback의 `RequestResult`로 전달된다.
- Submit 반환 규칙과 completion 뒤 cancellation의 통합 정책은 Go·Rust submit draft 승인 결과가 정식 기준이 될 때까지 별도 review 항목으로 남긴다.

## FFI와 package 경계

Go cgo bridge의 include path는 package 안의 `include/`로 고정한다. Repository의
`core/include`를 package consumer가 직접 읽지 않는다. `bindings/go/tests/raw-core11-
allowlist.json`은 header file set, SHA-256, cgo raw symbol과 local callback helper를
machine-readable 형태로 고정한다. `zlink/service/` 및 이전 service symbol은
allowlist에 없다.

Module package는 다음 file proxy layout을 사용한다.

```text
zlink.systems/zlink/v11/@v/v11.1.0.info
zlink.systems/zlink/v11/@v/v11.1.0.mod
zlink.systems/zlink/v11/@v/v11.1.0.zip
```

지원 platform runtime은 module의 `native/<platform>/` 아래에 포함한다. Package
consumer는 `replace`와 repository `core/build` 없이 module cache의 runtime을
사용해야 한다.

## 공개 계약에 포함하지 않는 것

- Spot, Actor, MeshNode와 service operation
- Core 10 compatibility alias와 service header
- private cgo type, native pointer, callback userdata와 progress pump
- message별 codec registry 또는 호출자 raw encode/decode 우회
- `NativeErrno`와 이전 module path `zlink.systems/zlink`

GoDoc과 process sample의 현재 검증 진입점은 `bindings/go/README.godoc.md`,
`bindings/go/tests/run_tests.sh`와 `bindings/go/samples/run_samples.sh`에 기록한다.
이 문서의 public contract 변경은 먼저 common binding spec과 관련 draft의 review
상태를 확인한 뒤 반영한다.
