한국어 | [English](17-utilities.en.md)

[레퍼런스 목차](README.ko.md)

# 17. Utilities

이 category는 messaging API를 보완하는 독립 helper 진입점 — atomic counter, 고해상도
stopwatch, 그 밖의 프로세스 helper(capability 조회, proxy loop, sleep, OS thread 관리)를
다룬다. 어느 것도 context나 socket에 의존하지 않는다. 정확한 signature는
[Utilities 스펙](../spec/core/08-utilities.ko.md)이 소유한다.

---

## `zlink_atomic_counter_new` / `zlink_atomic_counter_destroy`

0으로 초기화된 atomic counter를 만들거나 파괴한다.

```c
void *counter = zlink_atomic_counter_new();
// ...
zlink_atomic_counter_destroy(&counter);
```

**Parameters.** `new`는 인자가 없다. `destroy`는 `void **counter_p_`를 받는다(파괴 뒤
`NULL`로 지워짐).

**Return과 errno.** `new`는 counter handle을, 실패하면(메모리 부족) `NULL`을
반환한다. `destroy`는 반환값이 없다(`void`).

**선택 기준.** Application이 스레드 사이에서 필요로 하는 독립 공유 count마다 counter
하나를 만든다. 다른 스레드가 같은 counter를 조작하는 동안에는 절대 `destroy`를
호출하지 않는다.

---

## `zlink_atomic_counter_set` / `zlink_atomic_counter_value`

Counter를 명시적 값으로 설정하거나 현재 값을 읽는다.

```c
zlink_atomic_counter_set(counter, 0);
int current = zlink_atomic_counter_value(counter);
```

**Parameters.** `set`은 새 `value_`를 받는다. `value`는 counter handle만 받는다.

**Return과 errno.** `set`은 반환값이 없다. `value`는 atomic하게 읽은 현재 값을
반환한다.

**선택 기준.** `set`은 다른 스레드가 counter를 조작하기 시작하기 전, setup 중에만
쓴다 — `inc`/`dec`/`value`와 달리 `set`은 같은 counter의 다른 operation과 동시에
호출하면 안전하지 않다. `value`는 언제 어느 스레드에서 호출해도 안전하다.

---

## `zlink_atomic_counter_inc` / `zlink_atomic_counter_dec`

Counter를 atomic하게 1 증가·감소시킨다.

```c
int previous = zlink_atomic_counter_inc(counter);
int still_nonzero = zlink_atomic_counter_dec(counter);
```

**Parameters.** 둘 다 counter handle만 받는다.

**Return과 errno.** `inc`는 증가 *직전*의 값을 반환한다. `dec`는 감소 후에도
counter가 0보다 크면 `1`, 0에 도달했으면 `0`을 반환한다 — 숫자 값이 아니다.

**선택 기준.** `dec`가 0 도달 여부를 직접 반환하는 특성을 별도 `value` 읽기 없이
"마지막 하나가 나갔다" 신호(예: reference count가 0에 도달했는지 확인)로 그대로
쓴다 — 이 결합된 확인-후-감소가 하나의 operation으로 atomic한 이유다.

---

## `zlink_stopwatch_start` / `zlink_stopwatch_intermediate` / `zlink_stopwatch_stop`

고해상도 stopwatch를 시작하거나, 멈추지 않고 경과 시간을 읽거나, 멈추고 총 시간을
읽는다.

```c
void *watch = zlink_stopwatch_start();
// ... 작업 ...
unsigned long partial_us = zlink_stopwatch_intermediate(watch);
// ... 더 작업 ...
unsigned long total_us = zlink_stopwatch_stop(watch);
```

**Parameters.** `start`는 인자가 없다. `intermediate`/`stop`은 watch handle만
받는다.

**Return과 errno.** `start`는 opaque handle을, 실패하면 `NULL`을 반환한다.
`intermediate`와 `stop` 둘 다 `start` 이후 경과한 마이크로초를 반환한다. `stop`은
handle도 해제한다 — 이후 사용하면 안 된다.

**선택 기준.** 하나의 측정 구간 동안 연속 읽기가 필요한 만큼 `intermediate`를 부르고,
마치고 handle을 해제할 때 정확히 한 번 `stop`을 부른다. 한 번에 하나의 스레드에서
하나의 handle을 쓴다 — 같은 handle에서 `intermediate`를 `stop`과 동시에 호출하면
안 된다.

---

## `zlink_has`

현재 library 빌드가 이름 붙은 capability를 제공하는지 확인한다.

```c
bool has_tls = zlink_has("tls");
```

**Parameters.** `capability_`는 non-`NULL`, 널 종료 문자열이며 호출이 보관하지
않는다.

**Return과 errno.** `bool`을 반환한다 — `"tcp"`는 항상 `true`. `"ipc"`, `"tls"`,
`"ws"`, `"wss"`는 빌드에 그 capability가 있을 때만 `true`. 그 밖의 문자열은
`false`.

**선택 기준.** 모든 transport가 컴파일에 포함되어 있다고 가정하는 대신, startup에
이걸로 선택적 빌드 capability를 분기한다(예: `zlink_has("tls")`가 `false`면 TLS
옵션 구성을 건너뜀).

---

## `zlink_proxy` / `zlink_proxy_steerable`

두 raw socket 사이의 양방향 forwarding loop를 실행하며, 끝날 때까지 호출한 스레드를
block한다.

```c
zlink_proxy(frontend, backend, capture); // capture는 NULL일 수 있음

// 또는 외부 제어와 함께:
zlink_proxy_steerable(frontend, backend, capture, control);
```

**Parameters.** `frontend_`/`backend_`는 proxy가 multipart 메시지를 주고받을
raw socket handle이며 필수다. `capture_`는 선택적이다 — non-`NULL`이면 전달되는
모든 메시지의 복사본을 받는다. `zlink_proxy_steerable`은 추가로
`PAUSE`/`RESUME`/`TERMINATE`/`STATISTICS` 명령을 받는 선택적 `control_` socket을
받는다. 모든 handle은 빌린 것이다 — 어느 함수도 handle을 닫거나 소유권을 갖지
않는다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — proxy loop가
정상적으로 끝나면 `ZLINK_CONFIG_OK`. 필수 handle이 `NULL`이거나 non-`NULL`
handle이 raw socket이 아니면 `ZLINK_CONFIG_INVALID_HANDLE`.

**선택 기준.** 런타임 제어 없이 전용 스레드에서 실행할 fire-and-forget forwarding
loop에는 평범한 `zlink_proxy`를 쓴다. Application이 다른 스레드에서 control
socket을 통해 loop를 멈추거나·재개하거나·깔끔히 종료하거나 통계를 뽑아야 하면
`zlink_proxy_steerable`을 쓴다 — `STATISTICS` 응답은 control socket의 일반 raw
send/receive 계약을 따른다. `zlink_proxy_steerable`은 `TERMINATE`, context 종료,
또는 오류가 끝낼 때까지 block한다.

---

## `zlink_sleep`

호출한 스레드를 최소 주어진 초만큼 재운다.

```c
zlink_sleep(1);
```

**Parameters.** `seconds_`는 최소 sleep 시간(정수 초)이다.

**Return과 errno.** 없음(`void`).

**선택 기준.** 초 단위 정밀도만 필요할 때 플랫폼 전용 sleep 호출 대신 이 portable
편의 wrapper를 쓴다.

---

## `zlink_thread_start` / `zlink_thread_join`

주어진 함수를 실행하는 새 OS thread를 시작하거나, 끝나기를 기다리며 handle을
해제한다.

```c
void *thread = zlink_thread_start(worker_fn, arg);
// ...
zlink_thread_join(thread);
```

**Parameters.** `start`는 `zlink_thread_fn *func_`와 `arg_`(함수의 유일한
인자로 그대로 전달됨)를 받는다. `join`은 thread handle을 받는다.

**Return과 errno.** `start`는 opaque thread handle을, 실패하면 `NULL`을
반환한다. `join`은 반환값이 없고(`void`) handle을 해제한다 — 이후 사용하면 안
된다.

**선택 기준.** 플랫폼 전용 API 대신 portable 백그라운드 thread에 이 쌍을 쓴다.
Handle마다 정확히 한 번 `join`을 호출하고, join되는 스레드 자신에서는 호출하지
않는다.

---

전체 근거는 [Utilities 스펙](../spec/core/08-utilities.ko.md)을 참고한다.
