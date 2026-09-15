한국어 | [English](01-core.en.md)

[레퍼런스 목차](README.ko.md)

# 01. Core

이 category는 context 수명주기, context option, routing identity, 그리고 자유
utility/capability 함수를 다룬다. socket 생성은 여기 factory 메서드가 아니라 각 구체
socket type 자신의 생성자로 이뤄진다(Sockets category) — dotnet의
`IContext.CreateXxx()` 메서드와 다르다. 정확한 signature는
[`Contracts/Core/`](../../../../bindings/cpp/include/zlink/Contracts/Core/)가 소유한다.

---

## `context_t`

메시징 context — socket의 factory이자 소유자이며, 어떤 socket type을 생성하든
전제조건이다. 이동 생성·대입 가능; 복사는 delete다 — context는 한 시점에 정확히
하나의 소유자만 가진다.

```cpp
zlink::context_t ctx;
zlink::context_t ctx_with_threads (zlink::io_thread_count_t::value (4));
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `context_t()` | 기본 I/O thread count로 생성 |
| `explicit context_t(io_thread_count_t)` | 기본값 대신 명시적 I/O thread count로 생성 |
| `valid()` | 이 context가 아직 사용 가능한지 — `term()` 이후엔 `false` |
| `shutdown()` | 이 context 하위 socket의 blocking operation을 닫지 않고 인터럽트 |
| `term()` | context를 종료하고 native resource를 해제 |
| `options()` | `context_options_t`(아래), context 전역 option facade를 반환 |
| `recalculate_auto_hwm()` | 일반 debounce 간격을 기다리지 않고 automatic high-water mark를 즉시 재계산 |

**완료 결과.** `valid()`/`options()`를 제외한 모든 member는 반환값 없이 동기다.
소멸자는 아직 종료되지 않았으면 `term()`을 호출한다.

**선택 기준.** application이 필요로 하는 context마다 `context_t` 하나를 생성한다 —
대부분은 정확히 하나가 필요하다. 여러 스레드에서 socket을 쓰는 중에 소멸시키기
전엔 `shutdown()`을 호출한다.

---

## `context_options_t`

`ctx.options()`로 도달하는 typed option facade. getter는 접미사가 없고, setter는
새 값을 받는다.

```cpp
ctx.options ().io_threads (zlink::io_thread_count_t::value (8));
ctx.options ().auto_hwm_profile (zlink::auto_hwm_profile::low_latency);
ctx.options ().add_thread_affinity (zlink::cpu_index_t::value (2));
```

**옵션.**

| Member | 타입 | 의미 |
| --- | --- | --- |
| `io_threads()` | `io_thread_count_t` | I/O thread 개수 |
| `max_sockets()` | `socket_count_t` | context 전역 socket 상한 |
| `max_msg_size()` | `byte_size_t` | 메시지당 크기 상한 |
| `thread_priority()` | `std::optional<thread_priority_t>` | dispatch thread 우선순위 |
| `thread_scheduling_policy()` | `thread_scheduling_policy_t` | dispatch thread 스케줄링 정책 |
| `thread_name_prefix()` | `std::string` | OS에 보이는 dispatch thread 이름 접두 |
| `blocky()` | `bool` | blocking 호출이 실제로 block할지 즉시 실패할지 |
| `auto_hwm_enabled()` | `bool` | auto-HWM 크기 조정 활성 여부 |
| `auto_hwm_recalc_debounce()` | `std::chrono::milliseconds` | 자동 재계산 사이 최소 간격 |
| `auto_hwm_profile()` | `zlink::auto_hwm_profile` | automatic HWM 크기 profile — Sockets category 참고 |
| `auto_hwm_msg_unit_bytes()` | `byte_count_t` | auto-HWM 크기 조정의 회계 단위 바이트 |
| `socket_limit()` | `socket_count_t` | 빌드의 `max_sockets` 하드캡(읽기 전용) |
| `msg_t_size()` | `byte_size_t` | native message struct 크기, 진단 전용(읽기 전용) |
| `add_thread_affinity(cpu_index_t)` | — | I/O thread를 CPU에 고정(setter만) |
| `remove_thread_affinity(cpu_index_t)` | — | I/O thread를 CPU에서 해제(setter만) |

**완료 결과.** 모든 getter/setter는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket 생성 전에 조정한다.
`auto_hwm_profile`/`auto_hwm_enabled` 변경은 `context_t::recalculate_auto_hwm()`과
짝지어 즉시 적용한다.

---

## Strongly-typed option 값 wrapper

raw `int`/`uint32_t` 대신 `context_options_t`와 socket option 전반에서 쓰이는 작은
value-type wrapper들로, 각각 static `value(...)` factory로 생성한다 — wrapper는
단위가 어긋나면 컴파일이 안 되게 하려고 존재한다.

**옵션.**

| 타입 | 감싸는 것 | 의미 |
| --- | --- | --- |
| `io_thread_count_t` | `int`(`::value(int)`/`.value()`) | `context_t` 생성자와 `context_options_t::io_threads`의 인자 |
| `socket_count_t` | `int`(`::value(int)`/`.value()`) | `context_options_t::max_sockets`/`socket_limit` |
| `worker_count_t` | `int`(`::value(int)`/`.value()`) | 상위 socket-option facade가 쓰는 worker-thread 개수(Sockets category) |
| `thread_priority_t` | `int`(`::value(int)`/`.value()`) | `context_options_t::thread_priority` |
| `cpu_index_t` | `int`(`::value(int)`/`.value()`) | `context_options_t::add_thread_affinity`/`remove_thread_affinity` |
| `socket_backlog_t` | `int`(`::value(int)`/`.value()`) | `common_socket_options_t::backlog`(Sockets category) |
| `byte_size_t` | `int64_t`(`::bytes(int64_t)`/`.bytes()`) | `max_msg_size` 같은 평범한 byte-size option |
| `byte_count_t`(Core) | `uint64_t`(`::bytes(uint64_t)`/`.bytes()`) | HWM과 byte-budget option이 쓰는 무손실 byte count |
| `peer_weight_t` | `uint32_t`(`::value(uint32_t)`) | load-balancing 가중치(Sockets category); 0-100 범위 밖이면 `std::invalid_argument` |

**완료 결과.** `peer_weight_t::value`를 제외한 모든 factory·accessor는
`noexcept`다 — 이건 범위를 검증한다.

**선택 기준.** 맨 정수를 넘기는 대신 호출 지점에서 이런 wrapper를 생성한다
(`io_thread_count_t::value(4)`).

---

## `routing_id_t`

메시징 peer나 route를 식별하는 1~255바이트의 binary-safe value type.

```cpp
auto from_string = zlink::routing_id_t::from (std::string ("worker-3"));
auto from_bytes = zlink::routing_id_t::from (raw_bytes);
auto from_uint = zlink::routing_id_t::from (uint32_t{42});
auto restored = zlink::routing_id_t::from_hex (previously_printed.to_hex ());
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `routing_id_t(const uint8_t *bytes_, size_t size_)` | raw byte pointer와 길이로부터 생성하는 생성자 |
| `from(const uint8_t*, size_t)` / `from(const std::vector<uint8_t>&)` | raw byte를 그대로 복사 |
| `from(const std::string&)` | raw byte를 복사, UTF-8 검증 없음 |
| `from(uint32_t)` | 4-byte big-endian으로 인코딩 |
| `from(const std::array<uint8_t, 16>&)` | 16-byte 값(예: GUID의 raw byte)을 복사 |
| `from_hex(const std::string&)` | `to_hex()`가 이전에 출력한 바이트를 복원 |
| `data()` | 밑바탕 바이트에 대한 pointer |
| `size()` | 바이트 길이, 1-255 |
| `to_bytes()` | 바이트의 소유 복사본을 `std::vector<uint8_t>`로 |
| `to_string()` | 표시용 형태: printable UTF-8, 그다음 4-byte를 uint32로, 그다음 16-byte를 GUID로, 마지막 `hex:` 접두 fallback |
| `to_hex()` | `from_hex`와 round-trip 가능한 hex 인코딩 |
| `operator==`/`!=` | 값 동등성 |
| `std::hash<routing_id_t>` | unordered container의 key로 쓸 수 있게 하는 특수화 |

**완료 결과.** 모든 factory·accessor는 동기다. 빈 입력, 255바이트 초과, 크기는
0이 아닌데 null pointer면 `std::invalid_argument`를 던진다. `from_hex`에 잘못된
hex 문자열을 주면 마찬가지다.

**선택 기준.** 사람이 부여한 identity엔 `from(const std::string&)`를, 숫자·GUID
형태 identity엔 `from(uint32_t)`/16-byte 배열 overload를, 이미 binary인
identity엔 raw byte overload를 쓴다. 내구성 있는 round trip 전용으로
`to_hex()`/`from_hex()`를 쓴다 — `to_string()`은 표시 전용이다.

---

## `zlink::version` / `zlink::error_text` / `zlink::has`

native library의 빌드 버전을 읽거나, native error code를 메시지로 변환하거나,
선택적 빌드 역할을 확인한다.

```cpp
int major, minor, patch;
zlink::version (major, minor, patch);
const char *message = zlink::error_text (errnum);
bool has_tls = zlink::has ("tls");
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `version(int &major_, int &minor_, int &patch_)` | 링크된 native library의 major·minor·patch 버전 번호를 `major_`/`minor_`/`patch_`에 씀 |
| `error_text(int errnum_) noexcept` | native error code `errnum_`의 메시지 텍스트를 `const char*`로 반환; caller가 수정·해제하면 안 됨 |
| `has(const std::string &capability_)` | 이름 붙은 선택적 역할이 이 빌드에 컴파일됐는지 — 인식하는 이름은 `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; 그 외 문자열은 `false` |

**완료 결과.** 셋 다 동기이며 예외를 던지지 않는다.

**선택 기준.** 동적으로 로드된 native library가 기대와 일치하는지 확인하려면
`version()`을 쓴다. 기동 시점에 선택적 transport를 분기하려면 `has(...)`를 쓴다.

---

## `stopwatch_t` / `atomic_counter_t` / `thread_t`

고해상도 stopwatch, thread-safe 정수 counter, 실행 중인 background thread — 같은
RAII 형태를 가진 세 개의 독립된 utility resource: 기본 생성 가능, move-only,
`valid() const noexcept`, `close()`(소멸자는 아직 닫히지 않았으면 `close()`를
호출).

```cpp
zlink::stopwatch_t watch;
uint64_t partial_us = watch.intermediate ();
uint64_t total_us = watch.stop ();

zlink::atomic_counter_t counter;
int new_value = counter.increment ();

zlink::thread_t worker ([] { do_work (); });
worker.join ();
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `stopwatch_t::intermediate()` | 생성 이후 경과 마이크로초, 몇 번이든 호출 가능 |
| `stopwatch_t::stop()` | 생성 이후 경과 마이크로초, 마치려고 정확히 한 번 호출 |
| `atomic_counter_t::set(int)` | counter 값을 대입 |
| `atomic_counter_t::increment()` / `decrement()` | counter를 1만큼 조정, *새* 값을 반환 |
| `atomic_counter_t::value() const` | 현재 값을 읽음 |
| `thread_t(std::function<void()> task_)` | 생성과 동시에 `task_`를 새 thread에서 즉시 실행 |
| `thread_t::join()` | task가 끝날 때까지 block |

**완료 결과.** 모두 동기다.

**선택 기준.** 스레드 전체에서 안전한 공유 count엔 `atomic_counter_t`를 쓴다.
벤치마킹엔 `stopwatch_t`를 쓴다. 플랫폼 특정 API 대신 이식 가능한 background
thread엔 `thread_t`를 쓴다.

---

[`Contracts/Core/`](../../../../bindings/cpp/include/zlink/Contracts/Core/)와
[C++ 바인딩 스펙](../../spec/cpp/README.ko.md)에서 전체 근거를 확인한다.
