---
title: "Go 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Python](../python/README.ko.md) | [다음: Rust](../rust/README.ko.md)
<!-- bindings-nav:end -->

# Go binding Core 공개 계약

> **이 장이 정의하는 것** — Go binding이 Core raw C API 위에
> 제공하는 공개 type·ownership·오류 계약.

이 문서는 Go binding의 공개 계약을 정의한다. 다른 언어에만 있는 기능은 이 문서의
계약이 아니다. 정확한 Go 식별자와 method
signature는 `bindings/go/contracts/`와 module root의 동일한 projection을 기준으로
확인한다.

| 절 | 다루는 내용 |
|---|---|
| [Module과 공개 package](#module과-공개-package) | import path, internal 경계, Core raw 범위 |
| [공개 계약 범주](#공개-계약-범주) | 범주별 공개 개념 표 |
| [Context와 resource 수명](#context와-resource-수명) | Context/socket/monitor/poller/timer 소유·해제 규칙 |
| [Byte HWM과 Auto-HWM](#byte-hwm과-auto-hwm) | Go `uint64`와 Core `uint64_t` byte HWM의 매핑 |
| [Message와 ownership](#message와-ownership) | native storage, builder 경로별 ownership |
| [Socket operation](#socket-operation) | builder terminal signature, socket별 operation |
| [Receive와 eventing](#receive와-eventing) | caller-provided receive 반환값, monitor·poller·timer |
| [Receive flow state](#receive-flow-state) | receive-flow 상태 타입, setter와 monitor 표면 |
| [Error contract](#error-contract) | `ZlinkError` interface와 concrete error type |
| [FFI와 package 경계](#ffi와-package-경계) | cgo include 경계, module proxy layout |
| [공개 계약에 포함하지 않는 것](#공개-계약에-포함하지-않는-것) | 범위 밖 기능 목록 |

## Module과 공개 package

Go module의 import path는 `zlink.systems/zlink`이다. 일반적인 consumer는 module
root의 `zlink` package를 import한다. `zlink.systems/zlink/contracts`는 같은
계약을 분류해 선언하는 public projection이며 root package가 이를 다시 export한다.

Runtime handle, cgo declaration, native struct, completion drain state와 buffer
marshalling은 `internal/native`의 구현 세부사항이다. 이 타입과 package는 consumer
계약이 아니다.

- Package 계약은 Core raw C API를 투영한다.
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

Context option은 I/O thread와 socket default를 설정한다. Auto-HWM memory limit과
Core budget은 public `uint64` method로 받고 Core의 `uint64` byte 값으로 손실 없이
전달한다. Profile은 `AutoHwmProfile`로 전달한다. Context는
`CoreHwmBudgetSnapshot() (CoreHwmBudgetSnapshot, error)`와
`ResetCoreHwmBudgetMetrics() error`를 제공한다.

Poller가 등록한 socket과 timer는 해당 resource의 handle을 빌려 사용한다. 따라서
source를 `Close`하기 전에 poller에서 제거해야 하며, 하나의 poller에 대한 add,
modify, remove와 wait 호출은 호출자가 직렬화한다.

`(*Poller).AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error`,
`(*Poller).ModifyMonitor(monitor *SocketMonitor, events PollEventFlag) error`,
`(*Poller).RemoveMonitor(monitor *SocketMonitor) error`는 socket monitor를 poller source로 등록·수정·제거한다
(공통 spec "`Poller`의 monitor source"). 기존 `AddSocket/ModifySocket/RemoveSocket`(`SocketTarget`)도 monitor를
수용한다. monitor에는 `POLLIN`만 유효하고 다른 bit는 typed `ConfigResult` `InvalidArgument`로 거절한다. ready 뒤
`monitor.Recv(RecvFlagsDontWait)`로 drain하며 poll event는 socket과 같은 slot/source kind로 보고한다.

## Byte HWM과 Auto-HWM

[HWM](../../../../core/doc/spec/core/glossary.ko.md#hwm)(queue의 byte 보관량을 제한하는 기준)의 계산과 queue admission은 Core가 담당한다. Go 바인딩은
`SetSendHighWaterMark(uint64)`와 `SetReceiveHighWaterMark(uint64)`의 값을 Core의
8-byte `uint64_t` option으로 손실 없이 전달한다. Getter도 Core의 전체 범위를
`uint64`로 반환한다. 값 `0`은 무제한이다.

Context memory limit·Core budget의 byte 값과 profile option을 Core에 전달한다.
계산·수동 override·admission은 [Core HWM 계산·admission](../README.ko.md#hwm-계산과-admission)을 따른다.

입력 우선순위는 수동 Core budget, 명시 memory limit, Go runtime에 설정된 유한한
memory limit hint, Core fallback 순서다. 앞의 두 값을 지정하면 runtime hint를 자동
감지하지 않는다. Binding은 hint와 Core hard limit을 직접 결합하지 않는다. 명시 입력이
Core가 감지한 finite hard limit보다 크면 `EINVAL`에 대응하는 기존 config error를 그대로
전달하고 clamp하지 않는다.

`MonitorStatus`의 planned, applied, deferred HWM과 in-flight 사용량은 `uint64` byte다.
Pending message count는 별도 진단값이며 slot·message-unit·size-cap·connection-bucket
property는 제공하지 않는다.

## Message와 ownership

`NewMessage`와 `NewMessageWithSize`는 Core가 소유하는 native message storage를
만든다. `NewMessage`의 입력 byte는 native storage로 복사된다. `Message.Data`는
message가 열려 있는 동안에만 유효한 native payload view를 반환한다. 수명을
message 밖으로 연장해야 할 때는 `Message.Bytes`가 snapshot을 만든다.

| Builder 경로 | ownership 규칙 |
|---|---|
| `Message` 추가 | Core admission 전 실패하면 caller message를 보존하고, admission에 성공하면 소비 |
| `MoveMessage` | `Submit` 시점에 ownership을 명시적으로 이전 — 반환 뒤 caller가 원래 message를 재사용할 수 있다는 보장 없음 |
| `Bytes` | `Submit` 중 caller slice를 읽고, `Submit` 반환 뒤 slice를 보관하지 않음 |

수신 결과의 `Message` parts는 Go wrapper가 소유한다. `Received`, `TopicMessage`,
`SubscriptionEvent`와 successful request result로 전달된 parts는 사용 후
명시적으로 close한다. `Recv` 계열이 caller-provided output을 받는 경우 output
객체의 기존 parts를 정리한 뒤 새 native parts와 metadata를 채운다.

수신 회계와 결과 수명의 경계는 [공통 수신 ownership 계약](../README.ko.md#receive-ownership)을 따른다.

## Socket operation

Send, publish, request와 reply는 multipart builder를 사용한다. Builder는 payload와
해당 operation에 허용된 option을 모은 뒤 terminal `Submit`에서 한 번 실행된다.
같은 builder를 두 번 submit하면 두 번째 completion은 state error로 끝난다.

Send와 request는 `Submit(context.Context)`가 Core `DONTWAIT` completion을 기다린다. Reply는
호출 진입 전에 Context를 확인하고, native 호출 뒤 admission 대기는 socket `SNDTIMEO`가
소유한다. Publish만 별도 `PublishOp`의 `Flags(SendFlags)`를 제공한다. Exact interface는
[Pull completion 공개 계약](#pull-completion-공개-계약)에 둔다.

### Context와 오류 분류

- 호출 전에 취소됐거나 deadline이 지난 Context는 native operation을 시작하지 않고
  `context.Canceled` 또는 `context.DeadlineExceeded`로 실패한다.
- Successful submit 뒤 Context cancellation은 caller wait만 끝낸다. Native completion은 runtime이
  계속 drain하며 request result를 다시 전달하지 않는다.
- 같은 socket에 동시에 제출한 multipart record의 part는 서로 섞이지 않는다. Public API가
  caller message를 보존하는 경우 binding staging으로 복구하며 재전송 queue는 만들지 않는다.
- Non-blocking receive의 no-data만 `false, nil`로 표현하고 다른 receive 실패는 error다.

### Socket별 operation

| Socket | 제공 operation |
|---|---|
| PAIR | `Send()` |
| PUB, XPUB | 별도 `PublishOp`를 반환하는 `Publish(topic)` |
| DEALER | `Send()`, `Request()` |
| ROUTER | `SendTo(RoutingID)`, `Request(RoutingID)`, `Reply(RoutingID, ReplyToken)` |
| STREAM | `SendTo(RoutingID)`, RAW `Recv`, PACKET `RecvPacket` |

| Socket | 수신 API |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | `Received` 저장소를 채우는 `Recv` |
| SUB, XSUB | `TopicMessage` 저장소를 채우는 `Subscribe` |

Core의 part 함수는 이 multipart 수신 API를 구현하기 위한 internal 기반이며 Go
public method로 노출하지 않는다.

## Receive와 eventing

Caller-provided receive method는 `(bool, error)`를 반환한다. `bool`이 `false`이면
`RecvFlagsDontWait`에서 읽을 데이터가 없었다는 뜻이며 error는 nil이다. `bool`이
`true`이면 output에 하나 이상의 결과가 채워졌다. 실제 실패는 `*RecvError`다.

`Received`와 `TopicMessage`는 part, Routing ID, `ReplyToken`, topic과 multipart framing을
보존한다. 결과를 정리하는 Go API는 [Message와 ownership](#message와-ownership)을 따른다.

Socket monitor는 typed event mask로 열고 `MonitorEvent`, `MonitorStatus`를 제공한다.
Core의 각 monitor event mask와 delivered event value는 대응하는 typed constant로
제공한다. `MonitorEventMask`는 monitor를 열 때 사용하고 `MonitorEventType`은
수신한 `MonitorEvent.Event`를 검사할 때 사용한다.
`OpenSocketMonitor(socket, options...)`는 `MonitorEventMask`와
`MonitorHwmBytes(uint64)`를 `MonitorOpenOption`으로 받는다. Event mask가 없으면 모든
event를 선택하고 여러 mask는 OR로 합친다. `MonitorHwmBytes(0)`은 Core 기본값을
선택하며 양수는 변환하지 않고 정확한 byte HWM으로 전달한다. 같은 옵션을 여러 번
지정하면 호출 순서상 마지막 값이 적용된다.

`MonitorStatus`는 pending message count와 별도로 `SndPendingBytes`와 `RcvPendingBytes`를
노출한다. `CoreHwmBudgetSnapshot`은 ABI version/size, configured/runtime/resolved memory
limit, configured/effective budget, planned/applied/manual-reserved HWM, Core queue/application/
current/peak/provisional accounted byte, completion current/peak/pending과 total messaging byte,
monitor/instance aggregate, application/completion queue count,
`OutstandingApplicationLeaseCount`, `RetiredQueueCount`, `DeferredOriginCreditBytes`,
oversize·blocked·aggregate flag, `BudgetGeneration`과 `MeasurementEpoch`을 정확한
`uint64`/boolean 값으로 제공한다. `ApplicationAccountedBytes`와 위 세 owner-lifecycle
필드는 ABI 예약 필드이며 항상 `0`이다. Reset은 current·pending·queue count를 유지하고 두
peak를 current로 재기준화하며 epoch counter를 0으로 만든 뒤 `MeasurementEpoch`을
증가시킨다. ABI version/size 불일치는 unsupported error다.
Poller는 socket, file descriptor와 timer source의 readiness를 `PollEvent`로 보고한다.
Timer는 interval event를 poller 또는 직접 receive하는 데 사용한다. Monitor와 timer의
public pull method가 event와 fire count를 반환한다.

## Receive flow state

`ReceiveFlowState` 타입은 `ReceiveFlowRunning`, `ReceiveFlowPaused`를 제공한다.
`SetReceiveFlowState(ReceiveFlowState) error`는 성공 시 `nil`, 실패 시 native result와
errno를 담은 `*ConfigError`를 반환한다. Nil 또는 닫힌 handle은 native 호출 전에
`ConfigInvalidHandle`로 거부한다.
상태·결과·monitor 투영은 [공통 receive-flow 계약](../README.ko.md#receive-flow-projection)을 따른다.

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

- Context가 `Submit` 호출 전에 이미 취소되었거나 deadline을 넘겼으면 해당 표준 error를
  반환한다. 이 error를 함수군별 Core error로 변환하지 않는다.
- Native request가 수용된 뒤의 reply와 실패는 `Submit(context.Context)`의
  `([]*Message, error)` 결과로 전달한다.

## FFI와 package 경계

Go cgo bridge의 include path는 package 안의 `include/`로 고정한다. Repository의
`core/include`를 package consumer가 직접 읽지 않는다. `bindings/go/tests/raw-core11-
allowlist.json`은 header file set, SHA-256, cgo raw symbol과 local native helper를
machine-readable 형태로 고정한다. `zlink/service/` 및 이전 service symbol은
allowlist에 없다.

Module package는 다음 file proxy layout을 사용한다. `<version>`은
[Core release metadata](../../../../VERSION)의 release 버전이다.

```text
zlink.systems/zlink/@v/v<version>.info
zlink.systems/zlink/@v/v<version>.mod
zlink.systems/zlink/@v/v<version>.zip
```

지원 platform runtime은 module의 `native/<platform>/` 아래에 포함한다. Package
consumer는 `replace`와 repository `core/build` 없이 module cache의 runtime을
사용해야 한다.

## 공개 계약에 포함하지 않는 것

- Spot, Actor, MeshNode와 service operation
- Core 10 compatibility alias와 service header
- private cgo type와 native pointer
- message별 codec registry 또는 호출자 raw encode/decode 우회
- `NativeErrno`

GoDoc과 process sample의 검증 진입점은 `bindings/go/README.godoc.md`,
`bindings/go/tests/run_tests.sh`와 `bindings/go/samples/run_samples.sh`에 기록한다.

## Pull completion 공개 계약

Go package 정보는 [배포 metadata](../../../go/go.mod)를, Core ABI 버전은 [Core release metadata](../../../../VERSION)를 따른다.

Go는 호출 goroutine에서 완료를 기다리는 `Submit(context.Context)` terminal 하나를 제공한다.
Caller wait 취소 입력은 `context.Context`이고 request의 취소 결과는 `(nil, ctx.Err())`다.

Native completion ID·`user_context`·raw drain은 public API에 노출하지 않는다.
제출 결과는 [공통 결과 투영](../README.ko.md#submit-result-projection)을, 완료 합류·수명과
`PollCompletion`의 진행 조건은 [비동기 실행 모델](../async-execution-model.ko.md)을 따른다.

`ReplyToken`은 package 내부에서 ROUTER REQUEST receive가 struct literal로 만든다. Zero value는
invalid이며 owner pointer와 opaque value를 함께 비교한다. `StreamPacket`의 zero value는 empty
reusable output이다. Publish는 send와 별도 `PublishOp`에서 기존 flags와 synchronous submit
결과를 유지한다.
Token은 raw accessor, ordering, serialization과 `Close`를 제공하지 않는다.
같은 output의 concurrent recv는 invalid-state다. Message pointer는 다음 recv 진입이나 `Close()`
전까지만 유효하다. `SetReceiveMode`는 첫 bind/connect 전에 `StreamReceiveRaw`·
`StreamReceivePacket`만 받고 `StreamReceiveUnspecified`를 거부한다.

### Public interface

```go
type SendOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
}

type SendSubmitOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
    Submit(context.Context) error
}

type RequestOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
}

type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Submit(context.Context) ([]*Message, error)
}

type ReplyOp interface {
    Message(*Message) ReplySubmitOp
}

type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Submit(context.Context) error
}

type PublishOp interface {
    Message(*Message) PublishSubmitOp
    MoveMessage(*Message) PublishSubmitOp
    Bytes([]byte) PublishSubmitOp
}

type PublishSubmitOp interface {
    Message(*Message) PublishSubmitOp
    MoveMessage(*Message) PublishSubmitOp
    Bytes([]byte) PublishSubmitOp
    Flags(SendFlags) PublishSubmitOp
    Submit(context.Context) (bool, error)
}

type ReplyToken struct {
    owner *replyTokenOwner
    value uint64
}

func (r *Received) ReplyToken() (ReplyToken, bool)
func (s *RouterSocket) Reply(
    rid RoutingID, token ReplyToken) ReplyOp

type StreamReceiveMode int32

const (
    StreamReceiveUnspecified StreamReceiveMode = iota
    StreamReceiveRaw
    StreamReceivePacket
)

type StreamPacket struct { /* unexported reusable state */ }

func (p *StreamPacket) Empty() bool
func (p *StreamPacket) RoutingID() RoutingID
func (p *StreamPacket) HasRoutingID() bool
func (p *StreamPacket) Header() *Message
func (p *StreamPacket) Body() *Message
func (p *StreamPacket) Close() error

func (s *StreamSocket) RecvPacket(
    out *StreamPacket, flags RecvFlags) (bool, error)
func (s *StreamSocket) ReceiveMode() (StreamReceiveMode, error)
func (s *StreamSocket) SetReceiveMode(StreamReceiveMode) error
```

Operation 시작 signature는 PAIR `Send() SendOp`, DEALER `Send() SendOp`·
`Request() RequestOp`, ROUTER `SendTo(RoutingID) SendOp`·
`Request(RoutingID) RequestOp`·`Reply(RoutingID, ReplyToken) ReplyOp`, STREAM
`SendTo(RoutingID) SendOp`다. `Received.Send()`와 `Received.Reply()`는 source target과 token을
capture한다. `PubSocket.Publish(topic)`과 `XPubSocket.Publish(topic)`은 `PublishOp`를 반환한다.
`Received.Reply()`는 DATA envelope에서 호출하면 state error를 반환한다.

Public Go surface에는 send/request/reply `Flags`, `RequestSyncSubmitOp`, completion channel,
`RequestReplyCompletion`, `Received.RequestSeq`, STREAM/monitor/timer callback, pair/generation
member, `SocketMonitor.OnEvent`, `Timer.OnFire`가 없다. `RoutedSendOp`와
`RoutedSendSubmitOp`도 public type이 아니다.

Monitor는 `Recv(RecvFlags) (*MonitorEvent, error)`·`Status()`·`Close()`를, timer는
`Start(intervalNs, repeatCount uint64)`·`Stop()`·`Recv() (uint64, bool, error)`·`Close()`를
제공한다. Monitor DONTWAIT no-data는 `*RecvError`의 `NO_DATA`로 구분한다. Native header mirror의
pending option은 `ZLINK_OPT_PENDING_MAX_MSGS`와 `ZLINK_OPT_PENDING_MAX_BYTES`다.
Monitor event의 `ConnectionID`는 진단과 correlation에만 사용하며 send·reply target이나
reconnect fence로 사용하지 않는다.
Pending native option은 public high-level option method를 추가하지 않는다.

## 구현 및 contract test 검증 요구

Public Go interface, 반환값과 poller event만으로 다음을 확인한다. 각 항목은 contract test
하나로 이어진다.

**Operation과 완료**

- Send·request가 `Submit(context.Context)` terminal 하나를 제공하고 request success는
  `([]*Message, nil)`, non-OK completion은 `(nil, typed request error)`를 반환한다.
- Go·Python과 공유하지 않는 send flags는 `PublishSubmitOp`에만 있으며 publish submit은
  `(bool, error)` 결과를 유지한다.
- 완료·cancellation·poller의 공통 관측은
  [실행 모델 검증 요구](../async-execution-model.ko.md#7-구현-및-contract-test-검증-요구)를 따른다.

**ReplyToken과 STREAM**

- `Received.ReplyToken()`은 ROUTER REQUEST에서 valid token과 `true`, DATA에서 zero token과
  `false`를 반환한다.
- Zero token과 다른 owner token은 reply builder 생성 전에 실패한다.
- Zero-value `StreamPacket`은 empty이며 `RecvPacket()`의 no-data·오류와 `Close()` 뒤에도 empty
  accessor 결과를 유지하고 재사용할 수 있다.

**Pull eventing**

- Monitor·timer recv는 handler 없이 event와 fire count를 반환하고 각각의 no-data 결과를
  구분한다.
