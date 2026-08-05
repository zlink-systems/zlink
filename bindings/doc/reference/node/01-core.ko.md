한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity, package
최상위에서 export되는 factory/utility 함수를 다룬다. **지금까지 다룬 다른
모든 wrapper binding과 달리, 이 factory 함수는 `contracts/core/` 아래
선언돼 있지 않다** — static class facade가 아니라 Node/JS의 module-export
관례를 따라
[`src/index.ts`](../../../../bindings/node/src/index.ts)에서 export되는
순수 함수다. `AtomicCounter`/`Stopwatch`/`Thread`도 소스에선
`contracts/core/`가 아니라 `contracts/eventing/timer.ts`에 물리적으로
선언돼 있다 — Node 자신의 소스 레이아웃이 아니라 관례(다른 모든 언어의
배치와 맞춤)에 따라 이 Core category로 묶었다. 정확한 signature는
[`contracts/core/`](../../../../bindings/node/src/zlink/contracts/core/)와
[`src/index.ts`](../../../../bindings/node/src/index.ts)가 소유한다.

---

## `createContext()`

메시징 context를 생성한다 — socket의 factory이자 소유자.

```ts
const ctx = createContext();
```

**Options.** 인자 없음.

**Completion result.** `Context`를 동기로 반환한다. caller가 소유하며
반드시 `ctx.close()`를 호출해야 한다 — close하면 그 하위에 아직 열려 있는
모든 것(context에서 생성된 socket 포함)이 종료된다.

**선택 기준.** application이 필요로 하는 context마다 한 번 호출한다 —
대부분의 application은 정확히 하나가 필요하다.

---

## `Context.shutdown()` / `Context.recalculateAutoHwm()`

context의 socket이 닫히지 않은 채 blocking operation을 인터럽트하거나,
automatic high-water mark의 즉시 재계산을 강제한다.

```ts
ctx.shutdown();
ctx.recalculateAutoHwm();
```

**Options.** 둘 다 인자 없음.

**Completion result.** 둘 다 반환값 없이 동기다. `shutdown()`은 이 context
하위 socket의 blocking 호출을 인터럽트하지만 context나 그 socket을 닫지
않는다. `recalculateAutoHwm()`은 아직 `AutoHwmProfile`이 설정된 socket에
대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 스레드/worker에서 socket을 쓰는 중인 context엔
`close()` 전에 `shutdown()`을 호출한다. auto-HWM profile이나
message-unit option을 바꾼 후엔 새 sizing을 즉시 적용하려고
`recalculateAutoHwm()`을 호출한다.

---

## `Context.options`

`Context`의 `options` getter로 읽는 context-wide option facade.

```ts
ctx.options.ioThreads = 8;
ctx.options.autoHwmProfile = AutoHwmProfile.LowLatency;
ctx.options.addThreadAffinity(2);
```

**Options.** 별도 표기 없으면 순수 property는 get/set 둘 다 가능하다.

| Member | 타입 | 의미 |
| --- | --- | --- |
| `ioThreads` | `number` | I/O thread 개수 |
| `maxSockets` | `number` | context 전체 socket 상한 |
| `socketLimit` | `number`, 읽기 전용 | `maxSockets`의 빌드상 하드 상한 |
| `maxMsgSize` | `number` | 메시지 하나의 크기 상한 |
| `msgTSize` | `number`, 읽기 전용 | native message struct 크기, 진단 전용 |
| `threadPriority` / `threadSchedulingPolicy` | `number` | dispatch thread 우선순위 / scheduling policy |
| `blocky` | `boolean` | blocking 호출이 실제로 block하는지 fail fast하는지 |
| `autoHwmEnabled` | `boolean` | auto-HWM sizing 활성 여부 |
| `autoHwmRecalcDebounceMs` | `number` | 자동 재계산 사이 최소 간격 |
| `autoHwmProfile` | `AutoHwmProfileValue` | automatic HWM sizing profile — Sockets category 참고 |
| `autoHwmMsgUnitBytes` | `bigint`, unsigned 64-bit planning unit | auto-HWM용 accounted-byte 단위; `0n`은 socket-type 기본값 선택 |
| `threadNamePrefix` | `string` | OS에 보이는 dispatch thread 이름 접두사 |
| `addThreadAffinity(cpu: number)` | method | I/O thread를 CPU에 고정 |
| `removeThreadAffinity(cpu: number)` | method | I/O thread의 CPU 고정을 해제 |

**Completion result.** 모든 property 읽기/쓰기와 두 method는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에
조정한다. `autoHwmProfile`/`autoHwmEnabled` 변경은
`Context.recalculateAutoHwm()`과 짝지어 즉시 적용한다.

---

## `createPairSocket(ctx)` / `createDealerSocket(ctx)` / `createRouterSocket(ctx)` / `createPubSocket(ctx)` / `createSubSocket(ctx)` / `createXPubSocket(ctx)` / `createXSubSocket(ctx)` / `createStreamSocket(ctx)`

context로부터 주어진 타입의 socket을 생성한다, caller가 소유.

```ts
const dealer = createDealerSocket(ctx);
```

**Options.** 각 factory는 소유 `Context`를 받는다.

**Completion result.** 각각 대응하는 socket interface를 동기로 반환한다.
caller가 소유하며 context와 독립적으로 반드시 `close()`해야 한다.

**선택 기준.** 각 socket interface의 operation·option·역할은 Sockets
category를 참고한다 — 이 항목은 각각이 어떻게 생성되는지만 다룬다.

---

## `createPoller()` / `createTimer()` / `createPollEvents(capacity)`

재사용 가능한 poller, standalone timer, poll-result buffer를 생성한다.

```ts
const poller = createPoller();
const timer = createTimer();
const events = createPollEvents(8);
```

**Options.** `createPoller()`/`createTimer()`는 인자 없음.
`createPollEvents(capacity: number)`는 buffer의 고정 결과 용량을 받는다.

**Completion result.** 셋 다 자신의 resource를 동기로 반환한다. caller가
소유하며 각각 반드시 `close()`해야 한다.

**선택 기준.** `Poller`, `Timer`, `PollEvents` 자신의 operation은 Eventing
category를 참고한다 — 이 항목은 생성만 다룬다.

---

## `RoutingId`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value type.

```ts
const fromString = RoutingId.from('worker-3');
const fromBytes = RoutingId.from(rawBuffer);
const fromUint32 = RoutingId.from(42);
const restored = RoutingId.fromHex(previouslyPrinted.toHex());
```

**Options.** instance는 freeze돼 있으며(`Object.freeze`) `from`/`fromHex`로만
생성 가능하다 — 생성자 자체는 private이며 내부 token으로 보호된다.

| Member | 의미 |
| --- | --- |
| `from(value: string \| Buffer \| Uint8Array \| number)` | `string`은 UTF-8로 인코딩되고, `number`는 4-byte big-endian uint32가 되며(`0..4294967295` 범위의 정수여야 함), `Buffer`/`Uint8Array`는 그대로 복사된다. dotnet/java와 달리 전용 UUID 타입 overload가 없다 — 16-byte 값도 `Buffer`/`Uint8Array` 경로로 생성한다 |
| `fromHex(value: string)` | `toHex()`가 출력한 byte를 복원 |
| `size` | getter, byte 길이, 1-255 |
| `toBytes()` | bytes의 방어적 복사 |
| `toHex()` | hex 인코딩, `fromHex`와 왕복 가능 |
| `toString()` | 표시 형태: printable UTF-8, 그 다음 4-byte를 uint32로, 그 다음 16-byte를 UUID 포맷으로, 마지막 `hex:` 접두 fallback |
| `equals(other)` | 값 동등성 |

**Completion result.** 모든 factory·accessor는 동기다. 범위를 벗어난
길이는 `TypeError`/`RangeError`를 던진다. `fromHex`에 잘못된 hex
문자열을 주면 마찬가지다.

**선택 기준.** 사람이 부여한 identity엔 `from(string)`을, 숫자 identity엔
`from(number)`를, identity가 이미 binary일 때(16-byte UUID의 raw byte
포함)는 `Buffer`/`Uint8Array` overload를 쓴다. 내구성 있는 raw-byte round
trip 전용으로 `toHex()`/`fromHex()`를 쓴다 — `toString()`은 표시 전용이며
가역성이 보장되지 않는다.

---

## `version()` / `strerror(code)` / `has(capability)`

native library의 빌드 버전을 읽거나, native error code를 메시지로
변환하거나, 선택적 빌드 역할을 확인한다.

```ts
const [major, minor, patch] = version();
const message = strerror(errnum);
const hasTls = has('tls');
```

**Options.**

| Member | 의미 |
| --- | --- |
| `version()` | `{major, minor, patch}`의 `[number, number, number]` tuple |
| `strerror(code: number)` | 해당 native error code의 메시지 텍스트 |
| `has(capability: string)` | 지정한 선택적 역할이 이 빌드에 컴파일돼 있는지 — 인식하는 이름은 `'tcp'`, `'ipc'`, `'tls'`, `'ws'`, `'wss'`; 다른 문자열은 `false`를 반환한다 |

**Completion result.** 모두 동기다. `version()`은 `[number, number,
number]` tuple(major/minor/patch)을 반환한다. `strerror`는 `string`을
반환한다. `has`는 `boolean`을 반환한다.

**선택 기준.** 링크된 native library 버전이 application이 기대하는
버전과 일치하는지 확인하려면 `version()`을 쓴다. 기동 시점에
`has(...)`로 선택적 transport에 분기한다. `strerror`는 다른 곳(Errors
category)에서 드러난 native error code와 함께 진단용으로 쓴다.

---

## `createAtomicCounter(initialValue?)` / `createStopwatch()` / `createThread(handler)`

thread-safe 정수 counter, 고해상도 stopwatch, 실행 중인 background
thread를 생성한다 — 세 개의 독립된 utility resource. 이들의
interface(`AtomicCounter`, `Stopwatch`, `Thread`)는 소스에서
`contracts/core/`가 아니라 `contracts/eventing/timer.ts`에 선언돼 있다.

```ts
const counter = createAtomicCounter(); // 또는 createAtomicCounter(10)
const newValue = counter.inc();

const watch = createStopwatch();
const partialUs = watch.intermediate();
const totalUs = watch.stop();

const thread = createThread(() => doWork());
thread.join();
```

**Options.**

| Member | 의미 |
| --- | --- |
| `createAtomicCounter(initialValue = 0)` | dotnet/java/cpp와 달리, 항상 0에서 시작하는 게 아니라 생성 시점에 직접 선택적 시작값을 받는다 |
| `AtomicCounter.set(value)` | counter 값을 지정 |
| `AtomicCounter.inc()`/`dec()` | counter를 1 조정, *새* 값을 반환 |
| `AtomicCounter.value()` | 현재 값을 읽음 |
| `AtomicCounter.close()` | counter를 해제 |
| `createStopwatch()` | 인자 없음 |
| `Stopwatch.intermediate()` | 생성 이후 경과 마이크로초(`number`), 몇 번이든 호출 가능 |
| `Stopwatch.stop()` | 생성 이후 경과 마이크로초(`number`), 정확히 한 번 호출해 종료 |
| `Stopwatch.close()` | stopwatch를 해제 |
| `createThread(handler: () => void)` | 새 스레드에서 `handler`를 즉시 실행 |
| `Thread.join()` | task가 끝날 때까지 block — **유일한 member**; 다른 모든 언어의 thread handle과 달리 이 interface는 `close()`/dispose 메서드를 선언하지 않는다 |

**Completion result.** 세 factory 모두 자신의 resource를 동기로
반환한다.

**선택 기준.** 별도 `set()` 호출 없이 공유 counter가 0이 아닌 시작값이
필요할 땐 `createAtomicCounter(initial)`을 쓴다. 벤치마킹엔
`createStopwatch()`를 쓴다 — `intermediate()`는 몇 번이든 호출하고,
`stop()`은 정확히 한 번 호출한다. zlink 런타임이 수명주기를 소유해야 할
땐 Node의 `worker_threads`를 직접 쓰는 대신 `createThread`를 쓴다.

---

## `proxy(...)` / `proxySteerable(...)` / `sleep(seconds)` / `multipartClose(parts)`

두 socket 사이의 양방향 message-forwarding loop을 실행하거나(선택적으로
control socket을 통해 조종 가능), 호출 스레드를 sleep하거나, multipart
배열의 모든 메시지를 닫는다.

```ts
proxy(frontend, backend, capture); // capture는 선택; context 종료까지 block
proxySteerable(frontend, backend, capture, control);
sleep(1); // 초 단위, 밀리초 아님
multipartClose(parts);
```

**Options.**

| Member | 의미 |
| --- | --- |
| `proxy(frontend, backend, capture?)` | `capture`는 선택 사항 |
| `proxySteerable(frontend, backend, capture, control)` | 필수 `control` socket을 더함; `capture`는 `null` 가능 |
| `sleep(seconds: number)` | 호출 스레드를 block; 초 단위를 직접 받는다 — dotnet(`Duration` overload만 public)과 달리 여기엔 별도의 subsecond 옵션이 없다 |
| `multipartClose(parts: Message[])` | 모든 part를 한 번에 닫음 |

**Completion result.** 모두 반환값 없이 동기다. `proxy`/`proxySteerable`은
context가 종료될 때까지(또는 `proxySteerable`의 경우 control 명령이나
에러가 loop을 끝낼 때까지) 호출 스레드를 block한다 — 둘 중 하나를 전용
worker 스레드에서 실행한다.

**선택 기준.** 단순한 fire-and-forget forwarding loop엔 `proxy`를,
application이 다른 스레드에서 control socket을 통해 loop을
일시정지·재개·종료해야 할 땐 `proxySteerable`을 쓴다. 수신되거나 구성된
multipart 배열의 모든 `Message`를 손으로 짠 loop 대신 한 호출로
해제하려면 `multipartClose`를 쓴다.

---

[`contracts/core/`](../../../../bindings/node/src/zlink/contracts/core/),
[`src/index.ts`](../../../../bindings/node/src/index.ts),
[Node 바인딩 스펙](../../spec/node/README.ko.md)에서 전체 근거를 확인한다.
