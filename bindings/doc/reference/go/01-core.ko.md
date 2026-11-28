한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity,
package-level utility 함수를 다룬다. **socket 생성은 `Context`의
method다**(`ctx.PairSocket()`, ...). 정확한
signature는
[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go)와
[`utility.go`](../../../../bindings/go/internal/native/utility.go)가
소유하며,
[`root_projection.go`](../../../../bindings/go/root_projection.go)를 통해
alias로 re-export된다.

---

## `NewContext()`

메시징 context를 생성한다 — socket의 factory이자 소유자.

```go
ctx, err := zlink.NewContext()
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

**선택 기준.** 여러 goroutine에서 socket을 사용하는 중인 context엔
`Close()` 전에 `Shutdown()`을 호출한다. 바꾼 auto-HWM profile을 즉시
적용하려면 `RecalculateAutoHwm()`을 호출한다.

---

## `Context.Options()`

I/O thread와, context에서 생성되는 모든 socket이 상속하는 기본값을
관장하는, 짝을 이루는 getter/setter 메서드를 가진 context-wide option
facade를 반환한다.

```go
opts := ctx.Options()
opts.SetIOThreads(8)
opts.SetCoreHwmProfile(zlink.AutoHwmProfileLowLatency)
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
| `MessageStructSize()` | 읽기 전용, native message struct 크기, 진단 전용 |
| `Blocky()` / `SetBlocky(bool)` | blocking 호출이 실제로 block하는지 fail fast하는지 |
| `CoreHwmProfile()` / `SetCoreHwmProfile(AutoHwmProfile)` | automatic HWM sizing profile — Sockets category 참고 |
| `CoreHwmMemoryLimitBytes()` / `SetCoreHwmMemoryLimitBytes(uint64)` | 설정한 메모리 한도를 바이트로 읽거나 설정한다. |
| `CoreHwmBudgetBytes()` / `SetCoreHwmBudgetBytes(uint64)` | 설정한 Core 예산을 바이트로 읽거나 설정한다. |
| `AddThreadAffinity(cpu int)` | setter만, I/O thread를 CPU에 고정 |
| `RemoveThreadAffinity(cpu int)` | setter만, I/O thread의 CPU 고정을 해제 |

**Completion result.** 모든 accessor는 동기이며, 값과 함께 `error`를
반환한다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에
조정한다. `CoreHwmProfile`/`AutoHwmEnabled` 변경은
`Context.RecalculateAutoHwm()`과 짝지어 즉시 적용한다.

---

## `Context.PairSocket()` / `DealerSocket()` / `RouterSocket()` / `PubSocket()` / `SubSocket()` / `XPubSocket()` / `XSubSocket()` / `StreamSocket()`

`*Context`의 method가 지정된 타입의 socket을 생성하고 caller가 소유한다.

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

`RoutingID`는 1~255바이트를 담는 값 타입이다. Go에서 직접 비교하거나 맵의 키로 사용할 수 있다.

```go
fromString := zlink.NewRoutingIDString("worker-3")
fromUint32 := zlink.NewRoutingIDUint32(42)
fromBytes := zlink.NewRoutingID(rawBytes)
restored, err := zlink.TryNewRoutingIDFromHex(previouslyPrinted.Hex())
```

| 메서드 | 의미 |
| --- | --- |
| `NewRoutingID(data []byte) RoutingID` / `NewRoutingIDString(value string) RoutingID` | 입력 바이트를 복사한다. 길이가 1~255바이트를 벗어나면 영값을 반환한다. |
| `NewRoutingIDFromHex(value string) RoutingID` | `Hex()`의 출력을 해석한다. 잘못된 입력에는 영값을 반환한다. |
| `TryNewRoutingID(data []byte) (RoutingID, error)` / `TryNewRoutingIDFromHex(value string) (RoutingID, error)` | 잘못된 입력은 즉시 `ConfigInvalidArgument`인 `*ConfigError`로 반환한다. |
| `NewRoutingIDUint32(value uint32) RoutingID` / `Uint32() (uint32, bool)` | 4바이트 big-endian 값으로 만들거나 읽는다. |
| `NewRoutingIDUUIDBytes(value [16]byte) RoutingID` / `UUIDBytes() ([16]byte, bool)` | UUID의 16바이트로 만들거나 읽는다. |
| `Bytes()` / `Size()` / `Hex()` | 바이트 사본, 바이트 길이 또는 16진수 문자열을 반환한다. |
| `Equal(other RoutingID) bool` / `Hash() uint64` | 바이트를 비교하거나 해시값을 계산한다. |
| `String()` | 표시용 문자열을 출력 가능한 UTF-8, 4바이트의 10진수 uint32, 16바이트의 UUID 표기, `hex:` 접두사가 붙은 16진수 순으로 선택한다. |

`Uint32()`와 `UUIDBytes()`는 기대하는 바이트 길이가 다르면 `ok=false`를 반환한다.

**Completion result.** 모든 member는 동기다. ID는 `==` 또는 `Equal()`로 비교한다.
`RoutingID`는 map key로 직접 쓸 수 있다. `Hash()`는 해시값을 반환한다.

**선택 기준.** 나중에 복원할 ID는 `Hex()`로 저장한다. 잘못된 16진수 입력을 오류로
처리하려면 `TryNewRoutingIDFromHex`를, 잘못된 바이트 슬라이스를 오류로 처리하려면
`TryNewRoutingID`를 사용한다.

---

## `Has(capability)`

선택적 빌드 역할을 확인한다.

```go
hasTLS, err := zlink.Has("tls")
```

**Options.** `capability string` — 인식하는 이름은 `"tcp"`, `"ipc"`,
`"tls"`, `"ws"`, `"wss"`; 다른 문자열은 `false`를 반환한다.

**Completion result.** `(bool, error)`를 반환한다 — **절대 실패하지
않는 다른 모든 언어의 `has`/`Has`와 달리**, 여기엔 error 반환이 있다.

**선택 기준.** 모든 transport가 컴파일에 포함됐다고 가정하는 대신
기동 시점에 선택적 transport에 분기하려고 사용한다. 이 binding엔
`Strerror`/`strerror` 대응물이 없다 — 모든 typed error(Errors
category)는 공유 native-errno-to-text 조회를 거치는 대신 자신의
`Error() string` 메서드로 자신만의 메시지를 포맷한다.

---

## `NewStopwatch()` / `NewAtomicCounter()` / `NewThread(target)`

고해상도 stopwatch, thread-safe 정수 counter, 실행 중인 background
thread를 생성한다 — 세 개의 독립된 utility resource, 모두
`utility.go`에 선언됨.

```go
watch := zlink.NewStopwatch()
partialUs := watch.Intermediate()
totalUs := watch.Stop()

counter := zlink.NewAtomicCounter()
newValue := counter.Increment()

thread, err := zlink.NewThread(doWork)
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
`AtomicCounter`를 사용한다. 벤치마킹엔 `Stopwatch`를 사용한다 —
`Intermediate()`는 몇 번이든 호출하고, `Stop()`은 정확히 한 번
호출한다. zlink 런타임이 밑에 깔린 native thread의 수명주기를
소유해야 할 땐 goroutine 대신 `NewThread`를 사용한다.

---

## `Proxy(...)` / `Sleep(seconds)` / `MultipartClose(parts)`

`Proxy`는 두 socket 사이에서 메시지를 양방향으로 전달한다. 선택한 캡처 socket에 트래픽을 복사할
수 있다.

```go
err := zlink.Proxy(frontend, backend, capture) // capture에는 nil을 전달할 수 있다.
zlink.Sleep(1) // 단위는 초다.
zlink.MultipartClose(parts)
```

**Options.** 여덟 socket 타입은 모두 `SocketTarget`을 구현하므로
`Proxy`에 직접 넘길 수 있다.

| Member | 의미 |
| --- | --- |
| `Proxy(frontend, backend, capture SocketTarget) error` | 캡처를 생략하려면 `nil`을 전달한다. |
| `Sleep(seconds int)` | 지정한 초만큼 호출 goroutine을 대기시킨다. |
| `MultipartClose(parts []*Message)` | 슬라이스의 메시지를 각각 닫는다. |

**Completion result.** `Proxy`는 context가 종료되거나 전달에 실패할 때까지
대기한 뒤 `error`를 반환한다. `Sleep`과 `MultipartClose`는 반환값이 없다.

**선택 기준.** 메시지 전달 중에도 caller가 계속 실행돼야 하면 `Proxy`를 별도
goroutine에서 실행한다. 수신하거나 구성한 multipart 슬라이스의 메시지를
한꺼번에 닫을 때는 `MultipartClose`를 사용한다.

---

[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go),
[`utility.go`](../../../../bindings/go/internal/native/utility.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
