---
title: "유틸리티"
---

[English](08-utilities.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Monitoring](07-monitoring.ko.md) | [다음: Runtime Boundary](09-runtime-boundary.ko.md)
<!-- zlink-nav:end -->

# 유틸리티

> **이 장이 정의하는 것** — version 조회, 기능 감지 등 개별 카테고리에 속하지 않는
> 유틸리티 API의 공개 계약.

이 문서는 ZLink Core의 atomic counter, timer, 고해상도 clock과 thread helper 공개 계약을
정의한다. 대상 독자는 이러한 utility의 lifecycle, thread-safety와 callback ownership을 C API와
bindings에 투영하는 개발자다. 이 문서는 “공통 runtime 기능을 사용할 때 각 handle과 callback의 수명,
동시 호출 범위와 반환값을 어떻게 해석하는가?”에 답한다.

## 원자적 카운터

원자적 카운터는 공유 정수에 대한 원자적 증가, 감소, 읽기 작업을 제공합니다.
카운터는 `zlink_atomic_counter_new`로 생성하고
`zlink_atomic_counter_destroy`로 파괴해야 합니다.

### zlink_atomic_counter_new

0으로 초기화된 새 원자적 카운터를 생성합니다.

```c
ZLINK_EXPORT void *zlink_atomic_counter_new (void);
```

초기값이 0인 원자적 카운터에 대한 불투명 핸들을 할당하고 반환합니다.

**반환값:** 성공 시 카운터 핸들, 실패 시 `NULL` (메모리 부족).

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_atomic_counter_set`, `zlink_atomic_counter_destroy`

---

### zlink_atomic_counter_set

카운터를 명시적 값으로 설정합니다.

```c
ZLINK_EXPORT void zlink_atomic_counter_set (void *counter_, int value_);
```

현재 카운터 값을 `value_`로 교체합니다.

**스레드 안전성:** 스레드 안전하지 않습니다. 같은 카운터에 대한 다른 작업과
동시에 호출하지 마세요. 보통 초기 설정 시에만 사용합니다.

**참고:** `zlink_atomic_counter_value`

---

### zlink_atomic_counter_inc

카운터를 1 증가시킵니다.

```c
ZLINK_EXPORT int zlink_atomic_counter_inc (void *counter_);
```

카운터를 원자적으로 증가시키고 이전 값(증가 직전의 값)을 반환합니다.

**반환값:** 증가 전 카운터 값.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_atomic_counter_dec`

---

### zlink_atomic_counter_dec

카운터를 1 감소시킵니다.

```c
ZLINK_EXPORT int zlink_atomic_counter_dec (void *counter_);
```

카운터를 원자적으로 감소시키고, 감소 후에도 0보다 크면 `1`을, 0에 도달하면
`0`을 반환합니다.

**반환값:** 감소 후 카운터가 아직 0이 아니면 `1`, 0에 도달했으면 `0`.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_atomic_counter_inc`

---

### zlink_atomic_counter_value

현재 카운터 값을 반환합니다.

```c
ZLINK_EXPORT int zlink_atomic_counter_value (void *counter_);
```

카운터의 현재 값을 원자적으로 읽습니다.

**반환값:** 현재 카운터 값.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_atomic_counter_set`

---

### zlink_atomic_counter_destroy

카운터를 파괴하고 메모리를 해제합니다.

```c
ZLINK_EXPORT void zlink_atomic_counter_destroy (void **counter_p_);
```

카운터 핸들을 해제합니다. 파괴 후 `*counter_p_`의 포인터는 `NULL`로
설정됩니다.

**스레드 안전성:** 다른 스레드가 동일한 카운터에서 작업 중일 때 호출해서는
안 됩니다.

**참고:** `zlink_atomic_counter_new`

---

## 콜백 타입

```c
typedef void (*zlink_timer_handler_fn) (void *timer_,
                                        uint64_t fire_count_,
                                        void *userdata_);

typedef void (zlink_thread_fn) (void *);
```

`zlink_timer_handler_fn`은 타이머 만료 콜백 시그니처입니다. `timer_`는
발화한 타이머 handle이고 `fire_count_`는 누적 발화 횟수이며 `userdata_`는
핸들러 등록 시 넘긴 사용자 포인터입니다.

`zlink_thread_fn`은 `zlink_thread_start`로 시작되는 스레드의 진입점
시그니처입니다.

## 타이머

나노초 정밀도의 주기적/일회성 generic 타이머를 제공합니다. `zlink_timer_new`로
독립 실행형 타이머를 생성합니다.
타이머는 `zlink_timer_recv`로 동기 수신하거나 `zlink_timer_handler` 콜백으로
구동할 수 있고, `zlink_poller_add_timer`로 poller에 통합할 수도 있습니다.

### zlink_timer_new

독립 실행형 타이머를 생성한다.

```c
ZLINK_EXPORT void *zlink_timer_new (void);
```

불투명 타이머 핸들을 할당해 반환합니다. 더 이상 필요하지 않으면
`zlink_timer_destroy`로 파괴해야 합니다.

**반환값:** 성공 시 타이머 핸들, 실패 시 `NULL`. 실패하면 errno를 설정합니다.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_timer_destroy`

---

### zlink_timer_destroy

타이머를 파괴하고 리소스를 해제한다.

```c
ZLINK_EXPORT zlink_close_result_t zlink_timer_destroy (void **timer_p_);
```

실행 중인 타이머를 정지하고 핸들을 해제합니다. 파괴한 뒤 `*timer_p_`는
`NULL`로 설정됩니다.

**반환값:** 성공 시 `ZLINK_CLOSE_OK`. 실패 시에는 `zlink_close_result_t`
값을 반환한다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지된다.

**스레드 안전성:** 다른 스레드가 같은 타이머를 사용하는 동안 호출해서는 안
됩니다.

**참고:** `zlink_timer_new`

---

### zlink_timer_start

타이머를 시작한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_timer_start (void *timer_,
                                         uint64_t interval_ns_,
                                         uint64_t repeat_count_);
```

`interval_ns_` 나노초 뒤 첫 event를 발생시키도록 타이머를 시작합니다.
`repeat_count_`가 0이면 명시적으로 정지할 때까지 반복하고, 양수이면 해당
횟수만큼 event를 발생시킨 뒤 자동으로 정지합니다.

**매개변수:**

| 이름 | 설명 |
|---|---|
| `timer_` | 타이머 handle |
| `interval_ns_` | event 사이의 간격(나노초) |
| `repeat_count_` | event 발생 횟수. `0`이면 무기한 반복 |

**반환값:** 성공 시 `ZLINK_CONFIG_OK`. 실패 시에는 `zlink_config_result_t`
값을 반환한다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지된다.

**스레드 안전성:** 같은 타이머의 다른 작업과 동시에 호출해서는 안 됩니다.

**참고:** `zlink_timer_stop`

---

### zlink_timer_stop

실행 중인 타이머를 정지한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_timer_stop (void *timer_);
```

타이머를 정지합니다. 다시 시작할 때까지 새 fire event를 생성하지 않습니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`. 실패 시에는 `zlink_config_result_t`
값을 반환한다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지된다.

**스레드 안전성:** 같은 타이머의 다른 작업과 동시에 호출해서는 안 됩니다.

**참고:** `zlink_timer_start`

---

### zlink_timer_recv

타이머 발동을 동기적으로 수신한다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_timer_recv (void *timer_, uint64_t *fire_count_out_);
```

recv 모드에서 다음 타이머 발동을 기다린다. 성공하면 `*fire_count_out_`에
누적 발동 횟수가 설정된다.

**매개변수:**

| 이름 | 설명 |
|---|---|
| `timer_` | 타이머 handle |
| `fire_count_out_` | 누적 발동 횟수를 받을 pointer |

**반환값:** 성공 시 `ZLINK_RECV_OK`. 실패 시에는 `zlink_recv_result_t`
값을 반환한다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지된다.

**에러:** 타이머가 이미 멈췄고 더 읽을 발동이 없으면 `ZLINK_RECV_NO_DATA`
(내부 `EAGAIN`).

**스레드 안전성:** 같은 타이머의 다른 작업과 동시에 호출해서는 안 됩니다.

**참고:** `zlink_timer_handler`, `zlink_timer_start`

---

### zlink_timer_handler

타이머 만료 콜백 핸들러를 등록한다.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_timer_handler (void *timer_,
                                            zlink_timer_handler_fn handler_,
                                            void *userdata_);
```

`handler_`를 등록하면 타이머가 fire할 때마다 호출됩니다. NULL `handler_`는
유효하지 않으며 `ZLINK_HANDLER_INVALID_ARGUMENT`(`EINVAL`)로 실패합니다.
핸들러를 등록한 뒤에는 같은 타이머의 `zlink_timer_recv`가 `ZLINK_RECV_BUSY`를
반환합니다.

callback은 타이머 handle, 누적 발동 횟수와 `userdata_`를 받습니다.

**매개변수:**

| 이름 | 설명 |
|---|---|
| `timer_` | 타이머 handle |
| `handler_` | callback 함수. `NULL`일 수 없음 |
| `userdata_` | callback에 전달할 불투명 pointer |

**반환값:** 성공 시 `ZLINK_HANDLER_OK`. 실패 시에는 `zlink_handler_result_t`
값을 반환한다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지된다.

**스레드 안전성:** 같은 타이머의 다른 작업과 동시에 호출해서는 안 됩니다.

**참고:** `zlink_timer_recv`, `zlink_timer_start`

---

## 스톱워치

벤치마킹 및 프로파일링을 위한 고해상도 타이밍 함수입니다. 스톱워치를 시작하고,
중간 측정값을 읽고, 중지하여 마이크로초 단위의 총 경과 시간을 얻습니다.

### zlink_stopwatch_start

고해상도 스톱워치를 시작합니다.

```c
ZLINK_EXPORT void *zlink_stopwatch_start (void);
```

현재 시간을 캡처하고 경과 시간을 측정하는 데 사용되는 불투명 핸들을
반환합니다. 핸들은 최종적으로 `zlink_stopwatch_stop`으로 해제해야 합니다.

**반환값:** 성공 시 불투명 스톱워치 핸들, 실패 시 `NULL`.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다. 반환된 핸들은 한 번에
하나의 스레드에서만 사용해야 합니다.

**참고:** `zlink_stopwatch_intermediate`, `zlink_stopwatch_stop`

---

### zlink_stopwatch_intermediate

스톱워치를 중지하지 않고 경과 마이크로초를 반환합니다.

```c
ZLINK_EXPORT unsigned long zlink_stopwatch_intermediate (void *watch_);
```

핸들을 해제하지 않고 `zlink_stopwatch_start`가 호출된 이후의 경과 시간을
읽습니다. 연속적인 측정을 위해 여러 번 호출할 수 있습니다.

**반환값:** 마이크로초 단위의 경과 시간.

**스레드 안전성:** 동일한 핸들에서 `zlink_stopwatch_stop`과 동시에 호출해서는
안 됩니다.

**참고:** `zlink_stopwatch_start`, `zlink_stopwatch_stop`

---

### zlink_stopwatch_stop

스톱워치를 중지하고 총 경과 마이크로초를 반환합니다.

```c
ZLINK_EXPORT unsigned long zlink_stopwatch_stop (void *watch_);
```

`zlink_stopwatch_start`가 호출된 이후의 총 경과 시간을 반환하고 스톱워치
핸들을 해제합니다. 이 호출 이후 핸들을 사용해서는 안 됩니다.

**반환값:** 마이크로초 단위의 경과 시간.

**스레드 안전성:** 동일한 핸들에서 다른 작업과 동시에 호출해서는 안 됩니다.

**참고:** `zlink_stopwatch_start`, `zlink_stopwatch_intermediate`

---

## 기타

### zlink_has

현재 library build가 capability를 제공하는지 확인합니다.

```c
ZLINK_EXPORT bool zlink_has (const char *capability_);
```

`capability_`는 NUL로 끝나는 non-NULL 문자열이며 함수는 이 문자열을 보관하지 않습니다. `"tcp"`는 항상
`true`입니다. `"ipc"`, `"tls"`, `"ws"`, `"wss"`는 해당 기능을 포함해 build한 경우에만
`true`입니다. 다른 문자열은 `false`입니다.

**스레드 안전성:** 전역 상태를 바꾸지 않으며 모든 스레드에서 호출할 수 있습니다.

---

### zlink_proxy

두 raw socket 사이에서 멀티파트 메시지를 양방향 전달합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_proxy (void *frontend_, void *backend_, void *capture_);
```

`frontend_`와 `backend_`는 필수 raw socket handle입니다. `capture_`는 NULL일 수 있으며, non-NULL이면
전달한 각 메시지의 사본을 받는 raw socket handle입니다. 함수는 실행 중인 proxy loop가 끝날 때까지
호출 스레드를 block합니다.

세 handle은 모두 borrowed입니다. 함수는 handle을 닫거나 소유하지 않습니다. 메시지 frame은 proxy가
수신해 상대 socket으로 전달하며 application에 frame pointer를 반환하지 않습니다.

**반환값:** proxy가 정상적으로 끝나면 `ZLINK_CONFIG_OK`, 그렇지 않으면 `zlink_config_result_t` 오류.
필수 handle이 NULL이거나 raw socket이 아니면 `ZLINK_CONFIG_INVALID_HANDLE`입니다.

---

### zlink_proxy_steerable

control socket으로 실행 상태를 제어할 수 있는 양방향 proxy를 실행합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_proxy_steerable (void *frontend_,
                                                          void *backend_,
                                                          void *capture_,
                                                          void *control_);
```

`frontend_`와 `backend_`는 필수입니다. `capture_`와 `control_`은 각각 NULL일 수 있습니다. Non-NULL
`control_`은 `PAUSE`, `RESUME`, `TERMINATE`, `STATISTICS` command를 받습니다. 함수는 `TERMINATE`,
context 종료 또는 오류로 proxy loop가 끝날 때까지 호출 스레드를 block합니다.

모든 handle은 borrowed이며 함수가 닫거나 소유하지 않습니다. `STATISTICS` reply의 message ownership은
control socket의 일반 raw send/recv 계약을 따릅니다.

**반환값:** proxy가 정상적으로 끝나면 `ZLINK_CONFIG_OK`, 그렇지 않으면 `zlink_config_result_t` 오류.
필수 handle이 NULL이거나 non-NULL 선택 handle이 raw socket이 아니면
`ZLINK_CONFIG_INVALID_HANDLE`입니다.

---

### zlink_sleep

지정된 초 동안 일시 중지(sleep)합니다.

```c
ZLINK_EXPORT void zlink_sleep (int seconds_);
```

호출 스레드를 최소 `seconds_`초 동안 일시 중지합니다. 이는 플랫폼별 sleep
함수에 대한 이식 가능한 편의 wrapper입니다.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_stopwatch_start`

---

### zlink_thread_start

지정된 함수를 실행하는 새 스레드를 시작합니다.

```c
ZLINK_EXPORT void *zlink_thread_start (zlink_thread_fn *func_, void *arg_);
```

`arg_`를 유일한 인수로 사용하여 `func_`를 실행하는 새 운영 체제 스레드를
생성하고 시작합니다. 반환된 핸들은 완료를 대기하고 리소스를 해제하기 위해
`zlink_thread_join`에 전달해야 합니다.

**반환값:** 성공 시 불투명 스레드 핸들, 실패 시 `NULL`.

**스레드 안전성:** 모든 스레드에서 호출할 수 있습니다.

**참고:** `zlink_thread_join`

---

### zlink_thread_join

스레드가 완료될 때까지 대기하고 핸들을 해제합니다.

```c
ZLINK_EXPORT void zlink_thread_join (void *thread_);
```

`thread_`로 식별되는 스레드가 종료될 때까지 호출 스레드를 기다리게 한 다음 핸들을
해제합니다. 이 호출 이후 핸들을 사용해서는 안 됩니다.

**스레드 안전성:** 핸들당 정확히 한 번만 호출해야 합니다. 조인 대상 스레드에서
호출하지 마십시오.

**참고:** `zlink_thread_start`
