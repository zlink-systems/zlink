한국어 | [English](15-polling.en.md)

[레퍼런스 목차](README.ko.md)

# 15. Polling and pollers

이 category는 raw socket, file descriptor, generic timer를 하나의 event loop에서
기다리는 진입점 — 일회성 `zlink_poll`과 재사용 가능한 poller family를 다룬다. Poller
readiness는 Core의 세 event family 중 하나다(나머지 둘은 socket monitor 이벤트와 timer
fire — Socket monitor·Timers category 참고). 정확한 signature는
[Polling 스펙](../spec/core/06-polling.ko.md)이 소유한다.

---

## `zlink_poll`

Caller가 제공한 poll item 배열을 한 번 기다린다.

```c
zlink_pollitem_t items[2] = {
    { .socket = s1, .events = ZLINK_POLLIN },
    { .socket = s2, .events = ZLINK_POLLIN | ZLINK_POLLOUT },
};
zlink_config_result_t err;
int ready = zlink_poll(items, 2, /*timeout_ms=*/1000, &err);
```

**Parameters.** `items`는 `zlink_pollitem_t`(`socket`/`fd`/`events`/`revents`) 배열이다.
`item_count`는 배열 길이다. `timeout_ms`는 무한이면 `-1`, 즉시 반환이면 `0`이다.
`error_out`은 실패 상세를 받는다.

**Return과 errno.** 준비된 item 수를 반환하고, timeout이면 `0`, 실패면 `-1`을 반환하며
`error_out`과 `errno` 둘 다 설정된다.

**선택 기준.** 작고 고정된 item 집합에 대한 단일 대기에 쓴다 — 각 item의 `revents`는
평가 전에 지워지므로 반환 직후의 스냅샷일 뿐이다. Item 집합이 자주 바뀌거나
`fd`·timer source가 필요하면 대신 아래 재사용 가능한 poller family를 쓴다 — 매 호출마다
배열을 다시 훑는 것은 점진적으로 유지되는 poller만큼 확장성이 좋지 않다.

---

## `zlink_poller_new` / `zlink_poller_destroy` / `zlink_poller_size`

재사용 가능한 poller를 만들거나, 파괴하거나, 현재 보유한 source 개수를 읽는다.

```c
void *poller = zlink_poller_new();
// ...
zlink_config_result_t err;
int n = zlink_poller_size(poller, &err);
// ...
zlink_poller_destroy(&poller);
```

**Parameters.** `new`는 인자가 없다. `destroy`는 `void **poller_p`를 받는다(handle을
비울 수 있다). `size`는 poller와 `error_out` 출력을 받는다.

**Return과 errno.** `new`는 poller handle 또는 `NULL`을 반환한다. `destroy`는
`zlink_close_result_t`를 반환한다 — `wait`가 진행 중일 때 파괴하면
`ZLINK_CLOSE_BUSY`와 `EBUSY`. `size`는 개수를, 실패하면 `-1`을 반환한다.

**선택 기준.** Poller 인스턴스 하나가 필요한 만큼의 source를 처리하며 여러 `wait` 호출에
재사용된다 — 서로 다른 poller는 동시에 쓸 수 있지만, caller는 하나의 poller에서
add/modify/remove/wait를 직렬화해야 한다.

---

## `zlink_poller_add` / `zlink_poller_modify` / `zlink_poller_remove`

Poller에 raw socket source를 등록·갱신·제거한다.

```c
zlink_poller_add(poller, s, userdata, ZLINK_POLLIN);
zlink_poller_modify(poller, s, ZLINK_POLLIN | ZLINK_POLLOUT);
zlink_poller_remove(poller, s);
```

**Parameters.** `source`는 socket handle이다. `user_data`(add에만)는 일치하는
`zlink_poller_event_t` 항목으로 되돌려받는 borrowed pointer다. `events`는
`zlink_pollitem_t`와 같은 `zlink_poller_event_mask_t` bit에 더해
`ZLINK_POLLCOMPLETION`(raw DEALER나 ROUTER를 추가할 때만 유효 — 아래 참고)을 받는다.

**Return과 errno.** 셋 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 이미 등록된 source를 추가하면 `ZLINK_CONFIG_CONFLICT`와
`EEXIST`. 없는 source를 갱신·제거하면 `ZLINK_CONFIG_NOT_FOUND`와 `ENOENT`. 잘못된
event bit면 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `EINVAL`. Source가 지원하지 않는
event면 `ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`.

**선택 기준.** Poller는 source handle을 빌릴 뿐이다 — 파괴하기 전에 source를 제거한다.
DEALER나 ROUTER를 추가할 때 `ZLINK_POLLCOMPLETION`을(단독으로, 또는
`ZLINK_POLLIN`/`ZLINK_POLLOUT`과 OR로) 설정하면 하나의 poller가 수신·송신·request 완료
진행을 함께 소유한다 — 다른 source, `zlink_poll` item, `zlink_poller_modify`에 쓰면
`ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`을 반환한다. 등록된 source가 닫히면
`POLLERR`를 한 번 만들며 명시적으로 제거될 때까지 등록 상태로 남는다.

---

## `zlink_poller_add_fd` / `zlink_poller_modify_fd` / `zlink_poller_remove_fd`

Poller에 raw platform file-descriptor source를 등록·갱신·제거한다.

```c
zlink_poller_add_fd(poller, fd, userdata, ZLINK_POLLIN);
```

**Parameters.** `fd`는 `zlink_fd_t`(플랫폼 file descriptor/handle)다. 나머지는
`zlink_poller_add`/`_modify`/`_remove`와 모양이 같다.

**Return과 errno.** 위 socket-source 삼총사와 같다 — 중복 add면
`ZLINK_CONFIG_CONFLICT`/`EEXIST`, 없는 source면 `ZLINK_CONFIG_NOT_FOUND`/`ENOENT`.

**선택 기준.** 순수 OS file descriptor를 socket·timer와 같은 event loop에 접어 넣을 때
쓴다 — readiness는 raw-socket readiness 규칙이 아니라 플랫폼 poll semantics(readable/
writable)를 따른다.

---

## `zlink_poller_add_timer` / `zlink_poller_remove_timer`

Poller에 generic timer source(Timers category)를 등록·제거한다.

```c
zlink_poller_add_timer(poller, timer, userdata);
zlink_poller_remove_timer(poller, timer);
```

**Parameters.** `timer`는 `zlink_timer_new`(Timers category)의 handle이다. `events`
mask는 없다 — timer source는 항상 `POLLIN`만 신호한다(fire count를 받을 수 있음).

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 다른 두 source
family와 같은 conflict/not-found 매핑이다.

**선택 기준.** Socket·FD와 같은 `wait` loop로 timer fire를 받고,
`wait`가 알리면 `zlink_timer_recv`(Timers category)로 누적된 count를 소진할 때 쓴다.

---

## `zlink_poller_wait`

Poller에 현재 등록된 모든 source에 대한 readiness를 기다린다.

```c
zlink_poller_event_t events[16];
zlink_config_result_t err;
int ready = zlink_poller_wait(poller, events, 16, /*timeout_ms=*/1000, &err);
```

**Parameters.** `events`/`event_capacity`는 caller 소유 출력 배열과 그 크기다.
`timeout_ms`와 `error_out`은 `zlink_poll`과 같은 관례를 따른다.

**Return과 errno.** 쓰인 준비된 이벤트 수를 반환하고, timeout이면 `0`, 실패면 `-1`을
반환하며 `error_out`/`errno`가 설정된다. 각 `zlink_poller_event_t`는
`source_kind`(`SOCKET`/`FD`/`TIMER` — 일치하는 `socket`/`fd`/`timer` 중 하나만
유효), `user_data`(등록 시의 borrowed pointer), `events`를 보고한다.

**선택 기준.** 반환된 배열은 caller 소유이며 Core storage에 대한 pointer를 담지
않는다. `ZLINK_POLLCOMPLETION` 신호만 내부적으로 처리됐으면(DEALER/ROUTER request 완료,
Socket lifecycle category와 DEALER/ROUTER category) `wait`는 공개 이벤트 없이 `0`을
반환할 수 있다 — caller는 reply callback이 이미 바꾼 상태를 살펴보고 계속 진행할 수
있다 — `recv_part` family는 이 완료 신호를 스스로 소진하지 않는다.

---

## Source별 readiness

| Source | `POLLIN` | `POLLOUT` | 추가 규칙 |
|---|---|---|---|
| raw socket | 완결된 record를 받을 수 있음 | submit 재시도가 가치 있음 | Socket별 수신 모드가 적용됨 |
| timer | fire count를 받을 수 있음 | 지원 안 함 | `zlink_timer_recv()`(Timers category)로 소진 |
| FD | 플랫폼에서 읽기 가능 | 플랫폼에서 쓰기 가능 | 플랫폼 poll semantics |

`ZLINK_POLLITEMS_DFLT`는 stack buffer용 권장 초기 item 개수 힌트일 뿐 readiness bit가
아니다. `ZLINK_HAVE_POLLER == 1`은 이 공개 poller API가 빌드에 포함되어 있다는
뜻이다.

---

전체 근거는 [Polling 스펙](../spec/core/06-polling.ko.md)을 참고한다.
