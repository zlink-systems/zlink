한국어 | [English](04-eventing.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone
timer를 다룬다 — 각각 `OpenSocketMonitor(...)`, `NewPoller()`,
`NewTimer()`, 그리고 standalone `Poll(...)` 함수. `Timer`, `Poller`,
`Poll`은 모두 `poller_timer.go`에 선언돼 있다 — 다른 일부 언어와 달리
별도 timer 파일이 없다. 정확한 signature는
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go)와
[`poller_timer.go`](../../../../bindings/go/internal/native/poller_timer.go)가
소유하며,
[`contracts/eventing.go`](../../../../bindings/go/contracts/eventing.go)를
통해 alias로 re-export된다.

---

## `SocketMonitor`

socket의 connection lifecycle event를 관찰하고 현재 status를 읽는다.

```go
monitor, err := contracts.OpenSocketMonitor(dealer, contracts.MonitorEventConnectionReady)
monitor.OnEvent(func(event *contracts.MonitorEvent) {
    fmt.Println(event.RemoteAddr)
})
status, err := monitor.Status()
```

**Options.**

| Member | 의미 |
| --- | --- |
| `OpenSocketMonitor(socket SocketTarget, events ...MonitorEventMask) (*SocketMonitor, error)` | monitor를 연다; **variadic이고 실제로 필터링한다** — `events`를 넘기지 않으면 `MonitorEventAll`을 구독하지만, 넘긴 mask는 OR로 합쳐져 실제로 반영된다, `SocketMonitor::open`이 그런 파라미터를 전혀 받지 않고 구성한 어떤 mask 값과도 무관하게 항상 모든 이벤트를 구독하는 rust와 다르다 |
| `Recv(flags RecvFlags) (*MonitorEvent, error)` | blocking과 non-blocking 둘 다를 위한 단일 진입점; non-blocking 형태엔 `RecvFlagsDontWait`를 넘긴다 — **여기서 no-data 시 값-반환 형태는 문서화된 예외다**, 자신의 doc comment에 따르면: "Value-return form is allowed for monitor/timer control-plane APIs by doc/spec/bindings/go/README.md §Receive And Subscribe Shape" |
| `Status() (*MonitorStatus, error)` | 시점 스냅샷을 반환 |
| `OnEvent(handler func(*MonitorEvent)) error` | 수동적 lifecycle-event 콜백을 등록 |
| `Close() error` | monitor를 닫음 |

**Completion result.** 모든 member는 동기다. `SocketTarget`(Core
category)은 모든 내장 socket type이 구현하는 공유 interface다 —
`Proxy`/`Poller` 등록이 쓰는 것과 같다.

**선택 기준.** 한 번 등록하는 수동적 lifecycle observer엔 `OnEvent`를
쓴다; 대신 pull 기반 drain loop엔 `Recv`를 쓴다. `OpenSocketMonitor`에
특정 `MonitorEventMask` 값을 넘겨 구독을 제한한다 — rust와 달리 이
binding의 mask 인자는 실제로 효과가 있다.

---

## `MonitorEventMask` / `MonitorEventType`

같은 16개 lifecycle-event 비트를 두 개의 별개 명명 타입이 보고한다:
`MonitorEventMask`(`uint32`)는 `OpenSocketMonitor`의 variadic 인자가
받는 타입이고; `MonitorEventType`(`uint64`)는 `MonitorEvent.Event`가
보고하는 타입이다. 모든 명명 상수는 이름과 숫자값이 일치하는 형태로
두 계열 모두에 존재한다(`MonitorEventConnected` /
`MonitorEventTypeConnected`, ...) — 구독한 mask를 수신된 event의 type과
비교하려는 caller는 둘 사이를 명시적으로 변환해야 한다; 같은 비트를
가졌다 해도 cast 없이는 서로 바꿔 쓸 수 없다.

**Options.**

| Member | 의미 |
| --- | --- |
| `MonitorEventMask` 상수 | `MonitorEventConnected`, `MonitorEventConnectDelayed`, `MonitorEventConnectRetried`, `MonitorEventListening`, `MonitorEventBindFailed`, `MonitorEventAccepted`, `MonitorEventAcceptFailed`, `MonitorEventClosed`, `MonitorEventCloseFailed`, `MonitorEventDisconnected`, `MonitorEventMonitorStopped`, `MonitorEventHandshakeFailedNoDetail`, `MonitorEventConnectionReady`, `MonitorEventHandshakeFailedProtocol`, `MonitorEventHandshakeFailedAuth`, `MonitorEventPeerWeightChanged`, `MonitorEventAll` |
| `MonitorEventType` 상수 | 위 모든 `MonitorEventMask` 값을 `MonitorEventType`-접두 이름으로 미러링(`MonitorEventTypeConnected`, ...) |
| `MonitorSourceKind`(`uint32`) | `MonitorSourceSocket` — 유일한 값 |

**Completion result.** N/A — 순전히 구독/비교에만 쓰이는 평범한 bitmask
value type.

**선택 기준.** `MonitorEventMask` 상수를(OR로 합치거나 별도 variadic
인자로) `OpenSocketMonitor`에 넘긴다. `MonitorEvent.Event`는
`MonitorEventMask` 계열이 아니라 `MonitorEventType` 상수와
비교하거나, 명시적으로 변환한다.

---

## `MonitorEvent`

monitor가 보고하는 단일 socket connection-lifecycle event.

```go
if event.IsConnected() {
    // ...
}
```

**Options.** 편의 predicate 메서드는 전체 lifecycle event 집합 중
일부만 다룬다 — **`ConnectDelayed`, `ConnectRetried`, `BindFailed`,
`AcceptFailed`, `Closed`, `CloseFailed`, `MonitorStopped`,
`HandshakeFailedNoDetail`, `HandshakeFailedProtocol`,
`HandshakeFailedAuth`, `PeerWeightChanged`엔 predicate가 없다** —
caller가 직접 `event.Event&MonitorEventTypeX`로 비트 검사해야 한다,
다른 모든 언어의 `MonitorEvent` predicate 집합과 같은 공백이다.

| Field | 타입 | 의미 |
| --- | --- | --- |
| `Event` | `MonitorEventType` | lifecycle event의 종류 |
| `Value` | `uint32` | event별 값 |
| `RoutingID` | `RoutingID`, 없으면 zero-value | event가 제공할 때만 존재하는 peer routing id |
| `LocalAddr` / `RemoteAddr` | `string` | event에 결부된 local/remote 주소 |
| `HasRoutingID()` / `IsConnected()` / `IsDisconnected()` / `IsListening()` / `IsAccepted()` / `IsConnectionReady()` | `bool` | 해당 event 종류에 대한 predicate |

**Completion result.** N/A — monitor가 전달하는 불변 값.

**선택 기준.** 다루는 다섯 lifecycle 전이엔 명명된 `Is*` predicate를
쓴다; 그 외 event 종류는 `Event`를 문서화된 `MonitorEventType` 상수와
직접 비트 검사한다.

---

## `MonitorStatus`

`SocketMonitor.Status()`가 반환하는, 모니터링 대상 상태와
auto-high-water-mark 텔레메트리의 시점 스냅샷. 평범한 공개 필드
struct다.

**Options.** 파라미터 없음 — 모든 field가 공개.

| 그룹 | Field |
|---|---|
| ABI identity | `ABIVersion`, `StructSize`(`uint32`) |
| Source/state | `SourceKind`(`MonitorSourceKind`: `MonitorSourceSocket`뿐), `StateFlags`/`DetailFlags`(`uint32` bitmask), `IsReady()`(계산 메서드) |
| Pending count | `SndPendingMsgs`, `RcvPendingMsgs`(`uint64`) |
| Auto-HWM 설정 | `AutoHwmEnabled`(`bool`), `AutoHwmProfile`/`AutoHwmRole`/`AutoHwmPolicyClass`(`uint32`), `AutoHwmUnitBudgetBytes`/`AutoHwmSocketMessageSlots`(`uint64`), `AutoHwmSizeCap`(`uint32`) |
| Connection bucket | `AutoHwmConnectionBucketEnabled`(`bool`), `AutoHwmConnectionBucketCount`/`Index`/`Hwm4K`(`uint32`), `AutoHwmConnectionBucketHysteresisRetained`(`bool`) |
| Auto-HWM plan(바이트) | `AutoHwmEffectiveMessageBytes`, `AutoHwmPlannedSndHwmBytes`/`RcvHwmBytes`, `AutoHwmAppliedSndHwmBytes`/`RcvHwmBytes`(`uint64`), `AutoHwmEffectiveSndBuf`/`RcvBuf`(`int32`) |
| Auto-HWM recalc | `AutoHwmLastRecalcMs`(`uint64`), `AutoHwmLastRecalcReason`(`AutoHwmRecalcReason`, Core category), `AutoHwmSendBlockedRatioPPM`(`uint32`) |
| Auto-HWM deferred shrink | `AutoHwmDeferredSndHwmBytes`/`RcvHwmBytes`(`uint64`, 대응하는 `AutoHwmDeferredSndHwmValid`/`RcvHwmValid` `bool`이 true일 때만 유효) |
| In-flight/charging | `SndBytesInFlight`, `RcvBytesInFlight`, `MinimumCoreMessageChargeBytes`, `OversizeMessageAdmissionCount`, `OversizeMessageAdmissionMaxBytes`(`uint64`) |

**Completion result.** N/A — 평범한 공개 field, 계산 메서드
`IsReady()` 하나 추가. **이 binding의 이 타입엔 `IsClosed()` 메서드가
없다**, `is_ready()`와 나란히 `is_closed()`가 있는 rust와 다르다.

**선택 기준.** `StateFlags`를 직접 디코딩하는 대신 `IsReady()`를
호출한다. socket의 실효 send/receive HWM이 설정한
`CommonSocketOptions` 값(Sockets category)과 다른 이유를 진단할 땐
connection-bucket과 auto-HWM-plan field를 쓴다.

---

## `Poller`

socket, file descriptor, timer를 하나의 재사용 가능한 wait로
multiplex한다.

```go
poller, err := contracts.NewPoller()
poller.AddSocket(dealer, contracts.PollIn, 1)
poller.AddTimer(timer, 2)
events := make([]contracts.PollEvent, 8)
ready, err := poller.Wait(events, time.Second)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewPoller() (*Poller, error)` | poller를 생성 |
| `AddSocket(socket SocketTarget, events PollEventFlag, slot uintptr) error` | socket을 등록; `slot`은 대응하는 결과로 그대로 되돌아오는 caller token |
| `AddFd(fd int, events PollEventFlag, slot uintptr) error` | raw file descriptor를 등록, 같은 형태 |
| `AddTimer(timer *Timer, slot uintptr) error` | timer를 socket/fd와 함께 multiplex하도록 등록 |
| `ModifySocket(socket, events) error` / `ModifyFd(fd, events) error` | 이미 등록된 socket/fd의 감시 event를 교체; `ModifySocket`은 특히 `PollCompletion` flag 변경을 거부한다 — 그 등록 모드는 `RemoveSocket` + `AddSocket`으로만 바꿔야 한다, completion 처리가 Core에서 별도 소유권을 갖기 때문 |
| `RemoveSocket(socket) error` / `RemoveFd(fd int) error` / `RemoveTimer(timer *Timer) error` | source 등록을 해제 |
| `Wait(events []PollEvent, timeout time.Duration) (int, error)` | `timeout`까지 block하며 `len(events)`까지 결과를 그 자리에 써 넣음; **`time.Duration`을 받는다**, rust의 raw millisecond `i64`와 다름; native wait이 인터럽트되면(`EINTR`) error가 아니라 `(0, nil)`로 처리한다 |
| `Size() int` | 현재 등록된 source 개수; 내부 error가 나면 전파하는 대신 `0`을 반환 |

**Completion result.** 등록/제거 member는 `error`를 반환한다.
`Wait`는 `(int, error)`를 반환한다 — ready count.

**선택 기준.** 서비스 수명 전체에 걸쳐 poller 하나를 쓴다. `Wait`
호출마다 새로 할당하는 대신 `[]PollEvent` 슬라이스 하나를 재사용한다.

---

## `PollItem` / `PollEvent` / `Poll(...)`

`PollItem`은 `Poller` 대신 standalone `Poll(...)` 함수가 쓰는 raw
poll descriptor다; `PollEvent`는 `Poller.Wait`가 보고하는 ready
source 하나다.

```go
items := []contracts.PollItem{{Socket: dealer, Events: contracts.PollIn}}
ready, err := contracts.Poll(items, 500*time.Millisecond)
if items[0].REvents&contracts.PollIn != 0 { /* ... */ }
```

**Options — `PollItem`**(모든 field 공개).

| Field | 타입 | 의미 |
| --- | --- | --- |
| `Socket` | `SocketTarget`, 일반 fd 항목이면 `nil` 가능 | 이 item이 감시하는 socket |
| `Fd` | `int` | 이 item이 감시하는 file descriptor |
| `Events` / `REvents` | `PollEventFlag` | 감시할/반환된 poll-event bitmask |

**Options — `PollEvent`**, `Poller.Wait`가 반환. **`PollEvent`의
밑바탕 socket/timer 참조는 export되지 않는다** — 공개 field인
`PollItem.Socket`과 달리, `Wait`가 반환한 `PollEvent`는 원래 socket이나
`*Timer`를 직접 복원할 방법이 없다; caller는 등록할 때 쓴 것을 `Slot`
으로 되짚어야 한다.

| Field | 타입 | 의미 |
| --- | --- | --- |
| `SourceKind` | `PollSourceKind`: `PollSourceSocket`/`PollSourceFD`/`PollSourceTimer` | source가 socket·file descriptor·timer 중 무엇인지 |
| `Fd` | `int` | file descriptor, FD kind source에서만 채워짐 |
| `Slot` | `uintptr` | 등록 시 넘긴 caller token |
| `Revents` | `PollEventFlag` | raw poll-event bitmask |

**Options — `Poll(...)`.** `Poll(items []PollItem, timeout
time.Duration) (int, error)`은 어떤 `Poller` 인스턴스와도 무관하게
`PollItem` 배치를 한 번 poll하고, `REvents`를 각 item에 그 자리에서
다시 써 넣는다.

**Completion result.** `PollItem`/`PollEvent`는 평범한 value type이다.
`Poll`은 `(int, error)`를 반환한다 — ready count, `items`를 그
자리에서 변경.

**선택 기준.** `Poller.Wait` 결과 각각을 대응하는 socket, descriptor,
timer로 되돌리려면 `PollEvent.SourceKind`/`Slot`으로 분기한다.
`Poller`의 등록 기반 모델 밖에서 일회성 배치 poll엔
`PollItem`/`Poll`만 쓴다.

---

## `Timer`

interval마다 fire하며 poll되거나 기다려질 수 있는 timer, `Poller`와
독립적으로 생성되지만 `Poller.AddTimer`로 등록할 수 있다.

```go
timer, err := contracts.NewTimer()
timer.OnFire(func(t *contracts.Timer, fireCount uint64) {
    fmt.Println("fired", fireCount, "times")
})
timer.Start(1_000_000_000, 0) // 나노초 단위 interval, time.Duration 아님
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewTimer() (*Timer, error)` | timer를 생성 |
| `Start(intervalNs, repeatCount uint64) error` | `intervalNs`마다 fire를 시작; **interval은 raw 나노초 `uint64`다, `time.Duration`이 아니다** — 이 binding의 다른 곳(Core/Sockets category)에 있는 duration-typed option 관례를 깬다; `repeatCount`가 `0`이면 무한 반복 |
| `Stop() error` | fire를 멈춤; `Start`로 재시작 가능 |
| `Recv() (uint64, bool, error)` | 누적 fire count; 대기 중인 게 없으면 error가 아니라 `bool`이 `false` — **`SocketMonitor.Recv`와 같은 문서화된 no-data 시 값-반환 예외**, doc/spec/bindings/go/README.md §Receive And Subscribe Shape를 인용하는 자신의 doc comment에 따르면 |
| `OnFire(handler func(timer *Timer, fireCount uint64)) error` | 수동적 interval 콜백을 등록 |
| `Close() error` | timer를 닫음 |

**Completion result.** 모든 member는 동기다.

**선택 기준.** 수동적 interval callback엔 `OnFire`를 쓴다; 대신
expiration을 poll하려면 `Recv`를 쓰거나, socket과 함께 하나의
wait에서 multiplex하려면 `Poller.AddTimer`로 timer를 등록한다.

---

## Eventing 상수

| 타입 | 사용처 | 값 |
|---|---|---|
| `PollEventFlag`(명명된 `int16`) | `Poller.AddSocket`/`ModifySocket`/`AddFd`/`ModifyFd`, `PollItem.Events`/`.REvents`, `PollEvent.Revents` | `PollIn`(`1`), `PollOut`(`2`), `PollErr`(`4`), `PollPri`(`8`), `PollCompletion`(`32`) — **이 binding은 `PollErr`/`PollPri`를 포함해 다섯 개 전부를 선언한다**, 둘 다에 대응물이 없는 rust와 다르다 |
| `MonitorSourceKind`(명명된 `uint32`) | `MonitorStatus.SourceKind` | `MonitorSourceSocket` |
| `PollSourceKind`(명명된 `int32`) | `PollEvent.SourceKind` | `PollSourceSocket`, `PollSourceFD`, `PollSourceTimer` |

---

[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller_timer.go`](../../../../bindings/go/internal/native/poller_timer.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
