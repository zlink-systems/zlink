한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity,
package-level utility 함수를 다룬다. **socket 생성은 `Context`의
method다**(`ctx.PairSocket()`, ...), top-level factory가 아니다 —
지금까지 다룬 wrapper binding 중 유일하게 이런 형태다. 정확한
signature는
[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go)와
[`utility.go`](../../../../bindings/go/internal/native/utility.go)가
소유하며,
[`contracts/core.go`](../../../../bindings/go/contracts/core.go)를 통해
alias로 re-export된다.

---

## `NewContext()`

메시징 context를 생성한다 — socket의 factory이자 소유자.

```go
ctx, err := contracts.NewContext()
defer ctx.Close()
```

**Options.** 인자 없음.

**Completion result.** `(*Context, error)`를 반환한다. caller가
소유하며 반드시 `Close()`를 호출해야 한다 — close하면 그 하위에 아직
열려 있는 모든 것(context에서 생성된 socket 포함)이 종료된다.

**선택 기준.** application이 필요로 하는 context마다 한 번 호출한다 —
대부분의 application은 정확히 하나가 필요하다.

---

## `Context.Shutdown()` / `Context.RecalculateAutoHwm()`

context의 socket이 닫히지 않은 채 blocking operation을 인터럽트하거나,
automatic high-water mark의 즉시 재계산을 강제한다.

```go
ctx.Shutdown()
ctx.RecalculateAutoHwm()
```

**Options.** 둘 다 인자 없음.

**Completion result.** 둘 다 `error`를 반환한다. `Shutdown`은 이
context 하위 socket의 blocking 호출을 인터럽트하지만 context나 그
socket을 닫지 않는다. `RecalculateAutoHwm`은 아직 `AutoHwmProfile`이
설정된 socket에 대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 goroutine에서 socket을 쓰는 중인 context엔
`Close()` 전에 `Shutdown()`을 호출한다. auto-HWM profile이나
message-unit option을 바꾼 후엔 새 sizing을 즉시 적용하려고
`RecalculateAutoHwm()`을 호출한다.

---

## `Context.Options()`

I/O thread와, context에서 생성되는 모든 socket이 상속하는 기본값을
관장하는, 짝을 이루는 getter/setter 메서드를 가진 context-wide option
facade를 반환한다.

```go
opts := ctx.Options()
opts.SetIOThreads(8)
opts.SetAutoHwmProfile(contracts.AutoHwmProfileLowLatency)
opts.AddThreadAffinity(2)
```

**Options.** 모든 getter는 `(T, error)`를 반환한다. 모든 setter는
`error`를 반환한다 — accessor 일부만 실패할 수 있는 다른 언어와 다르다.

| Member | 의미 |
| --- | --- |
| `IOThreads()` / `SetIOThreads(int)` | I/O thread 개수 |
| `MaxSockets()` / `SetMaxSockets(int)` | context 전체 socket 상한 |
| `SocketLimit()` | 읽기 전용, `MaxSockets`의 빌드상 하드 상한 |
| `ThreadPriority()` / `SetThreadPriority(int)` | dispatch thread 우선순위 |
| `ThreadSchedulingPolicy()` / `SetThreadSchedulingPolicy(int)` | dispatch thread scheduling policy |
| `ThreadNamePrefix()` / `SetThreadNamePrefix(string)` | OS에 보이는 dispatch thread 이름 접두사 |
| `AutoHwmEnabled()` / `SetAutoHwmEnabled(bool)` | auto-HWM sizing 활성 여부 |
| `AutoHwmRecalcDebounce()` / `SetAutoHwmRecalcDebounce(time.Duration)` | 자동 재계산 사이 최소 간격 |
| `MaxMessageSize()` / `SetMaxMessageSize(int)` | 메시지 하나의 크기 상한 |
| `MessageStructSize()` | 읽기 전용, native message struct 크기, 진단 전용 |
| `Blocky()` / `SetBlocky(bool)` | blocking 호출이 실제로 block하는지 fail fast하는지 |
| `AutoHwmProfile()` / `SetAutoHwmProfile(AutoHwmProfile)` | automatic HWM sizing profile — Sockets category 참고 |
| `AutoHwmMsgUnitBytes()` / `SetAutoHwmMsgUnitBytes(int)` | auto-HWM용 accounted-byte 단위; `0`은 socket-type 기본값 선택 |
| `AddThreadAffinity(cpu int)` | setter만, I/O thread를 CPU에 고정 |
| `RemoveThreadAffinity(cpu int)` | setter만, I/O thread의 CPU 고정을 해제 |

**Completion result.** 모든 accessor는 동기이며, 값과 함께 `error`를
반환한다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에
조정한다. `AutoHwmProfile`/`AutoHwmEnabled` 변경은
`Context.RecalculateAutoHwm()`과 짝지어 즉시 적용한다.

---

## `Context.PairSocket()` / `DealerSocket()` / `RouterSocket()` / `PubSocket()` / `SubSocket()` / `XPubSocket()` / `XSubSocket()` / `StreamSocket()`

주어진 타입의 socket을 생성한다, caller가 소유. **`*Context`의
method로 선언돼 있다**, 자유 함수가 아니다 — 지금까지 다룬 wrapper
binding 중 유일하게 이런 형태다.

```go
dealer, err := ctx.DealerSocket()
defer dealer.Close()
```

**Options.** 8개 factory 메서드 모두 인자 없음.

**Completion result.** 각각 `(*SocketType, error)`를 반환한다.
caller가 소유하며 context와 독립적으로 반드시 `Close()`해야 한다.

**선택 기준.** 각 socket type의 operation·option·역할은 Sockets
category를 참고한다 — 이 항목은 각각이 어떻게 생성되는지만 다룬다.

---

## `RoutingID`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value
type. value type이다(`struct`, pointer가 아님), 다른 모든 언어의
reference/handle 형태 routing id와 다르다.

```go
fromString := contracts.NewRoutingIDString("worker-3")
fromBytes := contracts.NewRoutingID(rawBytes)
fromUint32 := contracts.NewRoutingIDUint32(42)
restored, err := contracts.NewRoutingIDFromHex(previouslyPrinted.Hex())
```

**Options.** Package-level 생성자; `NewRoutingIDFromHex`를 제외하곤
error를 반환하지 않는다 — 나머지는 잘못된 길이에 panic한다.

| Member | 의미 |
| --- | --- |
| `NewRoutingID(data []byte)` | slice 전체를 그대로 복사; 범위를 벗어나면 panic |
| `NewRoutingIDString(value string)` | UTF-8 인코딩; 범위를 벗어나면 panic |
| `NewRoutingIDUint32(value uint32)` | 4-byte big-endian; 범위를 벗어나면 panic |
| `NewRoutingIDUUIDBytes(value [16]byte)` | 16-byte, 예: UUID byte; 범위를 벗어나면 panic |
| `NewRoutingIDFromHex(value string) (RoutingID, error)` | `Hex()`가 출력한 byte를 복원 — panic 대신 error를 반환하는 유일한 생성자 |
| `Bytes()` | bytes의 방어적 복사 |
| `Size()` | byte 길이, 1-255 |
| `Hash() uint64` | 진단/map-key helper, `RoutingID`가 고정 크기 value struct라는 점에서 Go의 내장 `==` 비교 가능성과는 별개 |
| `Hex()` | hex 인코딩, `NewRoutingIDFromHex`와 왕복 가능 |
| `Equal(other RoutingID) bool` | 값 동등성 |
| `String()` | 표시 형태: printable UTF-8, 그 다음 4-byte를 uint32로, 그 다음 16-byte를 UUID 포맷으로, 마지막 `hex:` 접두 fallback |

**Completion result.** 모든 member는 동기다. `RoutingID`가 순수
value struct라는 건 `==`로 직접 비교 가능하고 `Hash()`/`Equal()`
없이도 map key로 쓸 수 있다는 뜻이다, 다만 둘 다 명시적 대안으로
제공된다.

**선택 기준.** 입력이 잘못됐을 수 있을 땐 특별히(hex-디코딩된 byte에
`NewRoutingID`가 아니라) `NewRoutingIDFromHex`를 쓴다 — 잘못된
입력에 panic하는 대신 error를 보고하는 유일한 routing-id 생성자이기
때문이다.

---

## `Has(capability)`

선택적 빌드 역할을 확인한다.

```go
hasTLS, err := contracts.Has("tls")
```

**Options.** `capability string` — 인식하는 이름은 `"tcp"`, `"ipc"`,
`"tls"`, `"ws"`, `"wss"`; 다른 문자열은 `false`를 반환한다.

**Completion result.** `(bool, error)`를 반환한다 — **절대 실패하지
않는 다른 모든 언어의 `has`/`Has`와 달리**, 여기엔 error 반환이 있다.

**선택 기준.** 모든 transport가 컴파일에 포함됐다고 가정하는 대신
기동 시점에 선택적 transport에 분기하려고 쓴다. 이 binding엔
`Strerror`/`strerror` 대응물이 없다 — 모든 typed error(Errors
category)는 공유 native-errno-to-text 조회를 거치는 대신 자신의
`Error() string` 메서드로 자신만의 메시지를 포맷한다.

---

## `NewStopwatch()` / `NewAtomicCounter()` / `NewThread(target)`

고해상도 stopwatch, thread-safe 정수 counter, 실행 중인 background
thread를 생성한다 — 세 개의 독립된 utility resource, 모두
`utility.go`에 선언됨.

```go
watch := contracts.NewStopwatch()
partialUs := watch.Intermediate()
totalUs := watch.Stop()

counter := contracts.NewAtomicCounter()
newValue := counter.Increment()

thread, err := contracts.NewThread(doWork)
_ = thread.Join()
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewStopwatch()` | resource를 error 없이 직접 반환 — **error 경로 없음**, 이 binding의 다른 대부분의 생성자와 다름 |
| `Stopwatch.Intermediate()` / `Stop()` | 생성 이후 경과 마이크로초(`uint64`), 둘 다 error 반환 없음; `Intermediate()`는 몇 번이든 호출, `Stop()`은 정확히 한 번 호출해 종료 |
| `NewAtomicCounter()` | `NewStopwatch()`와 마찬가지로 resource를 error 없이 직접 반환 |
| `AtomicCounter.Set(int)` | counter 값을 지정, error 반환 없음 |
| `AtomicCounter.Increment()` / `Decrement()` | counter를 1 조정, *새* 값을 반환 |
| `AtomicCounter.Value()` | 현재 값을 읽음 |
| `AtomicCounter.Close()` | counter를 해제, error 반환 없음 |
| `NewThread(target func()) (*Thread, error)` | 새 OS 스레드에서 `target`을 즉시 실행 |
| `Thread.Join() error` | task가 끝날 때까지 block |
| `Thread.Close() error` | thread handle을 해제 |

**Completion result.** `Stopwatch`/`AtomicCounter` 생성과 대부분의
메서드는 error 경로가 전혀 없다. `Thread` 생성과 메서드는 있다.

**선택 기준.** goroutine 전체에서 안전한 공유 count엔
`AtomicCounter`를 쓴다. 벤치마킹엔 `Stopwatch`를 쓴다 —
`Intermediate()`는 몇 번이든 호출하고, `Stop()`은 정확히 한 번
호출한다. zlink 런타임이 밑에 깔린 native thread의 수명주기를
소유해야 할 땐 goroutine 대신 `NewThread`를 쓴다.

---

## `Proxy(...)` / `ProxySteerable(...)` / `Sleep(seconds)` / `MultipartClose(parts)`

두 socket 사이의 양방향 message-forwarding loop을
실행하거나(선택적으로 control socket을 통해 조종 가능), 호출
goroutine을 sleep하거나, multipart slice의 모든 메시지를 닫는다.

```go
contracts.Proxy(frontend, backend, capture) // capture는 nil 가능; context 종료까지 block
contracts.ProxySteerable(frontend, backend, capture, control)
contracts.Sleep(1) // 초 단위, 밀리초 아님
contracts.MultipartClose(parts)
```

**Options.** `Proxy`와 모든 socket type이 `SocketTarget` interface를
만족하므로(Eventing category도 `Poller`/`SocketMonitor` 등록에 이걸
쓴다), 어떤 구체 socket이든 직접 넘길 수 있다.

| Member | 의미 |
| --- | --- |
| `Proxy(frontend, backend, capture SocketTarget) error` | `capture`는 `nil` 가능 |
| `ProxySteerable(frontend, backend, capture, control SocketTarget) error` | 필수 `control` source를 더함 |
| `Sleep(seconds int)` | 호출 goroutine을 block; 반환값이 전혀 없다 — 여기 대부분의 함수가 반환하는 값 없는 `error`조차 없다 |
| `MultipartClose(parts []*Message)` | 모든 part를 한 번에 닫음 |

**Completion result.** `Proxy`/`ProxySteerable`은 `error`를
반환하며 context가 종료될 때까지(또는 `ProxySteerable`의 경우 control
명령이나 에러가 loop을 끝낼 때까지) 호출 goroutine을 block한다 —
둘 중 하나를 전용 goroutine에서 실행한다. `Sleep`/`MultipartClose`는
반환값이 없다.

**선택 기준.** 단순한 fire-and-forget forwarding loop엔 `Proxy`를,
application이 다른 goroutine에서 control socket을 통해 loop을
일시정지·재개·종료해야 할 땐 `ProxySteerable`을 쓴다. 수신되거나
구성된 multipart slice의 모든 메시지를 손으로 짠 loop 대신 한
호출로 해제하려면 `MultipartClose`를 쓴다.

---

[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go),
[`utility.go`](../../../../bindings/go/internal/native/utility.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
