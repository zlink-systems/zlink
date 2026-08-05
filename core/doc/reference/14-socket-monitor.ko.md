한국어 | [English](14-socket-monitor.en.md)

[레퍼런스 목차](README.ko.md)

# 14. Socket monitor

이 category는 routing이나 queue 상태를 바꾸지 않고 raw socket의 connection·transport·
protocol·lifecycle 상태를 관찰하는 진입점을 다룬다. Monitor는 Core의 세 event family 중
하나다(나머지 둘은 poller readiness와 timer fire — Polling and pollers category
참고) — 관찰 대상 socket에는 절대 영향을 주지 않는다. 정확한 signature는
[Monitoring 스펙](../spec/core/07-monitoring.ko.md)이 소유한다.

---

## `zlink_socket_monitor_open`

Socket에 대한 monitor를 만든다 — recv model로 시작한다.

```c
zlink_socket_monitor_open_options_t options = { .events = ZLINK_EVENT_ALL };
void *monitor = zlink_socket_monitor_open(s, &options);
```

**Parameters.** `options_->events`는 관찰할 이벤트를 고르는
`ZLINK_SOCKET_MONITOR_EVENT_*`(더 짧은 `ZLINK_EVENT_*` alias) bitmask다 —
`CONNECTED`/`CONNECT_DELAYED`/`CONNECT_RETRIED`/`LISTENING`/`BIND_FAILED`/`ACCEPTED`/
`ACCEPT_FAILED`/`CLOSED`/`CLOSE_FAILED`/`DISCONNECTED`/`MONITOR_STOPPED`/
`HANDSHAKE_FAILED_NO_DETAIL`/`CONNECTION_READY`/`HANDSHAKE_FAILED_PROTOCOL`/
`HANDSHAKE_FAILED_AUTH`/`PEER_WEIGHT_CHANGED`, 전부는 `ALL`(`0xFFFF`) — `events == 0`은
아무것도 선택하지 않는다.

**Return과 errno.** 성공하면 monitor handle을, 실패하면 `NULL`을 반환하며 `errno`가
설정된다.

**선택 기준.** 관찰해야 하는 socket마다 한 번 호출한다. Monitor는 **recv model**로
시작한다 — `zlink_socket_monitor_recv`로 이벤트를 뽑거나,
`zlink_socket_monitor_handler`로 callback 전용 model로 전환한다. 더 이상 필요 없으면
`zlink_monitor_close`로 handle을 닫는다.

---

## `zlink_socket_monitor_handler` / `zlink_socket_monitor_recv`

Monitor를 callback 전달로 전환하거나, 큐잉된 다음 이벤트를 뽑는다 — raw socket의 수신
모드(Raw receive category)처럼 서로 배타적인 모드다.

```c
zlink_socket_monitor_handler(monitor, on_monitor_event, userdata);
// 또는 recv model에서:
zlink_monitor_event_t event;
zlink_socket_monitor_recv(monitor, &event, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `handler`는 `zlink_monitor_handler_fn`과 `userdata_`를 받는다. `recv`는
`event_out_` 출력 구조체와 `flags_`(`ZLINK_RECV_FLAGS_NONE` 또는 `_DONTWAIT`)를 받는다.

**Return과 errno.** `handler`는 `zlink_handler_result_t`를 반환한다 — 성공하면
`ZLINK_HANDLER_OK`. 이미 다른 모드에 있는데 handler 모드를 활성화하면 `EBUSY`. `recv`는
`zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`.

**선택 기준.** Recv model에서는 이벤트 주소와 routing ID가 caller 소유 출력 구조체 안의
값이고, handler model에서는 callback의 이벤트 pointer와 담긴 값이 callback이 반환될
때까지만 유효한 borrowed view다. `DISCONNECTED.value`는 `zlink_disconnect_reason_t`,
`HANDSHAKE_FAILED_PROTOCOL.value`는 `zlink_protocol_error_t`,
`PEER_WEIGHT_CHANGED.value`는 `0..10000` 범위의 새 weight다 — 다른 실패 이벤트는 그
실패의 errno를 담는다. Monitor queue는 유계다 — 가득 차면 Core는 동일한 고빈도 이벤트를
집계하고 connection-state·protocol-error·lifecycle 이벤트를 우선한다 — 다음
`zlink_monitor_status` 스냅샷이 집계된 개수를 반영한다. 지연된 consumer가 관찰 대상
socket의 raw-socket submission을 막는 일은 없다. 하나의 monitor 안에서 이벤트는 commit
순서로 큐잉된다 — connection I/O thread 사이의 wall-clock 순서는 보장하지 않는다.

---

## `zlink_monitor_status`

Monitor 자신의 상태와 관찰 대상 socket의 automatic-HWM 회계에 대한 시점 스냅샷을 읽는다.

```c
zlink_monitor_status_t status;
zlink_monitor_status(monitor, &status);
```

**Parameters.** Monitor handle과 caller 소유 `zlink_monitor_status_t *status_out_`만
받는다.

**Return과 errno.** `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. `status_out_->abi_version`은 `ZLINK_MONITOR_STATUS_ABI_VERSION`(현재
`2`)이다. `detail_flags`의 각 bit는 field 행 정확히 하나를 유효하게
만든다(`SND_PENDING_MSGS`/`RCV_PENDING_MSGS`/`AUTO_HWM_BUDGET`/`AUTO_HWM_BUFFERS` —
bit별 정확한 field 목록은 Monitoring 스펙의 detail-bit 표 참고) — 존재하는 bit의 행 밖의
field는 0이다. Connection bucket이 적용되지 않으면
`auto_hwm_connection_bucket_index`는 `UINT32_MAX`다.

**선택 기준.** 이벤트 스트림 자체가 아니라 현재 HWM 계획·적용 상태(계획 대비 적용된 byte
HWM, in-flight 바이트, oversize-admission 카운터)가 필요한 진단 대시보드나 health check에
쓴다. Version 2의 HWM field는 이전 32비트 count가 아니라 64비트 바이트다 — 이전 layout은
호환 fallback으로 받아들여지지 않는다.

---

## `zlink_monitor_close`

Monitor를 닫고 자원을 해제한다.

```c
zlink_monitor_close(&monitor);
```

**Parameters.** `void **monitor_p` — handle에 대한 pointer를 받으며, 호출이 그것을
비울 수 있다.

**Return과 errno.** `zlink_close_result_t`를 반환한다 — 성공하면 `ZLINK_CLOSE_OK`.

**선택 기준.** 관찰이 끝나면 연 monitor마다 정확히 한 번 닫는다.

---

## `zlink_monitor_ignore_handler`

아무 동작도 하지 않고 이벤트를 흘려보내는 no-op event handler다.

```c
zlink_socket_monitor_handler(monitor, zlink_monitor_ignore_handler, NULL);
```

**Parameters.** `zlink_monitor_handler_fn` signature와 일치해 바로 등록할 수 있다.

**Return과 errno.** 없음 — 이벤트나 `userdata_`를 보관하지도 해제하지도 않는다.

**선택 기준.** Handler-mode semantics는 원하지만(monitor queue가 무한정 커지지 않도록)
application이 지금 당장 이벤트로 할 일이 없을 때 등록한다 — `event`는 다른 handler-mode
callback과 마찬가지로 호출 동안만 유효한 borrowed view다.

---

전체 근거는 [Monitoring 스펙](../spec/core/07-monitoring.ko.md)을 참고한다.
[Events 카탈로그](../spec/core/05-events.ko.md)는 monitor 이벤트를 나머지 두 event
family인 poller readiness·timer fire와 연결한다.
