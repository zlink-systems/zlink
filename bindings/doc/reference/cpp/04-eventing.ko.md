한국어 | [English](04-eventing.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone timer를 다룬다 —
각각 `socket_t::monitor_open(...)`(Sockets category)와 직접 생성(`poller_t`,
`timer_t`)으로 만들어진다. 이 투영엔
`Zlink.CreatePoller()`/`CreateTimer()` 스타일의 factory가 없다. 정확한
signature는
[`Contracts/Eventing/`](../../../../bindings/cpp/include/zlink/Contracts/Eventing/)가
소유한다.

---

## `socket_monitor_t`

socket의 connection lifecycle event를 관찰하고 현재 상태를 읽는다.

```cpp
zlink::socket_monitor_t monitor =
    zlink::socket_monitor_t::open (socket, zlink::monitor_event::connected | zlink::monitor_event::disconnected);
monitor.on_event ([] (const zlink::monitor_event_t &e) { /* ... */ });
zlink::monitor_status_t status = monitor.status ();
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `socket_monitor_t()` | — | 기본, 대입 전까지 invalid |
| `open(const socket_t&, monitor_event)` | `monitor_event::all` | static — 실제 생성 경로, 내부적으로 `socket_t::monitor_open(...)`이 호출 |
| `valid()` | — | 이 monitor가 아직 사용 가능한지 |
| `on_event(std::function<void(const monitor_event_t&)>)` | — | 모든 lifecycle event에 대한 수동적 observer를 등록 |
| `recv(recv_flags_t)` | `recv_flags_t::none` | 다음 event를 가져옴; `std::optional<monitor_event_t>` 반환 |
| `status() const` | — | monitor 대상 socket 상태의 시점 스냅샷, `monitor_status_t` 반환 |
| `close()` | — | monitor의 native resource를 해제 |
| `ignore_event(const monitor_event_t&) noexcept` | — | static no-op, 의도적으로 아무것도 안 하는 handler가 필요한 caller용 |

**완료 결과.** 모든 member는 동기다. move-only다 — 소멸자는 암묵적으로
close하지 않는다.

**선택 기준.** 한 번 등록하는 수동적 lifecycle observer엔 `on_event`를, pull
기반 drain loop엔 `recv`를 쓴다. 시점 스냅샷엔 `status()`를 쓴다.

---

## `monitor_status_t`

`socket_monitor_t::status()`가 반환하는, socket의 monitored 상태와
auto-high-water-mark telemetry 스냅샷. accessor method가 있는 class가 아니라
순수 struct다(dotnet의 `MonitorStatus`와 다름) — 모든 필드가 public 데이터다.

**옵션.** 인자 없음 — `status()`로 생성하지 직접 생성하지 않는다.

| 그룹 | 필드 |
|---|---|
| ABI identity | `abi_version`, `struct_size`(`uint32_t`) |
| Source/state | `source_kind`(`monitor_source_kind`), `state_flags`/`detail_flags`(`uint32_t` 비트마스크 — 아래 enum 참고), `is_ready()`(계산값: `(state_flags & 1) != 0`) |
| Pending count | `snd_pending_msgs`, `rcv_pending_msgs`(`uint64_t`) |
| Auto-HWM 설정 | `auto_hwm_enabled`(`bool`), `auto_hwm_profile`, `auto_hwm_role`, `auto_hwm_policy_class`(`uint32_t` — `zlink::auto_hwm_profile` enum 타입이 아니라 raw 정수 필드), `auto_hwm_unit_budget_bytes`, `auto_hwm_socket_message_slots`(`uint64_t`), `auto_hwm_size_cap`(`uint32_t`) |
| Connection bucket | `auto_hwm_connection_bucket_enabled`(`bool`), `auto_hwm_connection_bucket_count`/`_index`/`_hwm_4k`(`uint32_t`), `auto_hwm_connection_bucket_hysteresis_retained`(`bool`) |
| Auto-HWM plan(byte) | `auto_hwm_effective_message_bytes`, `auto_hwm_planned_sndhwm_bytes`/`_rcvhwm_bytes`, `auto_hwm_applied_sndhwm_bytes`/`_rcvhwm_bytes`(`uint64_t`), `auto_hwm_effective_sndbuf`/`_rcvbuf`(`int32_t`) |
| Auto-HWM recalc | `auto_hwm_last_recalc_ms`(`uint64_t`), `auto_hwm_last_recalc_reason`(`uint32_t`), `auto_hwm_send_blocked_ratio_ppm`(`uint32_t`) |
| Auto-HWM deferred shrink | `auto_hwm_deferred_sndhwm_bytes`/`_rcvhwm_bytes`(`uint64_t`, 대응하는 `..._valid` `bool`이 true일 때만 유효) |
| In-flight/과금 | `snd_bytes_in_flight`, `rcv_bytes_in_flight`, `minimum_core_message_charge_bytes`, `oversize_message_admission_count`, `oversize_message_admission_max_bytes`(`uint64_t`) |

**완료 결과.** 해당 없음 — 순수 데이터, dispose 없음.

**선택 기준.** `state_flags`를 직접 디코딩하는 대신 `is_ready()`를 읽는다.
socket의 실제 send/receive HWM이 설정한 `common_socket_options_t` 값(Sockets
category)과 다른 이유를 진단할 땐 connection-bucket과 auto-HWM-plan 필드를
쓴다.

---

## `poller_t`

socket, file descriptor, monitor, timer를 하나의 재사용 가능한 wait로
multiplex한다.

```cpp
zlink::poller_t poller;
poller.add (dealer, zlink::poll_event_flag_t::pollin, /*slot=*/1);
poller.add (timer, /*slot=*/2);
std::vector<zlink::poll_event_t> ready (8);
size_t count = poller.wait (ready.data (), ready.size (), std::chrono::seconds (1));
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `add(socket_monitor_t&, poll_event_flag_t, std::uintptr_t slot_)` | — | monitor를 주어진 event로 등록; `slot_`은 대응하는 `poll_event_t`로 되돌아옴 |
| `add(socket_t&, poll_event_flag_t, std::uintptr_t slot_)` | — | 같은 방식으로 socket을 등록 |
| `add_fd(int fd_, poll_event_flag_t, std::uintptr_t slot_)` | — | 같은 방식으로 raw file descriptor를 등록 |
| `add(timer_t&, std::uintptr_t slot_)` | — | timer를 등록, fire하면 ready로 표시 |
| `modify_fd(int, poll_event_flag_t)` / `modify(socket_monitor_t&, poll_event_flag_t)` / `modify(socket_t&, poll_event_flag_t)` | — | 이미 등록된 source의 감시 event를 변경 |
| `remove(socket_monitor_t&)` / `remove(socket_t&)` / `remove(timer_t&)` / `remove_fd(int)` | — | 등록 해제; 각각 `bool` 반환, 등록돼 있었으면 true |
| `size() const` | — | 현재 등록된 source 개수(`int`) |
| `close()` | — | poller의 native resource를 해제 |
| `wait(poll_event_t* events_, size_t capacity_, std::chrono::milliseconds timeout_)` | — | source 하나 이상이 ready 상태이거나 `timeout_`이 지날 때까지 block |

**완료 결과.** 등록/제거 member는 동기다. `wait`는 `timeout_`까지 block하며,
`capacity_`까지 결과를 쓰고 쓴 개수를 반환한다(timeout이면 `0`). `poller_t`는
`socket_monitor_t&`도 직접 등록할 수 있다(dotnet의 `IPoller`의 `Add`
overload는 `IZlinkSocket`/`IZlinkTimer`만 받고, monitor는 대신
`ZlinkPoll.Poll(IReadOnlyList<ISocketMonitor>, ...)`을 통해 간접적으로
poll되는 것과 다르다).

**선택 기준.** 서비스 수명 전체에서 poller 하나를 쓴다. 감시하는 event만 바뀔
땐 `remove` + `add` 대신 `modify`를 선호한다.

---

## `poll_event_t`

`poller_t::wait` 호출이 보고하는 준비된 source 하나. 순수 struct이며 기본
생성 시 `source_kind = poll_source_kind_t::socket`이다.

**옵션.**

| Member | 타입 | 의미 |
| --- | --- | --- |
| `source_kind` | `poll_source_kind_t` | `socket`/`fd`/`timer` |
| `slot` | `std::uintptr_t` | 등록 시 제공한 caller token |
| `revents` | `poll_event_flag_t` | 실제로 발생한 event |
| `fd` | `int` | `fd` kind source에서만 채워짐 |

**완료 결과.** 해당 없음 — 순수 데이터.

**선택 기준.** `source_kind`/`slot`으로 분기해 각 `wait` 결과를 대응하는
socket·descriptor·timer로 연결한다.

---

## `poll_item_t` / `zlink::poll(...)`

`poller_t`와 구별되는 standalone one-shot poll helper — 아무것도 영구적으로
등록하지 않고 watch item의 순수 배열을 만들어 한 번 기다린다.

```cpp
std::vector<zlink::poll_item_t> items {
    zlink::poll_item_t::from_socket (dealer, zlink::poll_event_flag_t::pollin),
    zlink::poll_item_t::from_fd (raw_fd, zlink::poll_event_flag_t::pollin),
};
int ready = zlink::poll (items, std::chrono::milliseconds (1000));
```

**옵션.**

| Member | 의미 |
| --- | --- |
| `socket`(`socket_t*`) | poll할 socket, fd 기반 item이면 null |
| `fd`(`int`) | `socket`이 null일 때 poll할 file descriptor |
| `events`/`revents`(`poll_event_flag_t`) | 요청한/실제로 발생한 event |
| `from_socket(socket_t&, poll_event_flag_t)` / `from_fd(int, poll_event_flag_t)` | socket이나 raw fd용 `poll_item_t`를 만드는 static factory |
| `zlink::poll(poll_item_t* items_, size_t count_, std::chrono::milliseconds timeout_)` | `items_` 전체를 한 번 기다리며 `revents`를 그 자리에 씀; `std::vector<poll_item_t>&` 편의 overload도 있음 |

**완료 결과.** 동기다. 준비된 item 개수를 반환한다(timeout이면 `0`). 각
`poll_item_t`의 `revents`는 호출로 그 자리에서 쓰인다.

**선택 기준.** 작고 고정된 집합에 대한 임시 one-off wait엔 이 자유 함수 형태를
쓴다. 감시 대상 집합이 시간에 따라 바뀌거나 monitor/timer를 socket과 함께
multiplex해야 할 땐 `poller_t`를 쓴다.

---

## `timer_t`

interval마다 fire하며 (`recv`로) poll하거나 poller를 통해 구동할 수 있는
standalone timer.

```cpp
zlink::timer_t timer;
timer.on_fire ([] (uint64_t count) { /* ... */ });
timer.start (std::chrono::seconds (1), /*repeat_count=*/0);
```

**옵션.**

| Member | 기본값 | 의미 |
| --- | --- | --- |
| `start(duration, uint64_t repeat_count_)` | `repeat_count_ = 0` | `duration`마다 fire를 시작(임의 `std::chrono::duration`을 받는 template, 내부적으로 나노초로 변환; 음수는 `config_error_t{invalid_argument}`); `repeat_count_`가 횟수 상한 |
| `stop()` | — | fire를 멈춤; `start`로 재시작 가능 |
| `recv()` | — | 누적 fire count를 가져옴; `std::optional<uint64_t>` 반환, 대기 중인 게 없으면 `std::nullopt` |
| `on_fire(std::function<void(uint64_t)>)` | — | fire할 때마다 호출되는 수동적 observer를 등록; dotnet의 `Action<IZlinkTimer, ulong>`과 달리, 여기 콜백은 timer 자신이 아니라 fire count만 받음 |
| `close()` | — | timer의 native resource를 해제 |

**완료 결과.** 모든 member는 동기다. move-only다 — 소멸자는 암묵적으로
close하지 않는다.

**선택 기준.** 수동적 interval 콜백엔 `on_fire`를, 만료를 poll하려면
`recv`를, socket과 함께 multiplex하려면 `poller_t::add(timer_t&,
std::uintptr_t)`로 등록한다.

---

## Eventing enum

위 모든 항목에서 참조하는 공유 enum.

| Enum | 사용처 | 값 |
|---|---|---|
| `monitor_event` | `socket_monitor_t::open`/`monitor_event_t::event` | `connected`, `connect_delayed`, `connect_retried`, `listening`, `bind_failed`, `accepted`, `accept_failed`, `closed`, `close_failed`, `disconnected`, `monitor_stopped`, `handshake_failed_no_detail`, `connection_ready`, `handshake_failed_protocol`, `handshake_failed_auth`, `peer_weight_changed`, `all` |
| `monitor_target_kind_t` | 이 category에 문서화된 어떤 항목으로도 도달하지 않음 | `socket`, `discovery`, `spot` — 뒤 둘은 이 레퍼런스 tier에서 대응하는 public monitor 생성 진입점이 없다 |
| `monitor_source_kind` | `monitor_status_t::source_kind` | `socket`(1), `spot_pub`(3), `spot_sub`(4) — 뒤 둘은 선언돼 있지만 `monitor_target_kind_t::spot`처럼 bindings 계층에서 도달 가능한 public source가 없다(SPOT/Actor는 framework 계층에만 존재) |
| `monitor_state` | `monitor_status_t::state_flags`(비트마스크) | `ready`(1), `bound_ready`(2), `closed`(8) |
| `monitor_status_detail` | `monitor_status_t::detail_flags`(비트마스크) | `snd_pending_msgs`(2), `rcv_pending_msgs`(4) |
| `disconnect_reason` | 이 category에 문서화된 어떤 항목으로도 도달하지 않음 | `unknown`, `handshake_failed`, `transport_error`, `ctx_term` |
| `poll_source_kind_t` | `poll_event_t::source_kind` | `socket`, `fd`, `timer` |
| `poll_event_flag_t` | `poller_t::add`/`modify`/`wait`, `poll_item_t`, `zlink::poll` | `none`, `pollin`, `pollout`, `pollerr`, `pollcompletion`(이 투영엔 dotnet의 `PollEventFlags.PollPri`에 대응하는 `pollpri`가 없다) |

**선택 기준.** `monitor_source_kind::spot_pub`/`spot_sub`,
`monitor_target_kind_t::spot`/`discovery`, `disconnect_reason`은 이 bindings
계층의 public 진입점에서 선언은 됐지만 현재 도달 불가능한 것으로 취급한다 —
오늘 application 코드에서 우회할 대상이 아니라 스펙 차원의 질문이다.

---

[`Contracts/Eventing/`](../../../../bindings/cpp/include/zlink/Contracts/Eventing/)와
[C++ 바인딩 스펙](../../spec/cpp/README.ko.md)에서 전체 근거를 확인한다.
