---
title: "Go 바인딩 공개 계약"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Python](../python/README.ko.md) | [다음: Rust](../rust/README.ko.md)
<!-- bindings-nav:end -->

# Go binding Core 0.12.0 공개 계약

> **이 장이 정의하는 것** — 현재 구현된 Go binding이 Core 0.12.0 raw C API 위에
> 제공하는 공개 type·ownership·오류 계약.

이 문서는 현재 구현된 Go binding의 공개 계약만 정의한다. 구현 전 설계나 다른
언어에만 있는 기능은 이 문서에 추가하지 않는다. 정확한 Go 식별자와 method
signature는 `bindings/go/contracts/`와 module root의 동일한 projection을 기준으로
확인한다.

| 절 | 다루는 내용 |
|---|---|
| [Module과 공개 package](#module과-공개-package) | import path, internal 경계, Core 0.12.0 raw 범위 |
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

Runtime handle, cgo declaration, native struct, callback trampoline과 buffer
marshalling은 `internal/native`의 구현 세부사항이다. 이 타입과 package는 consumer
계약이 아니다.

- 현재 package 계약은 Core 0.12.0 raw C API만 투영한다.
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

## Byte HWM과 Auto-HWM

HWM의 계산과 queue admission은 Core가 담당한다. Go 바인딩은
`SetSendHighWaterMark(uint64)`와 `SetReceiveHighWaterMark(uint64)`의 값을 Core의
8-byte `uint64_t` option으로 손실 없이 전달한다. Getter도 Core의 전체 범위를
`uint64`로 반환한다. 값 `0`은 무제한이다.

Context memory limit·Core budget은 byte 값으로, profile은 정식 profile option으로 Core에
전달한다. Core가 profile 비율을 정확히 한 번 적용하고 physical directional queue별 planned
byte HWM을 계산한다.
Caller가 방향별 HWM을 설정하면 그 방향은 수동 override가 되어 Auto-HWM
재계산에서 제외된다.

입력 우선순위는 수동 Core budget, 명시 memory limit, Go runtime에 설정된 유한한
memory limit hint, Core fallback 순서다. 앞의 두 값을 지정하면 runtime hint를 자동
감지하지 않는다. Binding은 hint와 Core hard limit을 직접 결합하지 않는다. 명시 입력이
Core가 감지한 finite hard limit보다 크면 `EINVAL`에 대응하는 기존 config error를 그대로
전달하고 clamp하지 않는다.

실제 pipe에 쌓인 accounted byte가 applied HWM에 도달하면 Core가
backpressure를 결정한다. Go 바인딩은 message 수를 다시 세지 않으며 Core result를
기존 operation과 error 계약으로 전달한다. `MonitorStatus`의 planned, applied,
deferred HWM과 in-flight 사용량은 `uint64` byte다. Pending message count는 표시용
진단이며 slot·message-unit·size-cap·connection-bucket property는 제공하지 않는다.

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
`SubscriptionEvent`와 request completion channel로 전달된 parts는 사용 후
명시적으로 close한다. `Recv` 계열이 caller-provided output을 받는 경우 output
객체의 기존 parts를 정리한 뒤 새 native parts와 metadata를 채운다.

일반 `Recv`와 `Subscribe`는 part를 dequeue할 때 Core queue credit을 즉시
반환한다. 따라서 일반 application 수신 결과의 수명은 HWM accounting에 남지
않는다. Framework backend만 아래의 retained aggregate 경로를 명시적으로
선택한다.

## Socket operation

### Builder terminal signature

Send, publish, request와 reply는 multipart builder를 사용한다. Builder는 payload와
해당 operation에 허용된 option을 모은 뒤 terminal `Submit`에서 한 번 실행된다.
같은 builder를 두 번 submit하면 두 번째 completion은 state error로 끝난다.

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

// DEALER Send와 ROUTER SendTo의 HWM-managed routed submit이다.
type RoutedSendSubmitOp interface {
    Message(*Message) RoutedSendSubmitOp
    MoveMessage(*Message) RoutedSendSubmitOp
    Bytes([]byte) RoutedSendSubmitOp
    Submit(context.Context) <-chan error
}

// DEALER/ROUTER request의 단일 terminal은 completion channel을 반환한다.
type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Submit(context.Context) <-chan RequestReplyCompletion
}

type RequestReplyCompletion struct {
    Result RequestResult
    Parts  []*Message
    Err    error
}

// Received.Reply()가 만드는 reply builder는 성공 시 값 없이 error만 반환한다.
type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Flags(SendFlags) ReplySubmitOp
    Submit(context.Context) error
}
```

### DontWait와 오류 분류

- PAIR·PUB·XPUB·STREAM과 reply의 기존 one-shot submit은 허용된 builder에서
  `SendFlagsDontWait`를 사용할 수 있다.
- `ReplySubmitOp.Submit(ctx)`는 completion channel을 반환하지 않는 동기 one-shot이다.
  Terminal reply 또는 error reply를 HWM 없는 completion lane에 native 호출 한 번으로
  제출한다. HWM backpressure는 reply 결과가 아니며 `NOT_CONNECTED`, `TERMINATED`,
  `INVALID_ARGUMENT`와 그 밖의 non-HWM submit 실패는 즉시 `*SubmitError`를 `error`로
  반환한다.
- DEALER `Send`, ROUTER `SendTo`와 DEALER/ROUTER `Request`의 managed routed
  builder는 flags, callback, `SubmitAsync` 호환 terminal을 제공하지 않는다.
  `Submit(ctx)`는 validation·payload snapshot·target 선택을 마친 뒤 completion
  channel을 반환하며 HWM credit을 기다리느라 caller를 막지 않는다. 선택한
  `(RID, transport pair, generation)`은 operation 동안 바꾸지 않는다. 해당 연결이
  detach되면 다른 연결로 재선택하지 않고 operation을 error로 끝낸다. 대기 중 같은
  socket의 다른 target은 계속 진행할 수 있다.
- Socket runtime은 비동기 operation을 받기 전에 Core routed-target readiness handler를
  장기 등록한다. Operation은 최초 시도 전에 정확한 `(socket, RID, transport pair ID,
  generation)` key, completion channel과 complete record를 pending에 넣고 같은 target에
  `DONTWAIT`로 시도한다. Callback은 해당 key만 ready로 표시하며 native retry는 callback
  밖의 pump가 수행한다. Pair generation이 다른 event는 stale wake로 무시한다.
- 같은 native handle의 outbound 경로는 complete multipart의 첫 part부터 `FINAL`까지 한
  시도만 보호하는 짧은 attempt gate를 공유하고 readiness 대기 전에 반환한다.
- Routed send channel은 complete record가 Core에 수용되면 `nil`, 실패하면 error
  하나를 전달한 뒤 닫힌다. Request channel도 reply 또는 submit 실패·timeout·
  disconnect·socket close·context cancellation 중 하나를 정확히 한 번 전달한 뒤
  닫힌다. Request 성공 시 `Parts`는 caller가 닫으며, 실패는 `Err`와 대응하는
  `Result`에 담긴다. Context cancellation은 `Err`에 `context.Canceled` 또는
  `context.DeadlineExceeded`를 담는다.
- Routed send의 absolute deadline은 `Submit` 시점의 socket `SNDTIMEO`와 context
  deadline 중 이른 값이다. Request는 builder `Timeout`, 값이 없으면 socket request
  timeout, 그리고 context deadline 중 이른 값으로 정한다. HWM 대기는 이 deadline을
  연장하지 않는다.
- 같은 socket에서 동시에 submit한 complete record들의 payload part는 서로 섞이지
  않는다.
- `bool`을 반환하는 기존 one-shot send에서 temporary backpressure의 정상 결과는
  `false, nil`이고, 연결 단절·invalid argument·Core termination 같은 실제 실패는
  함수군별 error로 반환한다. Error만 반환하는 reply에는 이 `false, nil` 규칙을
  적용하지 않는다.
- Non-blocking receive의 no-data만 `false, nil`로 표현한다.
- 그 밖의 receive 실패는 error다.

### Socket별 operation

| Socket | 제공 operation |
|---|---|
| PAIR | one-shot `Send` |
| PUB, XPUB | `Publish` |
| DEALER | completion channel을 반환하는 managed routed `Send` |
| ROUTER | 대상 routing id를 받고 completion channel을 반환하는 managed routed `SendTo` |
| STREAM | 대상 routing id를 받는 one-shot send operation |
| DEALER, ROUTER | request operation — ROUTER는 수신한 request metadata가 있으면 그 metadata로 reply operation을 만든다 |
| STREAM | raw TCP packet callback과 caller-provided receive |

| Socket | 수신 API |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | `Received` 저장소를 채우는 일반 `Recv`, Framework backend 전용 `RecvRetained` |
| SUB, XSUB | `TopicMessage` 저장소를 채우는 일반 `Subscribe`, Framework backend 전용 `SubscribeRetained` |

Core의 part 함수는 이 multipart 수신 API를 구현하기 위한 internal substrate이며 Go
public method로 노출하지 않는다.

## Receive와 eventing

Caller-provided receive method는 `(bool, error)`를 반환한다. `bool`이 `false`이면
`RecvFlagsDontWait`에서 읽을 데이터가 없었다는 뜻이며 error는 nil이다. `bool`이
`true`이면 output에 하나 이상의 결과가 채워졌다. 실제 실패는 `*RecvError`다.

`RecvRetained(out, flags)`와 `SubscribeRetained(out, flags)`는 일반 수신과 같은
`Received`/`TopicMessage` shape, Routing ID, request sequence, topic과 multipart
framing을 유지한다. 차이는 caller에게 보이는 physical payload part마다 Core의
opaque retained credit 하나를 결과가 private하게 소유한다는 점뿐이다. 이 API는
Framework backend의 queue·executor·handler 수명에 Core credit을 함께 전달하기
위한 경계이며 일반 application 수신의 기본 경로가 아니다.

`Received.Close`와 `TopicMessage.Close`는 현재 part와 retained credit을 모두
정확히 한 번 반환한다. 같은 output으로 다음 일반 또는 retained 수신을 시작해도
기존 결과를 먼저 정리하며, no-data와 부분 multipart error도 이미 얻은 credit을
남기지 않는다. Framework의 drop·cancel·error 경로도 소유한 aggregate에 `Close`를
호출한다. Go binding은 GC 실행 시점을 수명 계약으로 사용하지 않으므로 정상 경로와
누락 방지는 모두 명시적인 `Close`/재사용에 의존한다.

개별 `Message` part는 retained credit을 숨겨 소유하지 않는다. Native lease handle,
별도 retry/application capacity, allowance나 중복 accounting 상태도 public API로
노출하지 않는다.

Socket monitor는 typed event mask로 열고 `MonitorEvent`, `MonitorStatus`를 제공한다.
Core 0.12.0의 각 monitor event mask와 delivered event value는 대응하는 typed constant로
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
`uint64`/boolean 값으로 제공한다. Reset은 current·pending·queue count와 위 세
owner-lifecycle gauge를 유지하고 두 peak를 current로 재기준화하며 epoch counter를 0으로 만든
뒤 `MeasurementEpoch`을 증가시킨다. ABI version/size 불일치는 unsupported error다.
Poller는 socket, file descriptor와 timer source의 readiness를 `PollEvent`로 보고한다.
Timer는 interval event를 poller 또는 직접 receive하는 데 사용한다. Monitor, poller와
timer의 callback 또는 event result는 native callback thread를 public consumer callback
실행 위치로 노출하지 않는다.

## Receive flow state

이 바인딩은 Core의 receive-flow 상태를 `ReceiveFlowState` 타입으로 노출한다. 값은
`ReceiveFlowRunning`과 `ReceiveFlowPaused`이며 설정은
`SetReceiveFlowState(ReceiveFlowState) error`다. Go error contract를 따른다. 성공은 `nil`
error이고, 실패는 native `zlink_config_result_t`를 `Result`로, native errno를 errno로 담은
`*ConfigError`다. 따라서 completion lane이 없는 socket은 `ConfigNotSupported`를 담은
`*ConfigError`를 반환한다. Handle이 nil이거나 이미 닫혔으면 Core를 호출하지 않고
`ConfigInvalidHandle`을 반환한다. 이미 유지하는 상태를 다시 설정하면 `nil`을 반환한다.

관측 표면은 C 계약을 따르며 상수와 metric 이름은 C 계층이 확정한다. Monitor event
`SEND_FLOW_PAUSED`, `SEND_FLOW_RESUMED`, `FLOW_STATE_STALE`(`1 << 16`, `1 << 17`,
`1 << 18`, 전체 mask `0x7FFFF`), event flag `SEND_FLOW_WRITABLE`(`1 << 1`),
`FLOW_STATE_STALE_GENERATION`(`1 << 2`), `FLOW_STATE_STALE_EPOCH`(`1 << 3`), status detail
bit `FLOW_STATE`(`1 << 5`), status field 5개 `flow_paused_connections`,
`flow_pause_applied_total`, `flow_resume_applied_total`, `flow_state_stale_total`,
`flow_pause_duration_ms`를 이 언어의 이름 규칙으로 투영한다.

Flow-state frame은 Core 안에 머문다. 바인딩은 setter를 호출하고 monitor event와 snapshot
field를 읽을 뿐, flow-state frame을 직접 encode, decode, 송신 또는 수신하지 않는다.

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

- Context가 `Submit` 호출 전에 이미 취소되었거나 deadline을 넘겼으면 routed send
  channel은 해당 표준 error를 전달하고, request channel은 그 error를
  `RequestReplyCompletion.Err`에 담아 전달한다. 이 표준 error를 함수군별 Core error로
  변환하지 않는다.
- Native request가 수용된 뒤의 reply와 실패도 같은 `RequestReplyCompletion`
  channel로 전달한다. 별도 callback terminal은 없다.

## FFI와 package 경계

Go cgo bridge의 include path는 package 안의 `include/`로 고정한다. Repository의
`core/include`를 package consumer가 직접 읽지 않는다. `bindings/go/tests/raw-core11-
allowlist.json`은 header file set, SHA-256, cgo raw symbol과 local callback helper를
machine-readable 형태로 고정한다. `zlink/service/` 및 이전 service symbol은
allowlist에 없다.

Module package는 다음 file proxy layout을 사용한다.

```text
zlink.systems/zlink/@v/v0.12.0.info
zlink.systems/zlink/@v/v0.12.0.mod
zlink.systems/zlink/@v/v0.12.0.zip
```

지원 platform runtime은 module의 `native/<platform>/` 아래에 포함한다. Package
consumer는 `replace`와 repository `core/build` 없이 module cache의 runtime을
사용해야 한다.

## 공개 계약에 포함하지 않는 것

- Spot, Actor, MeshNode와 service operation
- Core 10 compatibility alias와 service header
- private cgo type, native pointer와 callback userdata
- message별 codec registry 또는 호출자 raw encode/decode 우회
- `NativeErrno`

GoDoc과 process sample의 현재 검증 진입점은 `bindings/go/README.godoc.md`,
`bindings/go/tests/run_tests.sh`와 `bindings/go/samples/run_samples.sh`에 기록한다.
이 문서의 public contract 변경은 먼저 common binding spec과 관련 draft의 review
상태를 확인한 뒤 반영한다.
