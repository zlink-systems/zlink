
[레퍼런스 목차](README.ko.md)

# 16. Timers

이 category는 독립 generic timer의 진입점 — 나노초 해상도의 주기적·일회성 스케줄링,
동기 pull 소진, 그리고 선택적으로 poller에 통합(`zlink_poller_add_timer`/
`zlink_poller_remove_timer`, Polling and pollers category)하는 것을 다룬다. Timer fire는
Core의 세 event family 중 하나다(나머지 둘은 socket monitor 이벤트와 poller readiness —
Socket monitor·Polling and pollers category 참고). 정확한 signature는
[Utilities 스펙](../spec/core/07-utilities.ko.md)이 소유한다.

---

## `zlink_timer_new` / `zlink_timer_destroy`

독립 timer handle을 만들거나 파괴한다.

```c
void *timer = zlink_timer_new();
// ...
zlink_timer_destroy(&timer);
```

**Parameters.** `new`는 인자가 없다. `destroy`는 `void **timer_p_`를 받는다(파괴 뒤
`NULL`로 지워짐).

**Return과 errno.** `new`는 timer handle을, 실패하면 `NULL`을 반환하며 `errno`가
설정된다. `destroy`는 `zlink_close_result_t`를 반환한다 — 성공하면
`ZLINK_CLOSE_OK` — 실행 중이면 먼저 timer를 멈춘다.

**선택 기준.** Application이 필요로 하는 독립 schedule마다 timer handle을 하나
만든다. 정확히 한 번 파괴하고, 다른 스레드가 같은 handle을 쓰는 동안에는 절대
파괴하지 않는다.

---

## `zlink_timer_start` / `zlink_timer_stop`

Timer를 일정 간격으로 발화하도록 arm하거나 disarm한다.

```c
zlink_timer_start(timer, /*interval_ns=*/100_000_000, /*repeat_count=*/0);
// ...
zlink_timer_stop(timer);
```

**Parameters.** `interval_ns_`는 나노초 단위 발화 간격이다. `repeat_count_`는 timer가
스스로 멈추기 전 발화 횟수다(`0`이면 `stop`을 명시적으로 부를 때까지 무한).

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`.

**선택 기준.** 명시적 stop이 필요 없는 유계 일회성이나 고정 횟수 schedule에는 유한한
`repeat_count_`를, 계속되는 주기 timer에는 `0`을 쓰고 끝나면 `stop`을 호출한다. 둘 다
같은 timer의 다른 operation과 동시에 호출하면 안전하지 않다.

---

## `zlink_timer_recv`

Timer fire를 동기 pull API로 소진한다.

```c
uint64_t fire_count;
zlink_timer_recv(timer, &fire_count);
```

**Parameters.** 출력 `fire_count_out_`(누적 fire count)를 받는다.

**Return과 errno.** `recv`는 `zlink_recv_result_t`를 반환한다 — 성공하면
`ZLINK_RECV_OK`. Timer가 멈췄고 받을 fire 이벤트가 없으면
`ZLINK_RECV_NO_DATA`(`EAGAIN`)를 반환한다.

**선택 기준.** 발화를 동기적으로 poll-style 대기하려면 `recv`를 쓴다. 같은 timer의
다른 operation과 동시에 호출하면 안전하지 않다. Timer를 socket·FD와 함께 기존 event loop에 통합하려면
`zlink_poller_add_timer`(Polling and pollers category)를 쓰고, poller가 준비됐다고
알리면 `zlink_timer_recv`로 소진한다.

---

전체 근거는 [Utilities 스펙](../spec/core/07-utilities.ko.md)을 참고한다.
