한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context lifecycle, context 옵션, routing identity, `Zlink` static facade —
library의 프로세스 전역 진입점과 utility resource를 다룬다. `IContext`의 socket 생성
메서드는 완결성을 위해 여기 나열하되 자세한 내용은 Sockets category에서 다룬다. `Zlink`의
poller/timer 생성도 여기 나열하되 자세한 내용은 Eventing category에서 다룬다. 정확한
signature는 [`Contracts/Core/`](../../../../bindings/dotnet/src/Zlink/Contracts/Core/)가
소유한다.

---

## `Zlink.CreateContext()`

Messaging context — socket의 factory이자 소유자 — 를 만든다. 이 레퍼런스의 다른 모든
항목의 전제 조건이다.

```csharp
using IContext context = Zlink.CreateContext();
```

**옵션.** 매개변수 없음.

**완료 결과.** 동기로 `IContext`를 반환한다. Caller가 소유하며 해제해야 한다
(`IDisposable`/`IAsyncDisposable`) — 해제하면 그 아래 열려 있던 socket도 함께 종료된다.

**선택 기준.** Application이 필요로 하는 context마다 한 번 — 대부분은 정확히 하나만
필요하다.

---

## `IContext.Shutdown()` / `IContext.RecalculateAutoHwm()`

Context의 socket에 대한 blocking operation을 해제하지 않고 중단시키거나, automatic
high-water mark의 즉시 재계산을 강제한다.

```csharp
context.Shutdown();
context.RecalculateAutoHwm();
```

**옵션.** 둘 다 매개변수가 없다.

**완료 결과.** 둘 다 동기이며 `void`를 반환한다. `Shutdown`은 context 아래 socket의
blocking 호출을 중단시키지만 context나 그 socket을 해제하지 않는다. `RecalculateAutoHwm`은
여전히 `AutoHwmProfile`로 구성된 socket에 대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 스레드에서 socket을 쓰는 context를 해제하기 전에 `Shutdown`을 호출해
스레드가 무한정 block되는 걸 피한다. `AutoHwmProfile` 변경은 `RecalculateAutoHwm`과 짝지어
일반 갱신 경로를 기다리지 않고 즉시 적용한다.

---

## `IContext.Options`

Context 전역 옵션 facade — I/O thread와 context에서 만들어지는 모든 socket이 물려받는
기본값을 관장한다.

```csharp
context.Options.IoThreads = 8;
context.Options.AutoHwmProfile = AutoHwmProfile.LowLatency;
context.Options.AddThreadAffinityCpu(2);
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `IoThreads` | 1 | dispatch thread 개수 |
| `MaxSockets` | 1023 | context 전역 socket 상한 |
| `SocketLimit` | 읽기 전용 | 빌드의 `MaxSockets` 하드캡 |
| `ThreadPriority` | OS 기본값 | dispatch thread 우선순위 |
| `ThreadSchedulingPolicy` | OS 기본값 | dispatch thread 스케줄링 정책 |
| `MaxMessageSize` | 무제한 | 메시지당 크기 상한 |
| `MessageThreadSize` | 읽기 전용 | native message struct 크기, 진단 전용 |
| `Blocky` | `true` | blocking 호출이 실제로 block할지 즉시 실패할지 |
| `AutoHwmProfile` | `Balanced` | automatic HWM 크기 profile — Sockets category 참고 |
| `AutoHwmMessageUnitBytes` | profile 기본값 | auto-HWM 회계 단위 바이트(`ulong`) |
| `AutoHwmEnabled` | `true` | auto-HWM 크기 조정 활성 여부 |
| `AutoHwmRecalcDebounce` | profile 기본값 | 자동 재계산 사이 최소 간격 |
| `ThreadNamePrefix` | 없음 | OS에 보이는 dispatch thread 이름 접두 |
| `AddThreadAffinityCpu(cpu)` / `RemoveThreadAffinityCpu(cpu)` | 없음 | dispatch thread를 특정 CPU에 고정/해제 |

**완료 결과.** 모든 get/set과 두 affinity 메서드는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않으면 socket을 만들기 전에 조정한다.
`AutoHwmProfile`/`AutoHwmEnabled`는 실행 중인 context에서도 바꿀 수 있다 — 즉시 적용하려면
위 `RecalculateAutoHwm`과 짝짓는다.

---

## `IContext.CreatePairSocket()` / `CreateDealerSocket()` / `CreateRouterSocket()` / `CreatePubSocket()` / `CreateSubSocket()` / `CreateXPubSocket()` / `CreateXSubSocket()` / `CreateStreamSocket()`

주어진 타입의 socket을 만들며, caller가 소유한다.

```csharp
using IDealerSocket dealer = context.CreateDealerSocket();
```

**옵션.** 여덟 factory 메서드 모두 매개변수가 없다 — 각각 대응하는 interface
(`IPairSocket`, `IDealerSocket`, `IRouterSocket`, `IPubSocket`, `ISubSocket`, `IXPubSocket`,
`IXSubSocket`, `IStreamSocket`)를 반환한다.

**완료 결과.** 동기. Caller가 반환된 socket을 context와 독립적으로 소유·해제해야 한다.

**선택 기준.** 각 interface의 연산·옵션은 Sockets category를 참고한다 — 이 항목은 생성만
다룬다.

---

## `RoutingId`

Messaging peer나 route를 식별하는 1~255바이트 binary-safe value type이다.

```csharp
RoutingId fromString = RoutingId.From("worker-3");
RoutingId fromBytes = RoutingId.From(rawBytes);
RoutingId fromUint = RoutingId.From(42u);
RoutingId fromGuid = RoutingId.From(Guid.NewGuid());
RoutingId restored = RoutingId.FromHex(previouslyPrinted.ToHex());
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `From(ReadOnlySpan<byte>)` / `From(byte[])` | — | raw 바이트를 그대로 복사 |
| `From(string)` | — | UTF-8 인코딩 |
| `From(uint)` | — | 4바이트 big-endian |
| `From(Guid)` | — | 16바이트 big-endian |
| `FromHex(string)` | — | `ToHex()`가 출력한 바이트 복원 |
| `Size` / `IsEmpty` | — | 길이 / 길이 0 여부 |
| `ToBytes()` | — | 내부 storage 기반 view |
| `ToHex()` | — | `FromHex`와 round-trip 가능 |
| `TryToUInt32(out uint)` / `TryToGuid(out Guid)` | — | typed decode, 형태 불일치면 `false` |
| `ToString()` | — | 표시 전용: printable UTF-8, 그다음 `uint`, 그다음 `Guid`, 그다음 `hex:` prefix fallback |
| `Equals`/`==`/`!=`/`GetHashCode` | — | 값 동등성 |

**완료 결과.** 모두 동기다. 범위 밖 바이트 길이(1..255 아님)는
`ArgumentOutOfRangeException`을, `FromHex`의 잘못된 hex 문자열은 `ArgumentException`을
던진다.

**선택 기준.** 사람이 부여한 identity엔 `From(string)`, 숫자·GUID 형태 identity엔
`From(uint)`/`From(Guid)`, 이미 binary인 identity엔 raw `From(byte[])`를 쓴다. 안정적인
raw-byte round trip엔 `ToHex()`/`FromHex()`를 쓴다 — `ToString()`은 표시 전용이다.

---

## `Zlink.Version()` / `Zlink.Strerror(int)` / `Zlink.Has(string)`

Native library의 빌드 버전을 읽거나, native error code를 메시지로 바꾸거나, 선택적 빌드
capability를 확인한다.

```csharp
var (major, minor, patch) = Zlink.Version();
string message = Zlink.Strerror(errnum);
bool hasTls = Zlink.Has("tls");
```

**옵션.**

| Member | 매개변수 | 의미 |
| --- | --- | --- |
| `Version()` | 없음 | 링크된 native library 버전 |
| `Strerror(errnum)` | `int` error code | native errno 텍스트 |
| `Has(capability)` | `"tcp"`/`"ipc"`/`"tls"`/`"ws"`/`"wss"`; 그 외 문자열은 `false` | 선택적 빌드 capability 확인 |

**완료 결과.** 모두 동기다. `Version()`은 `(int Major, int Minor, int Patch)` tuple을,
`Strerror`는 `string`을, `Has`는 `bool`을 반환한다.

**선택 기준.** 동적으로 로드된 native library가 기대와 일치하는지 확인하려면
`Version()`을 쓴다. Startup에 선택적 transport를 분기하려면 `Has(...)`를 쓴다.
`Strerror`는 다른 곳에서 드러난 native error code와 함께 진단할 때 쓴다.

---

## `Zlink.CreateAtomicCounter()` / `Zlink.CreateStopwatch()` / `Zlink.CreateThread(Action)`

스레드에 안전한 정수 counter, 고해상도 stopwatch, 실행 중인 백그라운드 thread를 만든다.

```csharp
using IAtomicCounter counter = Zlink.CreateAtomicCounter();
int newValue = counter.Increment();

using IZlinkStopwatch watch = Zlink.CreateStopwatch();
ulong partialUs = watch.Intermediate();
ulong totalUs = watch.Stop();

using IZlinkThread thread = Zlink.CreateThread(() => DoWork());
thread.Join();
```

**옵션.**

| Member | 반환 | 의미 |
| --- | --- | --- |
| `CreateAtomicCounter()` | `IAtomicCounter` | 매개변수 없음 |
| `IAtomicCounter.Value` | `int` | get |
| `IAtomicCounter.Set(value)` / `.Increment()` / `.Decrement()` | `int` | 뒤 둘은 이전 값이 아니라 *새* 값을 반환 |
| `CreateStopwatch()` | `IZlinkStopwatch` | 매개변수 없음 |
| `IZlinkStopwatch.Intermediate()` / `.Stop()` | `ulong` 마이크로초 | `Intermediate()`는 몇 번이든, `Stop()`은 한 번 |
| `CreateThread(Action task)` | `IZlinkThread` | `task`가 새 thread에서 즉시 실행 |
| `IZlinkThread.Join()` | — | 작업이 끝날 때까지 block, 반복 호출은 no-op |
| `IZlinkThread.Close()` | — | 실행 중이면 먼저 join한 뒤 handle 해제 |

**완료 결과.** 세 factory 모두 자신의 resource interface를 동기로 반환한다 — caller가
각각을 소유하고 해제해야 한다(`IDisposable`/`IAsyncDisposable`).

**선택 기준.** 스레드 사이 공유 count엔 `CreateAtomicCounter`, benchmarking엔
`CreateStopwatch`, 플랫폼 전용 API 대신 portable 백그라운드 thread엔 `CreateThread`를
쓴다.

---

## `Zlink.Proxy(...)` / `Zlink.ProxySteerable(...)` / `Zlink.Sleep(TimeSpan)` / `Zlink.MultipartClose(...)`

두 socket 사이의 양방향 메시지 forwarding loop를 실행하거나(선택적으로 control
socket으로 조종 가능), 호출한 스레드를 재우거나, multipart payload의 모든 메시지를
해제한다.

```csharp
Zlink.Proxy(frontend, backend, capture); // capture는 null일 수 있다; context 종료까지 block한다
Zlink.ProxySteerable(frontend, backend, capture, control); // control이 런타임 명령을 받는다
Zlink.Sleep(TimeSpan.FromSeconds(1));
Zlink.MultipartClose(parts);
```

**옵션.**

| Member | 매개변수 | 의미 |
| --- | --- | --- |
| `Proxy(frontend, backend, capture)` | `IZlinkSocket` frontend/backend(필수), capture(선택) | context 종료까지 forward |
| `ProxySteerable(frontend, backend, capture, control)` | 필수 `control` socket 추가 | `control`로 pause/resume 가능 |
| `Sleep(TimeSpan)` | duration | 호출한 스레드를 block |
| `MultipartClose(IReadOnlyList<Message>)` | 해제할 part | 한 번에 모든 메시지 해제 |

**완료 결과.** 넷 다 동기이며 반환값이 없다. `Proxy`/`ProxySteerable`은 context가
종료될 때까지(또는 `ProxySteerable`의 경우 `TERMINATE` 명령이나 오류가 loop를 끝낼
때까지) 호출한 스레드를 block한다 — 둘 다 전용 스레드에서 실행한다.

**선택 기준.** 단순 fire-and-forget forwarding loop엔 `Proxy`, 다른 스레드에서
`control`로 loop를 멈추거나·재개하거나·종료해야 하면 `ProxySteerable`을 쓴다.
수신·구성된 multipart 배열을 한 번에 해제하려면 `MultipartClose`를 쓴다.

---

## `Zlink.CreatePoller()` / `Zlink.CreateTimer()`

재사용 가능한 poller나 독립 timer를 만든다.

```csharp
using IPoller poller = Zlink.CreatePoller();
using IZlinkTimer timer = Zlink.CreateTimer();
```

**옵션.** 두 factory 모두 매개변수가 없다(timer를 Spot의 lifecycle에 묶는
`CreateTimer(ISpot)` overload도 있다 — Service category).

**완료 결과.** 둘 다 자신의 resource interface(`IPoller`, `IZlinkTimer`)를 동기로
반환한다 — caller가 각각을 소유하고 해제해야 한다.

**선택 기준.** `IPoller`/`IZlinkTimer` 자신의 연산은 Eventing category를 참고한다 — 이
항목은 생성만 다룬다.

---

## `Zlink.UnhandledCallbackException`

사용자 callback이 예외를 던질 때 발생하는 static event다.

```csharp
Zlink.UnhandledCallbackException += ex => logger.LogError(ex, "callback failed");
```

**옵션.** `Action<Exception>`을 구독·구독 해제한다.

**완료 결과.** 동기 add/remove다. 예외를 던진 callback을 실행하는 백그라운드 dispatch
thread에서 이 event가 발생한다 — 그 스레드는 원래 caller에게 예외를 전파할 수 없다.

**선택 기준.** 등록된 어떤 callback(stream packet, monitor, poll, SPOT dispatch,
request/reply — Sockets/Eventing/Service category)에서든 예외를 관찰하려면 여기를
구독한다 — 구독하지 않으면 조용히 사라진다.

---

전체 근거는 [`Contracts/Core/`](../../../../bindings/dotnet/src/Zlink/Contracts/Core/)와
[.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)을 참고한다.
