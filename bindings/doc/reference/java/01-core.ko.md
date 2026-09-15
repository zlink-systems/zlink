한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity, `Zlink`
factory/utility class — library의 프로세스 전역 진입점과 utility resource를
다룬다. `Context`의 socket 생성 메서드는 완결성을 위해 여기 나열하되 자세한
내용은 Sockets category에서 다룬다. `Zlink`의 poller/timer 생성도 여기
나열하되 자세한 내용은 Eventing category에서 다룬다. Kotlin은 이 같은
runtime을 공유한다 — Core용 별도 Kotlin contract 소스는 없다. 정확한
signature는
[`contracts/core/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/core/)가
소유한다.

---

## `Zlink.createContext()`

메시징 context를 생성한다 — socket의 factory이자 소유자이며, 이 레퍼런스의
다른 모든 항목의 전제 조건이다.

```java
try (Context context = Zlink.createContext()) {
    // ...
}
```

**옵션.** 인자 없음.

**완료 결과.** `Context`를 동기로 반환한다. caller가 소유하며 반드시
`close()`해야 한다(`Context extends AutoCloseable`) — close하면 그 하위에
아직 열려 있던 socket을 포함해 모든 것이 종료된다.

**선택 기준.** application이 필요로 하는 context마다 한 번 — 대부분은
정확히 하나만 필요하다.

---

## `Context.shutdown()` / `Context.recalculateAutoHwm()`

context의 socket에 대한 blocking operation을 닫지 않고 중단시키거나,
automatic high-water mark의 즉시 재계산을 강제한다.

```java
context.shutdown();
context.recalculateAutoHwm();
```

**옵션.** 둘 다 인자 없음.

**완료 결과.** 둘 다 동기이며 `void`를 반환한다. `shutdown()`은 이 context
하위 socket의 blocking 호출을 중단시키지만 context나 그 socket을 닫지
않는다. `recalculateAutoHwm()`은 여전히 `AutoHwmProfile`로 구성된
socket(Sockets category)에 대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 스레드에서 socket을 쓰는 context를 닫기 전에
`shutdown()`을 호출해 스레드가 무기한 block되는 걸 피한다. `AutoHwmProfile`
변경은 `Context.recalculateAutoHwm()`과 짝지어 즉시 적용한다.

---

## `Context.options()` / `ContextOptions`

context 전역 옵션 facade — I/O thread와 context에서 생성되는 모든 socket이
물려받는 기본값을 관장한다. `ContextOptions`는 public 생성자를 가진다(`new
ContextOptions(context)`), 다만 `context.options()`가 일반적인 경로다.

```java
context.options().ioThreads(8);
context.options().autoHwmProfile(AutoHwmProfile.LOW_LATENCY);
context.options().addThreadAffinityCpu(2);
```

**옵션.**

| Member | 타입 | 의미 |
| --- | --- | --- |
| `ioThreads()`/`ioThreads(int)` | `int` | I/O thread 개수 |
| `maxSockets()`/`maxSockets(int)` | `int` | context 전역 socket 상한 |
| `socketLimit()` | `int`, 읽기 전용 | 빌드의 `maxSockets` 하드캡 |
| `threadPriority()`/`threadPriority(int)` | `int` | dispatch thread 우선순위 |
| `threadSchedulingPolicy()`/`threadSchedulingPolicy(int)` | `int` | dispatch thread 스케줄링 정책 |
| `threadNamePrefix()`/`threadNamePrefix(String)` | `String` | OS에 보이는 dispatch thread 이름 접두; getter는 이 facade instance에 마지막으로 설정된 값을 반환한다, native 재조회가 아니다 |
| `maxMessageSize()`/`maxMessageSize(int)` | `int` | 메시지당 크기 상한 |
| `messageThreadSize()` | `int`, 읽기 전용 | native message struct 크기, 진단 전용 |
| `blocky()`/`blocky(boolean)` | `boolean` | blocking 호출이 실제로 block할지 즉시 실패할지 |
| `autoHwmEnabled()`/`autoHwmEnabled(boolean)` | `boolean` | auto-HWM 크기 조정 활성 여부 |
| `autoHwmRecalcDebounce()`/`autoHwmRecalcDebounce(Duration)` | `Duration` | 자동 재계산 사이 최소 간격 |
| `autoHwmProfile()`/`autoHwmProfile(AutoHwmProfile)` | `AutoHwmProfile` | automatic HWM 크기 profile — Sockets category 참고 |
| `autoHwmMessageUnitBytes()`/`autoHwmMessageUnitBytes(long)` | `long`(unsigned 64-bit bit pattern) | auto-HWM 회계 단위 바이트; `0`이면 socket-type 기본값 선택 |
| `addThreadAffinityCpu(int)` | — | I/O thread를 CPU에 고정(setter만) |
| `removeThreadAffinityCpu(int)` | — | I/O thread를 CPU에서 해제(setter만) |

**완료 결과.** 모든 getter/setter는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에 조정한다.
`autoHwmProfile`/`autoHwmEnabled` 변경은 `Context.recalculateAutoHwm()`과
짝지어 즉시 적용한다.

---

## `Context.createPairSocket()` / `createDealerSocket()` / `createRouterSocket()` / `createPubSocket()` / `createSubSocket()` / `createXPubSocket()` / `createXSubSocket()` / `createStreamSocket()`

주어진 타입의 socket을 생성한다, caller가 소유.

```java
try (DealerSocket dealer = context.createDealerSocket()) {
    // ...
}
```

**옵션.** 8개 factory 메서드 모두 인자가 없다 — 각각 대응하는 interface
(`PairSocket`, `DealerSocket`, `RouterSocket`, `PubSocket`, `SubSocket`,
`XPubSocket`, `XSubSocket`, `StreamSocket`)를 반환한다.

**완료 결과.** 동기. caller가 반환된 socket을 context와 독립적으로
소유·close해야 한다.

**선택 기준.** 각 interface의 연산·옵션은 Sockets category를 참고한다 — 이
항목은 생성만 다룬다.

---

## `RoutingId`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value type
(`MAX_LENGTH`, public 상수). 내부적으로 receive hot path에서 재할당을
피하려고 스레드별 trusted-bytes 캐시를 유지한다 — public contract 표면이
아니다.

```java
RoutingId fromString = RoutingId.from("worker-3");
RoutingId fromBytes = RoutingId.from(rawBytes);
RoutingId fromRange = RoutingId.from(buffer, offset, length);
RoutingId fromUint = RoutingId.from(42L);
RoutingId fromUuid = RoutingId.from(UUID.randomUUID());
RoutingId restored = RoutingId.fromHex(previouslyPrinted.toHex());
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `from(byte[])` | 전체 배열을 그대로 복사 |
| `from(byte[] value, int offset, int length)` | 선택한 byte 범위 복사 — dotnet/cpp엔 없는 Java 고유 overload |
| `from(String)` | UTF-8 인코딩 |
| `from(long)` | unsigned 32-bit 값에서 4-byte big-endian; 32비트에 안 맞으면 `IllegalArgumentException` |
| `from(UUID)` | 16-byte big-endian |
| `fromHex(String)` | `toHex()`가 출력한 byte 복원 |
| `toBytes()` | byte의 방어적 복사 |
| `size()` | byte 길이, 1-255 |
| `toHex()` | `fromHex`와 round-trip 가능한 hex 인코딩 |
| `toString()` | 표시용 형태: printable UTF-8, 그다음 4-byte를 unsigned int로, 그다음 16-byte를 UUID로, 마지막 `hex:` 접두 fallback |
| `equals`/`hashCode` | 값 동등성 |

**완료 결과.** 모든 factory·accessor는 동기다. 범위를 벗어난 길이는
`IllegalArgumentException`을 던진다. `fromHex`에 잘못된 hex 문자열을 주면
마찬가지다.

**선택 기준.** 사람이 부여한 identity엔 `from(String)`을, 숫자·UUID 형태
identity엔 `from(long)`/`from(UUID)`를, 이미 binary이거나 더 큰 buffer의
slice인 identity엔 raw byte overload(범위 overload 포함)를 쓴다. 내구성
있는 round trip엔 `toHex()`/`fromHex()`를 쓴다 — `toString()`은 표시
전용이다.

---

## `Zlink.strerror(int)` / `Zlink.has(String)` / `Zlink.version()` / `ZlinkVersion.get()`

native error code를 메시지로 변환하거나, 선택적 빌드 역할을 확인하거나,
native library의 빌드 버전을 읽는다.

```java
String message = Zlink.strerror(errnum);
boolean hasTls = Zlink.has("tls");
int[] version = Zlink.version();
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `strerror(int errnum)` | 그 native error code의 메시지 텍스트 |
| `has(String capability)` | 이름 붙은 선택적 역할이 이 빌드에 컴파일됐는지 — 인식하는 이름은 `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; 그 외 문자열은 `false` |
| `version()` / `ZlinkVersion.get()` | `{major, minor, patch}`의 `int[]`; 동등하다 — `ZlinkVersion`은 `Zlink.version()`에 위임하는 얇은 편의 wrapper |

**완료 결과.** 모두 동기다. **`Zlink.errno()`는 소스에 존재하지만 `public`
수식어가 없다** — application 코드에서 도달할 수 없다.

**선택 기준.** 동적으로 로드된 native library가 기대와 일치하는지
확인하려면 `version()`을 쓴다. 기동 시점에 선택적 transport를 분기하려면
`has(...)`를 쓴다. `strerror`는 다른 곳(Errors category)에서 드러난 native
error code와 함께 진단할 때 쓴다.

---

## `Zlink.createAtomicCounter()` / `Zlink.createStopwatch()` / `Zlink.createThread(Runnable)`

thread-safe 정수 counter, 고해상도 stopwatch, 실행 중인 background thread를
생성한다.

```java
try (AtomicCounter counter = Zlink.createAtomicCounter()) {
    int newValue = counter.increment();
}

try (ZlinkStopwatch watch = Zlink.createStopwatch()) {
    Duration partial = watch.intermediate();
    Duration total = watch.stop();
}

try (ZlinkThread thread = Zlink.createThread(() -> doWork())) {
    thread.join();
}
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `createAtomicCounter()` | 인자 없음 |
| `AtomicCounter.set(int)` | counter 값을 대입 |
| `AtomicCounter.increment()`/`decrement()` | counter를 1만큼 조정, *새* 값을 반환 |
| `AtomicCounter.value()` | 현재 값을 읽음 |
| `createStopwatch()` | 인자 없음 |
| `ZlinkStopwatch.intermediate()` | 생성 이후 경과 `Duration`, 몇 번이든 호출 가능 |
| `ZlinkStopwatch.stop()` | 생성 이후 경과 `Duration`, 마치려고 정확히 한 번 호출 |
| `createThread(Runnable task)` | `task`를 새 스레드에서 즉시 실행 |
| `ZlinkThread.join()` | task가 끝날 때까지 block |

**완료 결과.** 세 factory 모두 자신의 resource를 동기로 반환한다 —
caller가 각각을 소유하고 close해야 한다(셋 다 `AutoCloseable`).

**선택 기준.** 스레드 전체에서 안전한 공유 count엔 `createAtomicCounter`를
쓴다. 벤치마킹엔 `createStopwatch`를 쓴다. zlink 런타임이 수명주기를
소유해야 할 땐 `java.lang.Thread`를 직접 쓰는 대신 `createThread`를 쓴다.

---

## `Zlink.proxy(...)` / `Zlink.proxySteerable(...)` / `Zlink.sleep(Duration)`

두 socket 사이의 양방향 message-forwarding loop을 실행하거나(선택적으로
control socket으로 조종 가능), 호출 스레드를 sleep한다.

```java
Zlink.proxy(frontend, backend, capture); // capture는 null 가능; context 종료까지 block
Zlink.proxySteerable(frontend, backend, capture, control);
Zlink.sleep(Duration.ofSeconds(1));
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `proxy(Socket frontend, Socket backend, Socket capture)` | `capture`는 `null` 가능 |
| `proxySteerable(Socket frontend, Socket backend, Socket capture, Socket control)` | 필수 `control` socket 추가 |
| `sleep(Duration)` | 호출 스레드를 block |

**완료 결과.** 셋 다 동기이며 반환값이 없다. `proxy`/`proxySteerable`은
context가 종료될 때까지(또는 `proxySteerable`의 경우 control 명령이나
오류가 loop를 끝낼 때까지) 호출 스레드를 block한다 — 둘 다 전용
스레드에서 실행한다. **`sleep(Duration)`만 public이다** —
`Zlink.sleep(int seconds)`와 `Zlink.multipartClose(Message[])`는 소스에
존재하지만 `public` 수식어가 없어 application 코드에서 도달할 수 없다,
dotnet의 public `Zlink.Sleep(TimeSpan)`/`Zlink.MultipartClose(...)` 짝과
다르다.

**선택 기준.** 단순 fire-and-forget forwarding loop엔 `proxy`를, 다른
스레드에서 `control`로 loop을 일시정지·재개·종료해야 하면
`proxySteerable`을 쓴다. public `multipartClose` 대응물이 없으므로 각
part는 `Message.close()`(Messaging category)로 개별 close한다.

---

[`contracts/core/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/core/)와
[Java 바인딩 스펙](../../spec/java/README.ko.md)에서 전체 근거를 확인한다.
