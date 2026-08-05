한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity, crate 최상위
자유 함수를 다룬다. **지금까지 다룬 다른 모든 wrapper binding과 달리, 이 자유
함수는 `contracts/core/` 아래 선언돼 있지 않다** — crate 최상위
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs)에 있다.
`Stopwatch`/`AtomicCounter`/`Thread`는 `contracts/core/utilities.rs`에 선언돼
있다. 정확한 signature는
[`contracts/core/`](../../../../bindings/rust/src/contracts/core/)와
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs)가 소유한다.

---

## `Context::new()`

메시징 context를 생성한다 — socket의 factory이자 소유자.

```rust
let ctx = Context::new()?;
```

**Options.** 인자 없음.

**Completion result.** `Result<Context, ConfigError>`를 반환한다 — **여기선
context 생성 자체가 실패할 수 있다**, 지금까지 다룬 다른 모든 언어에서
대응하는 factory의 public signature엔 error 경로가 없는 것과 다르다.
`Context`는 `Send`/`Sync`이며 스레드 전체에서 공유될 수 있다(예:
`std::sync::Arc`를 통해) — 마지막으로 소유하고 있던 `Context` 값이 drop되면
context가 종료되므로, 다른 스레드가 socket을 생성·사용하는 동안엔 소유자가
살아있어야 한다.

**선택 기준.** application이 필요로 하는 context마다 한 번 호출한다 —
대부분의 application은 정확히 하나가 필요하다. 어떤 스레드든 여기서 socket을
생성·사용하는 동안엔 소유하는 `Context`(또는 `Arc<Context>`)를 계속 살려둔다.

---

## `Context::shutdown()` / `Context::recalculate_auto_hwm()`

context의 socket이 닫히지 않은 채 blocking operation을 인터럽트하거나,
automatic high-water mark의 즉시 재계산을 강제한다.

```rust
ctx.shutdown()?;
ctx.recalculate_auto_hwm()?;
```

**Options.** 둘 다 인자 없음.

**Completion result.** `shutdown()`은 `Result<(), CloseError>`를 반환한다.
`recalculate_auto_hwm()`은 `Result<(), ConfigError>`를 반환한다. `shutdown`은
이 context 하위 socket의 blocking 호출을 인터럽트하지만 context나 그 socket을
drop하지 않는다. `recalculate_auto_hwm`은 아직 `AutoHwmProfile`이 설정된
socket에 대해서만 automatic HWM을 재계산한다.

**선택 기준.** 여러 스레드에서 socket을 쓰는 중인 context를 drop하기 전엔
`shutdown()`을 호출해 socket 호출을 기다리는 스레드가 무기한 block되는 걸
막는다. auto-HWM profile이나 message-unit option을 바꾼 후엔 새 sizing을
즉시 적용하려고 `recalculate_auto_hwm()`을 호출한다.

---

## `Context::options()`

I/O thread와, context에서 생성되는 모든 socket이 상속하는 기본값을 관장하는
context-wide option facade를 읽는다.

```rust
let options = ctx.options();
options.set_io_threads(8)?;
options.set_auto_hwm_profile(AutoHwmProfile::LowLatency)?;
options.add_thread_affinity(2)?;
```

**Options.** 아래 모든 getter/setter는 `Result<T, ConfigError>`를 반환한다.

| Member | 의미 |
| --- | --- |
| `io_threads()` / `set_io_threads(i32)` | I/O thread 개수 |
| `max_sockets()` / `set_max_sockets(i32)` | context 전체 socket 상한 |
| `socket_limit()` | 읽기 전용, `max_sockets`의 빌드상 하드 상한 |
| `thread_priority()` / `set_thread_priority(i32)` | dispatch thread 우선순위 |
| `thread_scheduling_policy()` / `set_thread_scheduling_policy(i32)` | dispatch thread scheduling policy |
| `max_message_size()` / `set_max_message_size(i32)` | 메시지 하나의 크기 상한 |
| `msg_t_size()` | 읽기 전용, native message struct 크기, 진단 전용 |
| `blocky()` / `set_blocky(bool)` | blocking 호출이 실제로 block하는지 fail fast하는지 |
| `thread_name_prefix()` / `set_thread_name_prefix(&str)` | OS에 보이는 dispatch thread 이름 접두사 |
| `auto_hwm_enabled()` / `set_auto_hwm_enabled(bool)` | auto-HWM sizing 활성 여부 |
| `auto_hwm_recalc_debounce()` / `set_auto_hwm_recalc_debounce(Duration)` | 자동 재계산 사이 최소 간격 |
| `auto_hwm_profile()` / `set_auto_hwm_profile(AutoHwmProfile)` | automatic HWM sizing profile — Sockets category 참고 |
| `auto_hwm_msg_unit_bytes()` / `set_auto_hwm_msg_unit_bytes(u64)` | auto-HWM용 accounted-byte 단위; `0`은 socket-type 기본값 선택 |
| `add_thread_affinity(i32)` | setter만, I/O thread를 CPU에 고정 |
| `remove_thread_affinity(i32)` | setter만, I/O thread의 CPU 고정을 해제 |

**Completion result.** 모든 getter/setter는 동기이며 `Result<_,
ConfigError>`를 반환한다(진짜 예외적인 에러에서만 throw하는 언어와 달리,
모든 option 접근이 실패할 수 있다).

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에
조정한다. `auto_hwm_profile`/`auto_hwm_enabled` 변경은
`Context::recalculate_auto_hwm()`과 짝지어 즉시 적용한다.

---

## `Context::pair_socket()` / `dealer_socket()` / `router_socket()` / `pub_socket()` / `sub_socket()` / `xpub_socket()` / `xsub_socket()` / `stream_socket()`

context로부터 주어진 타입의 socket을 생성한다, caller가 소유.

```rust
let dealer = ctx.dealer_socket()?;
```

**Options.** 8개 factory 메서드 모두 인자 없음.

**Completion result.** 각각 `Result<SocketType, ConfigError>`를 반환한다 —
**socket 생성 자체가 실패할 수 있다**, dotnet/java/node/cpp에서 대응하는
factory의 public signature엔 error 경로가 없는 것과 다르다.

**선택 기준.** 각 socket type의 operation·option·역할은 Sockets category를
참고한다 — 이 항목은 각각이 어떻게 생성되는지만 다룬다.

---

## `RoutingId`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value type.

```rust
let from_string: RoutingId = "worker-3".into();
let from_bytes: RoutingId = raw_bytes.as_slice().into();
let from_uint32: RoutingId = 42u32.into();
let restored = RoutingId::from_hex(&previously_printed.to_hex())?;
```

**Options.** static factory 메서드가 아니라 Rust `From<T>` trait
구현이다 — `.into()`나 `RoutingId::from(...)`로 도달하는 관용적 변환
메커니즘이다. **모든 `From` 변환과 `from_hex`는 빈 입력이나 길이 초과
입력에서 `Result`를 반환하는 게 아니라 panic한다.**

| Member | 의미 |
| --- | --- |
| `From<&[u8]>` / `From<&[u8; N]>` | slice/고정 크기 배열 전체를 그대로 복사 |
| `From<&str>` | UTF-8 인코딩 |
| `From<u32>` | 4-byte big-endian |
| `From<[u8; 16]>` | 16-byte, 예: UUID byte |
| `from_hex(&str)` | `to_hex()`가 출력한 byte를 복원; 잘못된 입력에 panic |
| `try_from_hex(&str) -> Result<Self, ConfigError>` | hex 디코딩 전용으로 panic하지 않는 대안 |
| `MAX_LEN` | `usize` 상수, `255` |
| `as_bytes()` | bytes의 방어적 복사 |
| `size()` | byte 길이, 1-255 |
| `is_empty()` | `size()`가 0인지 |
| `to_hex()` | hex 인코딩, `from_hex`/`try_from_hex`와 왕복 가능 |
| `Display` | printable UTF-8, 그 다음 4-byte를 `u32`로, 그 다음 16-byte를 UUID 포맷으로, 마지막 `hex:` 접두 fallback로 포맷 |
| `PartialEq`/`Eq`/`Hash`/`Copy`/`Clone` | derive된 값 동등성 |

**Completion result.** 모든 `From` 변환과 `from_hex`는 동기이며 잘못된
입력에 panic한다. `try_from_hex`만 panic 대신 `Result<Self,
ConfigError>`를 반환한다.

**선택 기준.** 입력이 이미 유효하다고 알려진 경우(예: 컴파일 타임 문자열
리터럴)엔 `From`/`.into()` 변환을 쓴다. hex 문자열이 프로그램 밖에서 오고
잘못됐을 수 있을 땐 `from_hex` 대신 `try_from_hex`를 쓴다 —
`from_hex`/모든 `From` 변환은 error를 반환하는 대신 panic하기 때문이다.

---

## `version()` / `has(capability)` / `strerror(errnum)`

native library의 빌드 버전을 읽거나, 선택적 빌드 역할을 확인하거나, native
error code를 메시지로 변환한다.

```rust
let (major, minor, patch) = version();
let has_tls = has("tls");
let message = strerror(errnum);
```

**Options.**

| Member | 의미 |
| --- | --- |
| `version()` | `{major, minor, patch}`의 `(i32, i32, i32)` tuple |
| `has(capability: &str)` | 지정한 선택적 역할이 이 빌드에 컴파일돼 있는지 — 인식하는 이름은 `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; 다른 문자열은 `false`를 반환한다 |
| `strerror(errnum: i32)` | 해당 native error code의 메시지 텍스트 |

**Completion result.** 모두 동기이며 error 경로가 없다. `version()`은
`(i32, i32, i32)`를 반환한다. `has`는 `bool`을 반환한다. `strerror`는
`&'static str`을 반환한다.

**선택 기준.** 링크된 native library 버전이 application이 기대하는
버전과 일치하는지 확인하려면 `version()`을 쓴다. 기동 시점에
`has(...)`로 선택적 transport에 분기한다. `strerror`는 다른 곳(Errors
category)에서 드러난 native error code와 함께 진단용으로 쓴다.

---

## `Stopwatch::start()` / `AtomicCounter::new()` / `Thread::start(task)`

고해상도 stopwatch, thread-safe 정수 counter, 실행 중인 background
thread를 생성한다 — 세 개의 독립된 utility resource, 모두
`contracts/core/utilities.rs`에 선언됨.

```rust
let mut watch = Stopwatch::start()?;
let partial_us = watch.intermediate();
let total_us = watch.stop();

let counter = AtomicCounter::new()?;
let new_value = counter.increment();

let mut thread = Thread::start(|| do_work())?;
thread.join();
```

**Options.**

| Member | 의미 |
| --- | --- |
| `Stopwatch::start()` | 인자 없음 |
| `Stopwatch.intermediate()` / `stop()` | 생성 이후 경과 마이크로초(`u64`), `&mut self` — `stop`은 handle을 무효화함 |
| `Stopwatch.close()` | stopwatch를 해제 |
| `AtomicCounter::new()` | 인자 없음 |
| `AtomicCounter.set(i32)` | counter 값을 지정 |
| `AtomicCounter.increment()` / `decrement()` | counter를 1 조정, *새* 값을 반환 |
| `AtomicCounter.value()` | 현재 값을 읽음 |
| `AtomicCounter.close()` | counter를 해제 |
| `Thread::start<F>(task: F)`(`F: FnOnce() + Send + 'static`) | 새 스레드에서 `task`를 즉시 실행 |
| `Thread.join(&mut self)` | task가 끝날 때까지 block; task가 panic했으면 `resume_unwind`로 그 panic을 다시 일으킴 |
| `Thread.close()` | thread handle을 해제 |

**Completion result.** 세 생성자 모두 `Result<Self, ConfigError>`를
반환한다. 각 타입은 `Drop`도 구현해서 아직 닫히지 않았으면 자동으로
`close()`를 호출한다.

**선택 기준.** 스레드 전체에서 안전한 공유 count엔 `AtomicCounter`를
쓴다. 벤치마킹엔 `Stopwatch`를 쓴다 — `intermediate()`는 몇 번이든
호출하고, `stop()`은 정확히 한 번 호출한다. zlink 런타임이 수명주기를
소유하고 `join()`을 통해 task panic을 다시 전파해야 할 땐
`std::thread`를 직접 쓰는 대신 `Thread`를 쓴다.

---

## `proxy(...)` / `proxy_steerable(...)` / `sleep(seconds)` / `multipart_close(parts)` / `poll(items, timeout_ms)`

두 pollable source 사이의 양방향 message-forwarding loop을
실행하거나(선택적으로 control source를 통해 조종 가능), 호출 스레드를
sleep하거나, multipart slice의 모든 메시지를 닫거나, raw poll item의
고정 배열을 wait한다.

```rust
proxy(&frontend, &backend, Some(&capture))?;
proxy_steerable(&frontend, &backend, Some(&capture), &control)?;
sleep(1); // 초 단위, 밀리초 아님
multipart_close(&mut parts);
let ready = poll(&mut items, 1000)?;
```

**Options.**

| Member | 의미 |
| --- | --- |
| `proxy(frontend: &dyn Pollable, backend: &dyn Pollable, capture: Option<&dyn Pollable>)` | `capture`는 선택 사항; 셋 다 구체 socket 타입이 아니라 `&dyn Pollable` trait object를 받는다 — 모든 내장 socket type이 sealed `Pollable` trait(Eventing category)을 구현한다 |
| `proxy_steerable(..., control: &dyn Pollable)` | 필수 `control` source를 더함, 같은 `Pollable` 형태 |
| `sleep(seconds: i32)` | 호출 스레드를 block; 초 단위를 직접 받는다 |
| `multipart_close(parts: &mut [Message])` | 모든 part를 한 번에 닫음 |
| `poll(items: &mut [PollItem], timeout_ms: i64)` | `Poller`(Eventing category)와 구별되는 standalone one-shot poll helper; 각 `PollItem`의 `revents`를 그 자리에서 채운다 |

**Completion result.** `proxy`/`proxy_steerable`은 `Result<(),
ConfigError>`를 반환한다 — **여기선 실패할 수 있다**, 다른 언어에서
대응하는 호출엔 error 경로가 없고 그냥 종료까지 block하는 것과 다르다.
`sleep`/`multipart_close`는 반환값이 없다. `poll`은 `Result<i32,
RecvError>`(준비된 개수)를 반환한다.

**선택 기준.** 단순한 fire-and-forget forwarding loop엔 자신의 스레드에서
`proxy`를 쓴다. application이 다른 스레드에서 control source를 통해
loop을 일시정지·재개·종료해야 할 땐 `proxy_steerable`을 쓴다. 수신되거나
구성된 multipart slice의 모든 `Message`를 한 호출로 해제하려면
`multipart_close`를 쓴다. 작고 고정된 raw file descriptor 집합에 대한
임시 wait엔 standalone `poll(...)`을, 감시 대상 집합이 시간에 따라
바뀌거나 socket/timer를 multiplex해야 할 땐 대신 `Poller`를
쓴다(Eventing category).

---

[`contracts/core/`](../../../../bindings/rust/src/contracts/core/),
[`src/lib.rs`](../../../../bindings/rust/src/lib.rs),
[Rust 바인딩 스펙](../../spec/rust/README.ko.md)에서 전체 근거를 확인한다.
