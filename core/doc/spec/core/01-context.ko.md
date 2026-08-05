---
title: "Context"
---

[English](01-context.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: 공개 계약 관리](00-public-contract-governance.ko.md) | [다음: Message](02-message.ko.md)
<!-- zlink-nav:end -->

# Context

> **이 장이 정의하는 것** — Context 생성·종료와 옵션 설정의 공개 계약.

이 문서는 ZLink Core의 Context lifecycle과 option 공개 계약을 정의한다. 대상 독자는 Context
생성, 설정과 종료를 C API와 bindings에 투영하는 개발자다. 이 문서는 “I/O thread와 socket의 최상위
container인 Context를 어떻게 생성하고 설정하며 안전하게 종료하는가?”에 답한다.

모든 application은 다른 zlink API를 사용하기 전에 최소한 하나의 Context를 생성해야 한다. Context는
thread-safe하며 여러 thread가 공유할 수 있다.

## Context 옵션 상수

옵션은 `zlink_ctx_set`과 `zlink_ctx_get`으로 설정하고 조회합니다.

```c
typedef enum zlink_ctx_option_t
{
    ZLINK_IO_THREADS              = 1,
    ZLINK_MAX_SOCKETS             = 2,
    ZLINK_SOCKET_LIMIT            = 3,
    ZLINK_THREAD_PRIORITY         = 3,
    ZLINK_THREAD_SCHED_POLICY     = 4,
    ZLINK_MAX_MSGSZ               = 5,
    ZLINK_MSG_T_SIZE              = 6,
    ZLINK_THREAD_AFFINITY_CPU_ADD      = 7,
    ZLINK_THREAD_AFFINITY_CPU_REMOVE   = 8,
    ZLINK_THREAD_NAME_PREFIX      = 9,
    ZLINK_CTX_OPT_BLOCKY          = 10,
    ZLINK_CTX_OPT_AUTO_HWM_ENABLE = 12,
    ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS = 14,
    ZLINK_CTX_OPT_AUTO_HWM_PROFILE = 17,
    ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES = 18
} zlink_ctx_option_t;
```

```c
typedef enum zlink_auto_hwm_profile_t
{
    ZLINK_AUTO_HWM_PROFILE_COMPACT = 0,
    ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY = 1,
    ZLINK_AUTO_HWM_PROFILE_BALANCED = 2,
    ZLINK_AUTO_HWM_PROFILE_THROUGHPUT = 3
} zlink_auto_hwm_profile_t;
```

| 상수 | 값 | 설명 |
|------|-----|------|
| `ZLINK_IO_THREADS` | 1 | Context의 I/O 스레드 수 |
| `ZLINK_MAX_SOCKETS` | 2 | 허용되는 최대 소켓 수 |
| `ZLINK_SOCKET_LIMIT` | 3 | 소켓 수의 하드 상한 (읽기 전용) |
| `ZLINK_THREAD_PRIORITY` | 3 | I/O 스레드 스케줄링 우선순위 |
| `ZLINK_THREAD_SCHED_POLICY` | 4 | I/O 스레드 스케줄링 정책 |
| `ZLINK_MAX_MSGSZ` | 5 | 최대 메시지 크기 (바이트 단위, `>= 0`, 기본값 `INT_MAX`) |
| `ZLINK_MSG_T_SIZE` | 6 | `zlink_msg_t`의 크기 (바이트 단위, 읽기 전용) |
| `ZLINK_THREAD_AFFINITY_CPU_ADD` | 7 | I/O 스레드 어피니티 집합에 CPU 추가 |
| `ZLINK_THREAD_AFFINITY_CPU_REMOVE` | 8 | I/O 스레드 어피니티 집합에서 CPU 제거 |
| `ZLINK_THREAD_NAME_PREFIX` | 9 | I/O 스레드 이름 접두사 |
| `ZLINK_CTX_OPT_BLOCKY` | 10 | context 종료 시 블로킹 동작을 제어 (`int`, 기본값 1) |
| `ZLINK_CTX_OPT_AUTO_HWM_ENABLE` | 12 | 자동 HWM(High-Water Mark) 정책 사용 여부 (`0` = 비활성, `1` = 활성) |
| `ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS` | 14 | 연결 변화가 이어질 때 자동 HWM 재계산을 다시 실행하기 전에 기다리는 최소 debounce 시간 (ms, `>= 0`) |
| `ZLINK_CTX_OPT_AUTO_HWM_PROFILE` | 17 | 자동 HWM profile (`ZLINK_AUTO_HWM_PROFILE_*`). 알 수 없는 값은 `EINVAL`로 실패 |
| `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` | 18 | 자동 byte HWM을 계산할 때 쓰는 context 수준 planning unit (`uint64_t`, byte 단위). `0`은 소켓 타입 기본값 사용. `zlink_ctx_set_data()`와 `zlink_ctx_get_data()`로 설정하고 조회 |

> **참고:** `ZLINK_SOCKET_LIMIT`과 `ZLINK_THREAD_PRIORITY`는 enum 값 `3`을
> 공유합니다. 현재 공개 C ABI의 옵션 조회는 값 `3`을 읽기 전용
> `ZLINK_SOCKET_LIMIT`으로 먼저 해석하므로, `ZLINK_THREAD_PRIORITY`는
> `zlink_ctx_set` / `zlink_ctx_get`으로 설정하거나 조회할 수 없습니다.

## 기본값

```c
#define ZLINK_IO_THREADS_DFLT           4
#define ZLINK_MAX_SOCKETS_DFLT          4095
#define ZLINK_THREAD_PRIORITY_DFLT      -1
#define ZLINK_THREAD_SCHED_POLICY_DFLT  -1
#define ZLINK_CTX_AUTO_HWM_ENABLE_DFLT  1
#define ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT 3000
#define ZLINK_CTX_AUTO_HWM_PROFILE_DFLT ZLINK_AUTO_HWM_PROFILE_BALANCED
#define ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT ((uint64_t) 0)
```

| 상수 | 값 | 설명 |
|------|-----|------|
| `ZLINK_IO_THREADS_DFLT` | 4 | 기본 I/O 스레드 수 |
| `ZLINK_MAX_SOCKETS_DFLT` | 4095 | 기본 최대 소켓 수 |
| `ZLINK_THREAD_PRIORITY_DFLT` | -1 | 기본 스레드 우선순위 (OS 기본값) |
| `ZLINK_THREAD_SCHED_POLICY_DFLT` | -1 | 기본 스케줄링 정책 (OS 기본값) |
| `ZLINK_CTX_AUTO_HWM_ENABLE_DFLT` | 1 | 자동 HWM 정책이 기본으로 활성화되어 있음. 애플리케이션이 auto-HWM을 끄거나 수동 HWM을 설정하지 않으면 balanced profile을 사용함 |
| `ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT` | 3000 | 자동 HWM 재계산 기본 debounce 시간 (ms) |
| `ZLINK_CTX_AUTO_HWM_PROFILE_DFLT` | `ZLINK_AUTO_HWM_PROFILE_BALANCED` | 자동 HWM 기본 profile |
| `ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT` | 0 | 각 소켓 타입의 기본 메시지 단위를 사용. STREAM은 `1024` bytes, 그 외 소켓은 `4096` bytes |

`SNDBUF` / `RCVBUF` 기본값은 `-1`입니다. 이 값은 zlink가 OS socket buffer
크기를 직접 정하지 않고 OS 기본값과 TCP 자동 조정에 맡긴다는 뜻입니다.
auto-HWM profile은 이 값을 자동으로 바꾸지 않습니다.

## 함수

### zlink_ctx_new

새 zlink context를 생성합니다.

```c
ZLINK_EXPORT void *zlink_ctx_new(void);
```

기본 옵션 값으로 새 context를 할당하고 초기화합니다. Context는 I/O 스레드 풀을
관리하며 소켓 생성의 기반이 됩니다. 모든 소켓은 context와 연결되어야 합니다.
Context가 더 이상 필요하지 않으면 `zlink_ctx_term`으로 해제합니다.

**반환값:** 성공 시 context 핸들, 실패 시 `NULL` (errno가 설정됨).

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다. 반환된 context
핸들은 스레드 간에 공유할 수 있습니다.

**참고:** `zlink_ctx_term`, `zlink_ctx_set`

---

### zlink_ctx_term

Context를 종료하고 관련된 모든 리소스를 해제합니다.

```c
ZLINK_EXPORT zlink_close_result_t zlink_ctx_term(void *context_);
```

Context를 파괴합니다. 이 호출은 context 내에서 생성된 모든 소켓이 닫힐 때까지
블로킹될 수 있습니다. Context에 속한 소켓의 블로킹 작업은 `zlink_ctx_shutdown`이
호출되거나 모든 소켓이 닫힌 후 `ETERM`을 반환합니다. 각 context는 정확히
한 번만 종료해야 합니다.

**반환값:** 성공 시 `ZLINK_CLOSE_OK`, 실패 시 `zlink_close_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EFAULT` -- 유효하지 않은 context 핸들.
- `EINTR` -- 시그널에 의해 종료가 중단됨; 재시도할 수 있습니다.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있지만, context당 정확히
한 번만 호출해야 합니다. 이 호출이 반환된 후에는 context 핸들을 사용하지
마세요.

**참고:** `zlink_ctx_new`, `zlink_ctx_shutdown`

---

### zlink_ctx_shutdown

Context를 즉시 종료합니다.

```c
ZLINK_EXPORT zlink_close_result_t zlink_ctx_shutdown(void *context_);
```

이 context에 속한 소켓의 모든 블로킹 작업이 `ETERM`과 함께 즉시 반환되도록
시그널을 보냅니다. 이것은 종료를 시작하지만 리소스를 해제하지 않는 논블로킹
호출입니다. 최종 정리를 위해 이후에 `zlink_ctx_term`을 호출해야 합니다.
term 전에 shutdown을 호출하면 여러 스레드에서 소켓을 사용할 때 데드락을
방지할 수 있습니다.

**반환값:** 성공 시 `ZLINK_CLOSE_OK`, 실패 시 `zlink_close_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EFAULT` -- 유효하지 않은 context 핸들.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_term`

---

### zlink_ctx_set

Context 옵션을 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set(void *context_, zlink_ctx_option_t option_, int optval_);
```

소켓이 생성되기 전 또는 후에 context를 구성합니다. 유효한 옵션 이름과 의미는 위의 옵션 상수
테이블을 참조하세요. `ZLINK_CTX_OPT_AUTO_HWM_ENABLE`은 이미 만들어진 소켓에도
즉시 반영되며, 아직 수동 `SNDHWM` / `RCVHWM` 값을 주지 않은 소켓만 자동
정책으로 다시 계산합니다.
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE`은 다음 자동 HWM 계산에서 쓰는 profile을
바꾸며, runtime 중에도 안전하게 조정할 수 있습니다. profile은 자동 HWM
planner가 쓰는 연결당 단위 예산과 size cap을 고릅니다. `SNDBUF` / `RCVBUF`
기본값은 `-1`이며, auto-HWM profile은 이 값을 자동으로 바꾸지 않습니다.
`ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 명시적인 소켓별 override가 없는
소켓의 자동 HWM 계산 메시지 단위를 바꿉니다. 값 `0`은 해당 소켓을 소켓 타입별
기본 메시지 단위로 되돌립니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EINVAL` -- 알 수 없는 옵션 또는 유효하지 않은 값.
- `EFAULT` -- 유효하지 않은 context 핸들 (`ZLINK_CONFIG_INVALID_HANDLE`).

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set_data`, `zlink_ctx_get`

---

### zlink_ctx_set_data

byte 버퍼로 context 옵션을 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set_data(void *context_,
                                         zlink_ctx_option_t option_,
                                         const void *optval_,
                                         size_t optvallen_);
```

공개 바인딩 타입이 `int`가 아닌 context 옵션에 사용합니다.
`ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 정확히 `sizeof(uint64_t)` byte를
받습니다. 이 값은 관찰한 평균 message 크기가 아니라 byte HWM을 계산하는
planning input입니다. `0`은 소켓 타입 기본값을 선택합니다. 이전 4-byte 값은
`ZLINK_CONFIG_INVALID_ARGUMENT`로 실패합니다.

`ZLINK_THREAD_NAME_PREFIX`에는 null 종료 문자열을 `optval_`로 전달하고
`strlen(prefix) + 1`을 `optvallen_`으로 전달합니다. 접두사는 플랫폼 스레드
이름 제한에 맞춰 최대 16바이트(`optvallen_ <= 16`)로 제한됩니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EINVAL` -- 알 수 없는 옵션 또는 유효하지 않은 값.
- `EFAULT` -- 유효하지 않은 context 핸들 (`ZLINK_CONFIG_INVALID_HANDLE`).

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set`, `zlink_ctx_get_data`, `zlink_ctx_get`

---

### zlink_ctx_get_data

호출자가 제공한 저장 공간으로 context 옵션을 조회합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_get_data(void *context_,
                                         zlink_ctx_option_t option_,
                                         void *optval_,
                                         size_t *optvallen_);
```

`ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`에는 `uint64_t` output buffer가
필요하고, 호출할 때 `*optvallen_`이 정확히 `sizeof(uint64_t)`여야 합니다. 더 큰
임시 buffer나 이전 4-byte 크기를 포함해 그 밖의 크기는 값을 잘라 쓰거나 일부만
채우지 않고 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `errno == EINVAL`로 실패합니다.
성공하면 `*optvallen_`은 `sizeof(uint64_t)`를 유지합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값.
`zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EINVAL` -- 알 수 없는 option 또는 잘못된 output 크기.
- `EFAULT` -- 유효하지 않은 context handle 또는 output pointer.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set_data`, `zlink_ctx_get`

---

### zlink_ctx_get

Context 옵션을 조회합니다.

```c
ZLINK_EXPORT int zlink_ctx_get(void *context_, zlink_ctx_option_t option_, zlink_config_result_t *error_out_);
```

Context 옵션의 현재 값을 가져옵니다. `ZLINK_SOCKET_LIMIT` 및 `ZLINK_MSG_T_SIZE`
같은 읽기 전용 옵션을 포함하여 언제든지 context 구성을 검사하는 데 사용할 수
있습니다. 실패 시 `*error_out_`에 설정 결과(`zlink_config_result_t`)가
기록되고, 성공 시 옵션 값이 기본 반환값으로 반환됩니다.

**반환값:** 성공 시 옵션 값, 실패 시 `-1`이며 `*error_out_`에
`zlink_config_result_t`가 기록됩니다. `zlink_errno()`는 진단용 내부 errno를
그대로 유지합니다.

**에러:**
- `EINVAL` -- 알 수 없는 옵션.
- `EFAULT` -- 유효하지 않은 context 핸들; `*error_out_`에 `ZLINK_CONFIG_INVALID_HANDLE`이 기록됩니다.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set`, `zlink_ctx_get_data`

---

### zlink_ctx_auto_hwm_recalculate

현재 context 전체에 자동 HWM 계획을 즉시 다시 적용합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_ctx_auto_hwm_recalculate(void *context_);
```

이 함수는 아직 자동 queue/buffer 정책을 따르는 소켓에 대해 즉시 자동 HWM
재계산을 실행합니다. 사용자가 수동으로 바꾼 값은 그대로 유지되고, 자동 HWM을
꺼 둔 경우도 그대로 유지됩니다. 자동 HWM profile이나 소켓의 message unit
옵션을 바꾼 뒤 새 per-connection sizing을 일반 refresh 경로를 기다리지 않고
바로 적용하고 싶을 때 사용합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값.
`zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EFAULT` -- 유효하지 않은 context 핸들.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set`, `zlink_monitor_status`
