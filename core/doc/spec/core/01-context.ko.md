---
title: "Context"
---

[English](https://zlink-systems.github.io/zlink/spec/core/01-context/) | 한국어

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

`int` 옵션은 `zlink_ctx_set`과 `zlink_ctx_get`으로 설정하고 조회합니다. Auto HWM
byte 옵션은 `zlink_ctx_set_data`와 `zlink_ctx_get_data`를 사용합니다.

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
    /* 값 18은 할당하지 않는다. */
    ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES = 19,
    ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES = 20,
    ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES = 21
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
| 할당하지 않음 | 18 | 공개 옵션이 아니다. 이 값을 사용하면 `EINVAL`로 실패 |
| `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES` | 19 | profile 비율을 적용할 명시적 memory limit (`uint64_t`, byte). `0`은 이 입력을 설정하지 않았다는 뜻이다. `zlink_ctx_set_data()`와 `zlink_ctx_get_data()`로 설정하고 조회 |
| `ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES` | 20 | managed runtime이 전달하는 memory limit hint (`uint64_t`, byte). `0`은 감지한 hint가 없다는 뜻이다. `zlink_ctx_set_data()`와 `zlink_ctx_get_data()`로 설정하고 조회 |
| `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES` | 21 | profile 계산을 건너뛰고 그대로 사용하는 Core budget (`uint64_t`, byte). `0`은 수동 budget을 설정하지 않았다는 뜻이다. `zlink_ctx_set_data()`와 `zlink_ctx_get_data()`로 설정하고 조회 |

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
#define ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)
#define ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT ((uint64_t) 0)
#define ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT ((uint64_t) 0)
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
| `ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT` | 0 | 명시적 memory limit을 설정하지 않음 |
| `ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT` | 0 | runtime memory hint가 없음 |
| `ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT` | 0 | 수동 Core budget을 설정하지 않음 |

`SNDBUF` / `RCVBUF` 기본값은 `-1`입니다. 이 값은 zlink가 OS socket buffer
크기를 직접 정하지 않고 OS 기본값과 TCP 자동 조정에 맡긴다는 뜻입니다.
auto-HWM profile은 이 값을 자동으로 바꾸지 않습니다.

## Auto HWM memory budget 계산

Core는 다음 순서에서 처음 사용할 수 있는 입력을 선택합니다.

1. 양수 `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES`
2. 양수 `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES`
3. 양수 runtime memory hint. Core가 finite hard limit도 감지했으면 두 값의 최솟값
4. Core가 감지한 finite hard limit
5. Core가 감지한 physical memory

수동 Core budget은 profile 비율을 적용하지 않고 그대로 사용합니다. 그 밖의 memory
입력에는 선택한 profile 비율을 한 번 적용합니다.

이 budget은 application directional pipe의 정상 상태 HWM을 나누는 기준이며 context
전체의 실제 사용량을 비교하는 hard cap이 아닙니다. 각 pipe는 자신의 HWM과 그 pipe에서
Framework로 이전된 retained-credit lease만 함께 검사합니다. 한 pipe가 HWM에 도달하거나
빈 pipe oversize 예외를 사용해도 다른 pipe의 HWM을 줄이거나 admission을 중단하지 않습니다.

| Profile | 비율 | 일반 data 역할 하한 | 일반 data 역할 상한 | STREAM 하한 | STREAM 상한 |
|---|---:|---:|---:|---:|---:|
| Compact | 2% | 32 KiB | 1 MiB | 8 KiB | 32 KiB |
| LowLatency | 5% | 32 KiB | 2 MiB | 16 KiB | 64 KiB |
| Balanced | 10% | 64 KiB | 4 MiB | 64 KiB | 128 KiB |
| Throughput | 20% | 128 KiB | 16 MiB | 256 KiB | 512 KiB |

STREAM 역할만 STREAM 경계를 사용합니다. `none` 역할은 계획에서 제외하며, 그 밖의
현재 역할은 일반 data 경계를 사용합니다. 비율 계산은 overflow를 피하도록 다음 식을
사용합니다.

```text
effectiveCoreBudgetBytes =
    (resolvedMemoryLimitBytes / 100) * profilePercent
  + ((resolvedMemoryLimitBytes % 100) * profilePercent) / 100
```

Core가 finite hard limit을 감지한 경우, 그보다 큰 명시적 memory limit이나 수동 Core
budget 설정은 `EINVAL`로 실패합니다. Runtime memory hint는 설정할 수 있으며 실제
계산에서는 finite hard limit과의 최솟값을 사용합니다.

Auto HWM option setter가 성공하면 설정값을 저장하고 기본 debounce 경로로 새 계산을
예약합니다. `zlink_ctx_get_data`는 저장된 설정값을 즉시 반환하지만 budget snapshot은
마지막으로 기록한 plan을 반환하므로 재계산 전에는 이전 결과일 수 있습니다.
`zlink_ctx_auto_hwm_recalculate`를 호출하면 새 plan을 즉시 기록합니다.

ABI v1 planner는 context의 physical directional queue registry를 사용합니다. Registry는
각 application ypipe 방향을 endpoint와 독립된 stable queue ID와 generation으로 한 번만
등록하고 send·receive 역할, profile 경계, manual HWM, current applied HWM과 accounted byte를
소유합니다. 같은 inproc ypipe를 두 endpoint가 관찰해도 한 방향으로만 집계합니다.

새 pipe pair의 application 방향에는 역할별 하한을 원자적으로 예약합니다. 두 방향을 모두
예약할 수 없으면 attach를 공개하기 전에 전체 예약을 거절하며 일부 방향만 등록하지 않습니다.
수동 방향에는 유한한 수동 HWM을 예약합니다. 수동 HWM이 `0`이면 admission은 계속
무제한이지만 계산용 예약에는 역할별 상한을 사용하고 aggregate HWM이 유한하지 않다는
flag를 설정합니다.

Inproc physical ypipe는 양 endpoint의 값을 더하지 않고 다음 규칙으로 최종 cap 하나를
계산합니다. 이 cap은 registry에서 한 번만 예약하고 적용합니다.

| 송신 endpoint | 수신 endpoint | Physical ypipe 최종 cap |
|---|---|---|
| Auto | Auto | Water-filling 결과 |
| Finite manual | Auto | Finite manual cap |
| Auto | Finite manual | Finite manual cap |
| Finite manual A | Finite manual B | `min(A, B)` |
| Unlimited manual | Finite manual | Finite manual cap |
| Finite manual | Unlimited manual | Finite manual cap |
| Unlimited manual | Auto | Auto plan |
| Auto | Unlimited manual | Auto plan |
| Unlimited manual | Unlimited manual | Admission은 unlimited, 역할별 상한을 계산용 reservation으로 사용 |

수동 예약을 뺀 budget이 모든 자동 방향의 하한 합계보다 작으면 하한을 낮추지 않고
budget 부족 flag를 설정합니다. 충분하면 아직 상한에 도달하지 않은 고유 physical queue
수로 남은 budget을 나누고 각 queue를 상한까지 반복해서 증가시킵니다. 나눗셈 remainder는
stable queue ID 순서로 1 byte씩 배정합니다. 따라서 같은 registry snapshot과 입력은 항상
같은 결과를 만듭니다.

새 explicit memory limit 또는 수동 Core budget이 현재 수동 reservation과 자동 하한을
함께 수용하지 못하면 setter는 `ENOBUFS`로 실패하고 이전 설정과 plan을 유지합니다. 새
동기 inproc attach가 필요한 하한을 예약하지 못해도 `ENOBUFS`로 실패합니다. Runtime memory
hint 또는 감지한 hard limit이 실행 중 감소한 경우에는 기존 pipe와 message를 제거하지 않고
새 입력을 기록한 뒤 budget 부족 flag를 설정합니다. 새 비동기 network attach는 필요한
reservation을 얻기 전에는 publish하지 않으며 실패한 연결 시도를 종료합니다.

연결 증가로 queue별 목표가 감소하면 Core는 새 목표를 즉시 기록하고 현재 보관량이 새
목표 아래로 drain될 때까지 추가 admission을 막습니다. 연결 감소로 목표를 늘릴 때는
cooldown 뒤 같은 generation의 live queue에만 적용합니다. Detach된 queue는 outstanding
retained lease가 없을 때 제거하고, lease가 남으면 retired queue로 유지합니다.

Core는 multipart frame을 allocation 전에 provisional byte로 예약하고 commit 또는 rollback을
같은 physical queue에 반영합니다. 비어 있는 application pipe의 oversize 예외는 admission 시점에
전체 accounted 크기를 아는 complete message 한 건, 즉 single-part 또는 total-known message에만
적용합니다. 최종 전체 크기를 모르는 incremental multipart는 첫 `MORE` frame부터 일반 byte HWM을
적용하며 나중에 oversize 예외로 바꾸지 않습니다. 이 예외를 위해 known-total metadata나
transaction 전체 reservation을 추가하지 않으며, 동시에 두 건 이상의 oversize에도 적용하지
않습니다. Queue에서 Framework job으로 이동한 message는 accounted
byte를 줄이지 않고 retained-credit lease로 owner만 이전합니다. Lease release는 exact origin
queue generation의 read credit을 반환하며 다른 queue의 admission을 깨우지 않습니다.

```text
originQueueUsedBytes(queue) =
    physicalQueueAccountedBytes(queue)
  + applicationLeaseBytesFrom(queue)
```

일반 admission은 이 origin-local 합계와 그 queue의 적용 HWM만 검사합니다. Context의
`current_accounted_bytes`가 `effective_core_budget_bytes`보다 크다는 이유로 다른 queue를
함께 차단하지 않습니다.

`total_planned_hwm_bytes`는 현재 application 방향 목표 합계이고
`total_applied_hwm_bytes`는 live application 방향에 실제 적용된 HWM 합계입니다. Retired
entry는 outstanding lease와 deferred origin credit만 유지하며 applied capacity 합계와 새
water-filling 분모에는 포함되지 않습니다.
`core_queue_accounted_bytes`와 `application_accounted_bytes`는 owner만 구분하며 두 값을 더한
`current_accounted_bytes`는 owner 이전 전후에 변하지 않습니다.

DEALER·ROUTER의 completion progress lane에는 byte HWM, LWM, inproc HWM boost와
legacy 256 KiB floor를 적용하지 않으며 위 water-filling 분모에서도 제외합니다. 이
lane은 terminal reply와 error reply의 진행성만 소유합니다. Completion lane은 HWM
admission과 Core budget reservation에서 제외하지만 current·peak accounted byte와 pending
message count를 별도로 관찰합니다. 이 값은 `total_messaging_accounted_bytes`에는 포함되고
application water-filling에는 포함되지 않습니다.

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
정책으로 다시 계산합니다. 값을 `0`으로 바꾸면 현재 pipe에 마지막으로 적용한 HWM을
유지하고 이후 자동 재계산에서 제외하며 snapshot의 planning-active flag를 지웁니다.
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE`은 다음 자동 HWM 계산에서 쓰는 profile을
바꾸며, runtime 중에도 안전하게 조정할 수 있습니다. Profile은 memory 비율과
역할별 byte 하한·상한을 선택합니다. `SNDBUF` / `RCVBUF` 기본값은 `-1`이며,
auto-HWM profile은 이 값을 자동으로 바꾸지 않습니다. 세 Auto HWM byte 옵션은
`zlink_ctx_set`으로 설정할 수 없고 `EINVAL`로 실패합니다.

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

세 Auto HWM byte 옵션은 정확히 `sizeof(uint64_t)` byte를 받습니다. 값 `0`은
unlimited가 아니라 해당 입력을 설정하지 않았다는 뜻입니다. 다른 크기와 제거된
context 옵션 값 `18`은 `ZLINK_CONFIG_INVALID_ARGUMENT`로 실패합니다.

`ZLINK_THREAD_NAME_PREFIX`에는 null 종료 문자열을 `optval_`로 전달하고
`strlen(prefix) + 1`을 `optvallen_`으로 전달합니다. 접두사는 플랫폼 스레드
이름 제한에 맞춰 최대 16바이트(`optvallen_ <= 16`)로 제한됩니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EINVAL` -- 알 수 없는 옵션 또는 유효하지 않은 값.
- `ENOBUFS` -- 새 explicit memory limit 또는 수동 Core budget이 현재 수동 reservation과 자동 하한을 함께 수용하지 못함.
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

세 Auto HWM byte 옵션에는 `uint64_t` output buffer가 필요하고, 호출할 때
`*optvallen_`이 정확히 `sizeof(uint64_t)`여야 합니다. 더 큰 임시 buffer나 4-byte
크기를 포함해 그 밖의 크기는 값을 잘라 쓰거나 일부만 채우지 않고
`ZLINK_CONFIG_INVALID_ARGUMENT`와 `errno == EINVAL`로 실패합니다. 이때 필요한
크기인 `sizeof(uint64_t)`를 `*optvallen_`에 기록합니다. 성공해도 같은 크기를
유지합니다.

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
꺼 둔 경우도 그대로 유지됩니다. Auto HWM profile이나 memory budget 입력을
바꾼 뒤 일반 refresh 경로를 기다리지 않고 새 계획을 기록할 때 사용합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값.
`zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:**
- `EFAULT` -- 유효하지 않은 context 핸들.

**스레드 안전성:** 모든 스레드에서 안전하게 호출할 수 있습니다.

**참고:** `zlink_ctx_set`, `zlink_monitor_status`

---

### zlink_ctx_get_auto_hwm_budget_snapshot

마지막으로 기록한 context-wide Auto HWM 계획을 versioned 구조체로 조회합니다.

```c
#define ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1 1u

#define ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE       (1u << 0)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT          (1u << 1)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_HWM_VALID   (1u << 2)
#define ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW    (1u << 3)

typedef struct zlink_auto_hwm_budget_snapshot_t {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t budget_generation;
  uint64_t measurement_epoch;
  uint64_t configured_memory_limit_bytes;
  uint64_t runtime_memory_limit_bytes;
  uint64_t resolved_memory_limit_bytes;
  uint64_t configured_core_budget_bytes;
  uint64_t effective_core_budget_bytes;
  uint64_t total_planned_hwm_bytes;
  uint64_t total_applied_hwm_bytes;
  uint64_t manual_reserved_hwm_bytes;
  uint64_t core_queue_accounted_bytes;
  uint64_t application_accounted_bytes;
  uint64_t current_accounted_bytes;
  uint64_t provisional_accounted_bytes;
  uint64_t peak_accounted_bytes;
  uint64_t completion_current_accounted_bytes;
  uint64_t completion_peak_accounted_bytes;
  uint64_t completion_pending_message_count;
  uint64_t total_messaging_accounted_bytes;
  uint64_t monitor_queue_applied_hwm_bytes;
  uint64_t monitor_queue_accounted_bytes;
  uint64_t total_instance_applied_hwm_bytes;
  uint64_t total_instance_accounted_bytes;
  uint64_t oversize_admission_count;
  uint64_t largest_oversize_message_bytes;
  uint64_t active_directional_queue_count;
  uint64_t active_completion_directional_queue_count;
  uint64_t active_send_queue_count;
  uint64_t active_receive_queue_count;
  uint64_t outstanding_application_lease_count;
  uint64_t retired_queue_count;
  uint64_t deferred_origin_credit_bytes;
  uint64_t unlimited_manual_queue_count;
  uint32_t blocked_ratio_ppm;
  uint32_t flags;
  uint64_t reserved_u64[8];
} zlink_auto_hwm_budget_snapshot_t;

ZLINK_EXPORT zlink_config_result_t
zlink_ctx_get_auto_hwm_budget_snapshot(
  void *context_,
  zlink_auto_hwm_budget_snapshot_t *snapshot_);
```

호출자는 구조체를 0으로 초기화하고 `abi_version`을
`ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1`, `struct_size`를 자신이 할당한 byte
크기로 설정합니다. Null snapshot이나 header 두 field보다 짧은 크기는 `EINVAL`,
지원하지 않는 version은 `ENOTSUP`, 유효하지 않은 context는 `EFAULT`, 종료 중인
context는 `ETERM`으로 실패합니다. 성공하면 caller 크기와 Core v1 크기 중 작은 prefix만 기록합니다.
반환된 `struct_size`는 Core v1 구조체의 전체 크기입니다.

V1에서 값을 제공하는 field는 다음과 같습니다.

- `abi_version`, `struct_size`
- `budget_generation`, `measurement_epoch`
- configured/runtime/resolved memory limit과 configured/effective Core budget
- `total_planned_hwm_bytes`, `total_applied_hwm_bytes`, `manual_reserved_hwm_bytes`
- `core_queue_accounted_bytes`, `application_accounted_bytes`,
  `current_accounted_bytes`, `provisional_accounted_bytes`, `peak_accounted_bytes`
- `completion_current_accounted_bytes`, `completion_peak_accounted_bytes`,
  `completion_pending_message_count`, `total_messaging_accounted_bytes`
- `monitor_queue_applied_hwm_bytes`, `monitor_queue_accounted_bytes`,
  `total_instance_applied_hwm_bytes`, `total_instance_accounted_bytes`
- `oversize_admission_count`, `largest_oversize_message_bytes`
- `active_directional_queue_count`,
  `active_completion_directional_queue_count`, `active_send_queue_count`,
  `active_receive_queue_count`, `outstanding_application_lease_count`,
  `retired_queue_count`, `deferred_origin_credit_bytes`,
  `unlimited_manual_queue_count`
- `blocked_ratio_ppm`
- 네 `ZLINK_AUTO_HWM_BUDGET_FLAG_*` bit

`reserved_u64` 원소는 모두 0입니다.

| Flag | 설정 조건 |
|---|---|
| `ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE` | 마지막으로 기록한 plan에서 Auto HWM이 활성화됨 |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT` | 수동 예약과 필요한 자동 하한을 budget 안에 함께 배정할 수 없음 |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_HWM_VALID` | manual unlimited 방향이 없어 HWM 합계를 유한값으로 해석할 수 있음 |
| `ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW` | planner 또는 queue 합산이 `uint64_t` 범위를 넘어 포화됨 |

`active_directional_queue_count`와
`active_completion_directional_queue_count`는 reader와 writer endpoint가 공유하는
고유 physical ypipe 방향을 한 번만 집계합니다. Completion 방향은 application
방향의 planning 분모와 HWM admission에서 제외됩니다. `active_send_queue_count`와
`active_receive_queue_count`는 마지막 socket plan의 관점별 count입니다. 새 context의 `budget_generation`은 0,
`measurement_epoch`은 1입니다. `budget_generation`은 새 계획을 기록할 때 증가하고
`measurement_epoch`은 metrics reset 때 증가합니다.

Physical queue accounting은 payload와 각 frame의 `sizeof(zlink_msg_t)`를 합산합니다.
끝나지 않은 multipart frame은 `provisional_accounted_bytes`에 포함되며 final frame에서
같은 byte를 중복 증가시키지 않고 committed 상태로 전환합니다. Read, rollback,
hiccup, termination과 conflate replacement는 실제 제거한 frame의 charge를 반환합니다.
Completion 방향은 별도 current·peak·complete-message pending count로 보고합니다.

`monitor_queue_applied_hwm_bytes`는 열려 있는 monitor의 고유한 physical ypipe
방향마다 현재 적용된 HWM을 한 번씩 합산합니다. Reader와 writer endpoint에 복사된
option을 각각 더하지 않습니다. `monitor_queue_accounted_bytes`는 같은 monitor
방향들이 현재 보유한 frame의 accounted byte를 합산합니다. 두 값은 application
planning, `total_applied_hwm_bytes`, `current_accounted_bytes`와
`total_messaging_accounted_bytes`에 포함하지 않고 다음 instance 합계에서만 더합니다.

```text
total_instance_applied_hwm_bytes =
    total_applied_hwm_bytes + monitor_queue_applied_hwm_bytes

total_instance_accounted_bytes =
    total_messaging_accounted_bytes + monitor_queue_accounted_bytes
```

Completion queue에는 HWM이 없으므로 `total_instance_applied_hwm_bytes`에 더하지
않습니다. 위 합계가 `uint64_t` 범위를 넘으면 `UINT64_MAX`로 포화하고
`ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW`를 설정합니다.

Retained-credit receive가 physical frame을 반환하면 해당 charge는 원자적으로
`core_queue_accounted_bytes`에서 `application_accounted_bytes`로 이동합니다.
`current_accounted_bytes`는 두 field의 포화 합이며 owner 이전만으로 변하지 않습니다.
`peak_accounted_bytes`도 queue와 application lease의 합을 기준으로 합니다.
`outstanding_application_lease_count`는 아직 release되지 않은 public lease 수이고,
`deferred_origin_credit_bytes`는 internal framing token 또는 public lease가 보유해 writer에
아직 게시하지 않은 exact-origin byte입니다. `retired_queue_count`는 detach 또는 generation
교체 뒤에도 retained origin 때문에 유지되는 directional queue generation 수입니다. Old
generation release는 새 generation의 accounting, credit과 wake에 합쳐지지 않으며 마지막
origin release 뒤 retired 항목이 제거됩니다.

Snapshot은 한 `budget_generation`에 속한 일관된 registry view입니다. Queue count, capacity와
accounted counter를 서로 다른 generation에서 섞지 않습니다. Current counter가 snapshot을
만드는 동안 변할 수 있더라도 반환된 합계와 구성 field는 같은 snapshot 경계에서 서로
일치해야 합니다.

`blocked_ratio_ppm`은 다음 식으로 계산합니다.

```text
floor(first_blocked_admission_attempts * 1,000,000 / total_admission_attempts)
```

`total_admission_attempts`가 0이면 비율도 0입니다.
같은 submit의 wake 뒤 재시도는 다시 세지 않습니다. 대상 application pipe의 HWM 때문에
처음 block된 시도만 분자에 넣고 transport I/O wait와 context aggregate 사용량은 제외합니다.

---

### zlink_ctx_reset_auto_hwm_budget_metrics

현재 Auto HWM measurement epoch을 변경합니다.

```c
ZLINK_EXPORT zlink_config_result_t
zlink_ctx_reset_auto_hwm_budget_metrics(void *context_);
```

성공하면 `measurement_epoch`을 1 증가시키고 application·completion peak를 현재
accounted byte로 다시 기준화하며 blocked·전체 admission attempt counter,
`blocked_ratio_ppm`, oversize 누적 count와 최대값을 0으로 초기화합니다.
현재 byte, monitor queue의 HWM·accounted byte, budget, plan, queue count와
`budget_generation`은 바꾸지 않습니다.
유효하지 않은 context는 `EFAULT`, 종료 중인 context는 `ETERM`으로 실패합니다.
