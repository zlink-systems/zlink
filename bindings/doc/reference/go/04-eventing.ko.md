한국어 | [English](04-eventing.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone
timer를 다룬다 — 각각 `OpenSocketMonitor(...)`, `NewPoller()`,
`NewTimer()`, 그리고 standalone `Poll(...)` 함수. `Poller`와 `Poll`은
`poller.go`에, `Timer`는 `timer.go`에 선언돼 있다. 정확한 signature는
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller.go`](../../../../bindings/go/internal/native/poller.go),
[`timer.go`](../../../../bindings/go/internal/native/timer.go)가
소유하며,
[`root_projection.go`](../../../../bindings/go/root_projection.go)가 alias로 re-export한다.

---

## `SocketMonitor`

socket의 connection lifecycle event를 관찰하고 현재 status를 읽는다.

```go
monitor, err := zlink.OpenSocketMonitor(dealer, zlink.MonitorEventConnectionReady)
event, err := monitor.Recv(zlink.RecvFlagsNone)
status, err := monitor.Status()
```

**Options.**

| Member | 의미 |
| --- | --- |
| `OpenSocketMonitor(socket SocketTarget, options ...MonitorOpenOption) (*SocketMonitor, error)` | 이벤트 마스크를 OR로 결합한다. 생략하면 모든 이벤트를 선택한다. `MonitorHwmBytes(uint64)`로 바이트 HWM을 설정한다. |
| `Recv(flags RecvFlags) (*MonitorEvent, error)` | 모니터 이벤트를 꺼낸다. |
| `Status() (*MonitorStatus, error)` | 시점 스냅샷을 반환 |
| `Recv(RecvFlagsDontWait)` | 큐가 비어 있으면 `nil`과 `RecvNoData`를 담은 `*RecvError`를 반환한다. |
| `Close() error` | monitor를 닫음 |

**Completion result.** 모든 member는 동기다. `SocketTarget`(Core
category)은 모든 내장 socket type이 구현하는 공유 interface다 —
`Proxy`/`Poller` 등록이 사용하는 것과 같다.

**선택 기준.** pull 기반 lifecycle-event drain loop엔 `Recv`를 사용한다.
`OpenSocketMonitor`에
특정 `MonitorEventMask` 값을 넘겨 구독을 제한한다.

---

## `MonitorEventMask` / `MonitorEventType`

event mask를 `MonitorOpenOption`으로 전달해 구독할 event를 고른다.
`MonitorEventMask`(`uint32`)와 `MonitorEventType`(`uint64`)에는 같은 이름과
비트값을 가진 event 상수가 각각 19개 있다. 두 타입의 값을 직접 비교하려면 명시적으로 변환한다.

**Options.**

| Member | 의미 |
| --- | --- |
| `MonitorEventMask` 상수 | `MonitorEventConnected`, `MonitorEventConnectDelayed`, `MonitorEventConnectRetried`, `MonitorEventListening`, `MonitorEventBindFailed`, `MonitorEventAccepted`, `MonitorEventAcceptFailed`, `MonitorEventClosed`, `MonitorEventCloseFailed`, `MonitorEventDisconnected`, `MonitorEventMonitorStopped`, `MonitorEventHandshakeFailedNoDetail`, `MonitorEventConnectionReady`, `MonitorEventHandshakeFailedProtocol`, `MonitorEventHandshakeFailedAuth`, `MonitorEventPeerWeightChanged`, `MonitorEventSendFlowPaused`, `MonitorEventSendFlowResumed`, `MonitorEventFlowStateStale`, `MonitorEventAll` |
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
사용한다; 그 외 event 종류는 `Event`를 문서화된 `MonitorEventType` 상수와
직접 비트 검사한다.

---

## `MonitorStatus`

`SocketMonitor.Status()`는 조회 시점의 상태를 반환한다. `ABIVersion`과 `StructSize`는 Core
레이아웃을, `SourceKind`는 관찰하는 자원의 종류를 나타낸다.

| 접근자 또는 필드 | 의미 |
| --- | --- |
| `State().Has(flag)`, `Detail().Has(flag)` | 타입이 지정된 상태·세부 필드를 검사한다. `IsReady()`는 `MonitorStateReady`를 검사한다. |
| `SndPendingMsgs`, `RcvPendingMsgs` | 대기 중인 메시지 수다. |
| `SndPendingBytes`, `RcvPendingBytes` | 대기 중인 메시지의 회계 바이트 수다. |
| `AutoHwmEnabled`, `AutoHwmProfile`, `AutoHwmRole`, `AutoHwmPolicyClass` | 자동 HWM의 설정 진단값이다. |
| `AutoHwmPlannedSndHwmBytes`, `AutoHwmPlannedRcvHwmBytes`, `AutoHwmAppliedSndHwmBytes`, `AutoHwmAppliedRcvHwmBytes` | 계획 및 적용된 HWM을 바이트로 나타낸다. |
| `AutoHwmEffectiveSndBuf`, `AutoHwmEffectiveRcvBuf` | 실제 송수신 buffer 크기다. |
| `AutoHwmDeferredSndHwmBytes`, `AutoHwmDeferredRcvHwmBytes` | 유예된 HWM을 바이트로 나타낸다. 대응하는 `AutoHwmDeferredSndHwmValid` 또는 `AutoHwmDeferredRcvHwmValid`가 `true`일 때 유효하다. |
| `AutoHwmLastRecalcMs`, `AutoHwmLastRecalcReason` | 최근 자동 HWM 재계산 시각과 원인이다. |
| `IsFlowStateDetailPopulated()` | 수신 흐름 필드에 값이 있는지 알려 준다. |
| `AutoHwmSendBlockedRatioPPM` | 최초 send 수락 시도 중 HWM으로 막힌 비율을 백만분율로 나타낸다. |
| `SndBytesInFlight`, `RcvBytesInFlight`, `MinimumCoreMessageChargeBytes`, `OversizeMessageAdmissionCount`, `OversizeMessageAdmissionMaxBytes` | 전송 중인 바이트 수, 메시지당 최소 회계 바이트 수, 크기 한도를 초과해 수락한 메시지의 수와 최대 바이트 크기다. |

**Completion result.** `Status()`는 공개 field를 가진 snapshot과 `error`를 반환한다.
`IsReady()`와 flag accessor는 snapshot을 동기적으로 읽는다.

**선택 기준.** 준비 여부는 `IsReady()`로 확인한다. 실제 송수신 HWM이
`CommonSocketOptions`의 설정과 다른 이유를 조사할 때는 계획, 적용, 유예 HWM
field를 비교한다.

---

## 연결 시도 스냅샷

나가는 연결을 지원하는 socket을 `PollConnect`로 등록한다. 이벤트가 준비되면 같은 socket의
`ConnectionAttemptsSnapshot() ([]ConnectionAttempt, error)`을 호출한다. socket당 관찰자를 하나 둔다.
조회가 성공하면 읽은 변경에 해당하는 준비 상태가 해제된다.

각 행은 나가는 연결 등록 하나를 나타낸다. `Endpoint`는 주소, `State`는 현재 상태다.
`IntentID`, `Revision`, `LastFailureID`는 동등성만 비교한다. `LastFailure`는 연결이 회복된 뒤에도
마지막 실패를 보존한다. 실패 이력이 없으면 `LastFailureID == 0`이다. 조회 자체가 실패하면
`*ConfigError`를 반환한다.

---

## `Poller`

socket, file descriptor, timer를 하나의 재사용 가능한 wait로
multiplex한다.

```go
poller, err := zlink.NewPoller()
poller.AddSocket(dealer, zlink.PollIn, 1)
poller.AddTimer(timer, 2)
events := make([]zlink.PollEvent, 8)
ready, err := poller.Wait(events, time.Second)
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewPoller() (*Poller, error)` | poller를 생성 |
| `AddSocket(socket SocketTarget, events PollEventFlag, slot uintptr) error` | socket을 등록; `slot`은 대응하는 결과로 그대로 되돌아오는 caller token |
| `AddFd(fd int, events PollEventFlag, slot uintptr) error` | raw file descriptor를 등록, 같은 형태 |
| `AddTimer(timer *Timer, slot uintptr) error` | timer를 socket/fd와 함께 multiplex하도록 등록 |
| `ModifySocket(socket, events) error` / `ModifyFd(fd, events) error` | 등록한 이벤트를 바꾼다. `PollCompletion`을 추가하거나 빼면 완료 처리 소유권을 얻거나 반환한다. |
| `RemoveSocket(socket) error` / `RemoveFd(fd int) error` / `RemoveTimer(timer *Timer) error` | source 등록을 해제 |
| `Wait(events []PollEvent, timeout time.Duration) (int, error)` | `timeout`까지 block하며 `len(events)`까지 결과를 그 자리에 써 넣음; **`time.Duration`을 받는다**, rust의 raw millisecond `i64`와 다름; 실패한 wait는 Core의 결과를 담은 `*ConfigError`다(다른 `Wait`가 진행 중이면 `ConfigBusy`, 709) |
| `Size() (int, error)` | 현재 등록된 source 개수; `Wait`가 진행 중이면 `ConfigBusy`(709) |

**Completion result.** 등록/제거 member는 `error`를 반환한다.
`Wait`는 `(int, error)`를 반환한다 — ready count.

**선택 기준.** 서비스 수명 전체에 걸쳐 poller 하나를 사용한다. `Wait`
호출마다 새로 할당하는 대신 `[]PollEvent` 슬라이스 하나를 재사용한다.

---

## `PollItem` / `PollEvent` / `Poll(...)`

`PollItem`은 `Poller` 대신 standalone `Poll(...)` 함수가 사용하는 raw
poll descriptor다; `PollEvent`는 `Poller.Wait`가 보고하는 ready
source 하나다.

```go
items := []zlink.PollItem{{Socket: dealer, Events: zlink.PollIn}}
ready, err := zlink.Poll(items, 500*time.Millisecond)
if items[0].REvents&zlink.PollIn != 0 { /* ... */ }
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
`*Timer`를 직접 복원할 방법이 없다; caller는 등록할 때 사용한 것을 `Slot`
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
`PollItem`/`Poll`만 사용한다.

---

## `Timer`

interval마다 fire하며 poll되거나 기다려질 수 있는 timer, `Poller`와
독립적으로 생성되지만 `Poller.AddTimer`로 등록할 수 있다.

```go
timer, err := zlink.NewTimer()
timer.Start(1_000_000_000, 0) // 나노초 단위 interval, time.Duration 아님
fireCount, ok, err := timer.Recv()
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewTimer() (*Timer, error)` | timer를 생성 |
| `Start(intervalNs, repeatCount uint64) error` | `intervalNs` 나노초마다 fire한다. `repeatCount`가 `0`이면 무한 반복한다. |
| `Stop() error` | fire를 멈춤; `Start`로 재시작 가능 |
| `Recv() (uint64, bool, error)` | 누적 fire count; 대기 중인 게 없으면 error가 아니라 `bool`이 `false` — **`SocketMonitor.Recv`와 같은 문서화된 no-data 시 값-반환 예외**, doc/spec/bindings/go/README.md §Receive And Subscribe Shape를 인용하는 자신의 doc comment에 따르면 |
| 반복 `Recv()` | pull surface로 pending cumulative fire count drain |
| `Close() error` | timer를 닫음 |

**Completion result.** 모든 member는 동기다.

**선택 기준.** expiration을 pull하려면 `Recv`를 사용하거나, socket과 함께 하나의
wait에서 multiplex하려면 `Poller.AddTimer`로 timer를 등록한다.

---

## Eventing 상수

| 타입 | 사용처 | 값 |
|---|---|---|
| `PollEventFlag` (`int16`) | `Poller.AddSocket`/`ModifySocket`/`AddFd`/`ModifyFd`, `PollItem.Events`/`REvents`, `PollEvent.Revents` | `PollIn` (1), `PollOut` (2), `PollErr` (4), `PollPri` (8), `PollCompletion` (32), `PollRoute` (64), `PollConnect` (128). |
| `MonitorSourceKind`(명명된 `uint32`) | `MonitorStatus.SourceKind` | `MonitorSourceSocket` |
| `PollSourceKind`(명명된 `int32`) | `PollEvent.SourceKind` | `PollSourceSocket`, `PollSourceFD`, `PollSourceTimer` |

---

[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller.go`](../../../../bindings/go/internal/native/poller.go),
[`timer.go`](../../../../bindings/go/internal/native/timer.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
