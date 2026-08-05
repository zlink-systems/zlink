한국어 | [English](04-eventing.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Eventing

이 category는 socket monitoring, 재사용 가능한 poller, standalone timer를
다룬다 — 각각 `Socket.monitorOpen(...)`(Sockets category)와
`Zlink.createPoller()`/`Zlink.createTimer()`(Core category)로 생성된다. 정확한
signature는
[`contracts/eventing/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/eventing/)가
소유한다.

---

## `SocketMonitor`

socket의 connection lifecycle event를 관찰하고 현재 상태를 읽는다.

```java
try (SocketMonitor monitor = socket.monitorOpen(MonitorEventType.CONNECTED, MonitorEventType.DISCONNECTED)) {
    monitor.onEvent(event -> logger.info("{} {}", event.event(), event.remoteAddr()));
    MonitorStatus status = monitor.status();
}
```

**Options.**

| Member | 의미 |
| --- | --- |
| `onEvent(SocketMonitorHandler handler)` | 수동적 lifecycle-event 콜백을 등록 |
| `recv()` / `recv(RecvFlags flags)` | 다음 event를 가져옴; 둘 다 `MonitorEvent`를 직접 반환한다 — dotnet의 `MonitorEvent?`와 달리 `Optional`/nullable이 아니다 |
| `status()` | 시점 스냅샷 `MonitorStatus`를 반환 |
| `IGNORE_HANDLER` | caller가 의도적으로 event를 버리고 싶을 때 등록할 수 있는 public static 상수 no-op `SocketMonitorHandler` |

**Completion result.** 모든 member는 동기다. `SocketMonitor extends
AutoCloseable`이다.

**선택 기준.** 한 번 등록하는 수동적 lifecycle observer엔 `onEvent`를,
pull 기반 drain loop엔 대신 `recv`를 쓴다. 시점 스냅샷엔 `status()`를 쓴다.

---

## `MonitorStatus`

`SocketMonitor.status()`가 반환하는, socket의 monitored 상태와
auto-high-water-mark telemetry 스냅샷. Java `record`다 — 모든 component가
불변이며 record accessor로 접근한다(`status.sndPendingMsgs()`, `get` 접두
메서드가 아님).

**Options.** 인자 없음 — 직접 생성하지 않고 `status()`로 얻는다.

| 그룹 | Component |
|---|---|
| ABI identity | `abiVersion`, `structSize`(`int`) |
| Source/state | `sourceKind`(`MonitorSourceKind`), `stateFlags`(`EnumSet<MonitorStateFlags>`), `detailFlags`(`EnumSet<MonitorStatusDetailFlags>`), `isReady()`(계산 method: `stateFlags.contains(MonitorStateFlags.READY)`) |
| Pending count | `sndPendingMsgs`, `rcvPendingMsgs`(`long`) |
| Auto-HWM 설정 | `autoHwmEnabled`(`boolean`), `autoHwmProfile`(`AutoHwmProfile`), `autoHwmRole`, `autoHwmPolicyClass`(`int`), `autoHwmUnitBudgetBytes`, `autoHwmSocketMessageSlots`(`long`), `autoHwmSizeCap`(`int`) |
| Connection bucket | `autoHwmConnectionBucketEnabled`(`boolean`), `autoHwmConnectionBucketCount`/`Index`/`Hwm4K`(`int`), `autoHwmConnectionBucketHysteresisRetained`(`boolean`) |
| Auto-HWM plan(byte) | `autoHwmEffectiveMessageBytes`, `autoHwmPlannedSendHwmBytes`/`PlannedRecvHwmBytes`, `autoHwmAppliedSendHwmBytes`/`AppliedRecvHwmBytes`(`long`), `autoHwmAppliedSndBuffer`/`AppliedRcvBuffer`(`int`) |
| Auto-HWM recalc | `autoHwmLastRecalcMs`(`long`), `autoHwmLastRecalcReason`(`AutoHwmRecalcReason`), `autoHwmSendBlockedRatioPpm`(`int`) |
| Auto-HWM deferred shrink | `autoHwmDeferredSendHwmBytes`/`DeferredRecvHwmBytes`(`long`, 대응하는 `autoHwmDeferredSendHwmValid`/`DeferredRecvHwmValid` `boolean`이 true일 때만 유효) |
| In-flight/과금 | `sendBytesInFlight`, `recvBytesInFlight`, `minimumCoreMessageChargeBytes`, `oversizeMessageAdmissionCount`, `oversizeMessageAdmissionMaxBytes`(`long`) |

**Completion result.** 해당 없음 — 불변 record 스냅샷.

**선택 기준.** `stateFlags`를 직접 디코딩하는 대신 `isReady()`를 호출한다.
socket의 실제 send/receive HWM이 설정한 `CommonSocketOptions` 값(Sockets
category)과 다른 이유를 진단할 땐 connection-bucket과 auto-HWM-plan
component를 쓴다.

---

## `Poller`

socket, file descriptor, timer를 하나의 재사용 가능한 wait로 multiplex한다.

```java
try (Poller poller = Zlink.createPoller()) {
    poller.add(dealer, 1L, PollEventFlags.POLLIN);
    poller.add(timer, 2L);
    PollEvents events = new PollEvents(8);
    int ready = poller.wait(events, Duration.ofSeconds(1));
    for (int i = 0; i < events.readyCount(); i++) {
        PollEvent event = events.eventAt(i);
    }
}
```

**Options.**

| Member | 의미 |
| --- | --- |
| `add(Socket socket, long slot, PollEventFlags... events)` | socket을 등록; `slot`은 대응하는 poll 결과로 그대로 되돌아오는 caller token — 인자 순서(`slot`이 varargs `events`보다 앞)를 참고 |
| `addFd(int fd, long slot, PollEventFlags... events)` | raw file descriptor를 등록, 같은 `slot`/varargs 형태 |
| `add(ZlinkTimer timer, long slot)` | timer를 socket/fd와 함께 multiplex하도록 등록 |
| `modify(Socket socket, PollEventFlags... events)` | 이미 등록된 socket의 감시 event를 교체 |
| `modifyFd(int fd, PollEventFlags... events)` | 이미 등록된 fd의 감시 event를 교체 |
| `remove(Socket)` / `remove(int fd)` / `remove(ZlinkTimer)` | source 등록을 해제; `boolean` 반환, 실제로 등록돼 있었으면 `true` |
| `clear()` | 모든 source를 한 번에 등록 해제 |
| `size()` | `int`, 현재 등록된 source 개수 |
| `wait(PollEvents events, Duration timeout)` | `timeout`까지 block하며 `events`를 그 자리에서 채움 |

**Completion result.** 등록/제거 member는 동기다. `wait`는 `timeout`까지
block하며, `events`를 그 자리에서 채우고 준비된 개수를 반환한다(이후
`events.readyCount()`로도 읽을 수 있다).

**선택 기준.** 서비스 수명 전체에서 poller 하나를 쓴다. 감시하는 event만
바뀔 땐 `remove` + `add` 대신 `modify`를 선호한다. `wait` 호출마다 새로
할당하는 대신, receive에 `Received`를 재사용하는 것과 같은 방식으로
`PollEvents` buffer 하나를 재사용한다.

---

## `PollEvents`

poller wait를 위한, `capacity`까지 준비된 event를 담는 미리 할당된 결과
buffer — dotnet의 `Span<PollEvent>`/cpp의 raw pointer-and-capacity 쌍과
구별되는 Java 고유 설계다.

```java
PollEvents events = new PollEvents(16);
poller.wait(events, Duration.ofMillis(500));
for (int i = 0; i < events.readyCount(); i++) {
    if (events.hasEvent(i, PollEventFlags.POLLIN)) { /* ... */ }
}
```

**Options.** public 생성자 `PollEvents(int capacity)`(`capacity <= 0`이면
`IllegalArgumentException`). 아래 모든 accessor는 `capacity()`가 아니라
`readyCount()`에 대해 bounds-check되며 범위를 벗어나면
`IndexOutOfBoundsException`을 던진다.

| Member | 의미 |
| --- | --- |
| `capacity()` | 생성자에 넘긴 고정 용량 |
| `readyCount()` | 마지막 `wait` 이후 몇 개 slot이 준비된 event를 담고 있는지 |
| `sourceKind(int index)` | 해당 index의 준비된 source 종류 |
| `slot(int index)` | 그 source 등록 시 넘긴 caller token |
| `revents(int index)` | 해당 index의 raw `int` poll-event bitmask |
| `hasEvent(int index, PollEventFlags event)` | `revents(index)`에 대한 편의 bit-test |
| `fd(int index)` | 해당 index의 file descriptor, `FD` kind source에서만 채워짐 |
| `eventAt(int index)` | 해당 index의 `PollEvent` record를 materialize |

**Completion result.** 모든 accessor는 동기다. `Poller.wait(...)`는
package-private `markReadyCount`/`markEvent`를 통해 `PollEvents`
instance를 그 자리에서 변경한다 — public contract 표면이 아니다.

**선택 기준.** `revents(index)`를 손으로 bit-test하는 대신
`hasEvent(index, flag)`를 선호한다. caller가 진짜로 boxed `PollEvent`
record가 필요할 때만 `eventAt(index)`를 쓴다 — 개별 accessor를 순회하면
hot polling loop에서 그 할당을 피할 수 있다.

---

## `PollEvent`

`PollEvents.eventAt(index)`가 필요 시 materialize하는, poller wait가
보고하는 준비된 source 하나. Java `record`다.

**Options.** 인자 없음 — 직접 생성하지 않고 `PollEvents.eventAt(...)`으로
얻는다.

| Component | 타입 | 의미 |
| --- | --- | --- |
| `sourceKind` | `PollSourceKind` | source가 `SOCKET`/`FD`/`TIMER` 중 무엇인지 |
| `slot` | `long` | 등록 시 제공한 caller token |
| `revents` | `int` | raw poll-event bitmask |
| `fd` | `int` | file descriptor, `FD` kind source에서만 채워짐 |

**Completion result.** 해당 없음 — 불변 record.

**선택 기준.** `sourceKind()`/`slot()`으로 분기해 각 결과를 대응하는
socket·descriptor·timer로 연결한다.

---

## `ZlinkTimer`

interval마다 fire하며 (`recv`로) poll하거나 poller를 통해 구동할 수 있는
standalone timer.

```java
try (ZlinkTimer timer = Zlink.createTimer()) {
    timer.onFire((t, count) -> logger.info("fired {} times", count));
    timer.start(Duration.ofSeconds(1), 0L);
}
```

**Options.**

| Member | 의미 |
| --- | --- |
| `start(Duration interval, long repeatCount)` | `interval`마다 fire를 시작; `repeatCount == 0`은 무제한 |
| `stop()` | fire를 멈춤; `start`로 재시작 가능 |
| `recv()` | `long`을 직접 반환 — 누적 fire count; dotnet의 `ulong?`/cpp의 `std::optional<uint64_t>`와 달리 소스에선 nullable/optional이 아니다 |
| `onFire(TimerHandler handler)` | 수동적 interval 콜백을 등록; handler는 `(ZlinkTimer timer, long fireCount)`를 받음 |

**Completion result.** 모든 member는 동기다. `ZlinkTimer extends
AutoCloseable`이다.

**선택 기준.** 수동적 interval 콜백엔 `onFire`를, 대신 만료를 poll하려면
`recv`를, socket과 함께 하나의 wait에서 multiplex하려면
`Poller.add(ZlinkTimer, long)`로 등록한다.

---

## Handler functional interface

| Interface | 등록하는 곳 | Signature |
|---|---|---|
| `SocketMonitorHandler` | `SocketMonitor.onEvent(...)` | `void onEvent(MonitorEvent event)` |
| `TimerHandler` | `ZlinkTimer.onFire(...)` | `void onFire(ZlinkTimer timer, long fireCount)` |

---

## Eventing enum

위 모든 항목에서 참조하는 공유 enum.

| Enum | 사용처 | 값 |
|---|---|---|
| `MonitorEventType` | `Socket.monitorOpen(MonitorEventType...)`(Sockets category), `MonitorEvent.event()` | `CONNECTED`, `CONNECT_DELAYED`, `CONNECT_RETRIED`, `LISTENING`, `BIND_FAILED`, `ACCEPTED`, `ACCEPT_FAILED`, `CLOSED`, `CLOSE_FAILED`, `DISCONNECTED`, `MONITOR_STOPPED`, `HANDSHAKE_FAILED_NO_DETAIL`, `CONNECTION_READY`, `HANDSHAKE_FAILED_PROTOCOL`, `HANDSHAKE_FAILED_AUTH`, `PEER_WEIGHT_CHANGED`, `ALL` — varargs 집합을 raw mask로 OR하는 static `combine(MonitorEventType...)` helper가 있음 |
| `MonitorSourceKind` | `MonitorStatus.sourceKind()` | `SOCKET` |
| `MonitorStateFlags` | `MonitorStatus.stateFlags()`(`EnumSet`으로) | `READY`, `BOUND_READY`, `CLOSED` |
| `MonitorStatusDetailFlags` | `MonitorStatus.detailFlags()`(`EnumSet`으로) | `SEND_PENDING_MESSAGES`, `RECEIVE_PENDING_MESSAGES`, `AUTO_HWM_BUDGET`, `AUTO_HWM_BUFFERS` |
| `PollSourceKind` | `PollEvent.sourceKind()`, `PollEvents.sourceKind(int)` | `SOCKET`, `FD`, `TIMER` |
| `PollEventFlags` | `Poller.add`/`modify`/`wait`, `PollEvents.hasEvent(...)` | `POLLIN`, `POLLOUT`, `POLLERR`, `POLLPRI`, `POLLCOMPLETION` — `NONE` member가 없다(빈 상태는 그냥 flag를 하나도 주지 않는 것이지 열거자가 아니다) |

**선택 기준.** Java는 bitmask 형태의 `MonitorStateFlags`/
`MonitorStatusDetailFlags`/`PollEventFlags`를 `[Flags]`가 붙은 enum(dotnet)이나
비트 OR 가능한 flag class(cpp)가 아니라, `EnumSet`/varargs로 소비되는 순수
`enum` 타입으로 노출한다 — enum 상수 자체에 비트 OR를 하는 게 아니라 여러
개를 varargs로 넘기거나 `EnumSet`으로 되읽어서 결합한다.

---

[`contracts/eventing/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/eventing/)와
[Java 바인딩 스펙](../../spec/java/README.ko.md)에서 전체 근거를 확인한다.
