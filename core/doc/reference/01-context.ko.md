
[레퍼런스 목차](README.ko.md)

# 01. Context

이 category는 `zlink_ctx_*` handle이 제공하는 진입점 — 생성, 종료, 옵션 구성, automatic-HWM
재계산 트리거를 다룬다. 정확한 signature는 [Context 스펙](../spec/core/01-context.ko.md)이
소유한다.

---

## `zlink_ctx_new`

새 context를 만든다. 이 레퍼런스의 다른 모든 항목의 전제 조건이다 — 모든 socket은 context에
속한다.

```c
void *ctx = zlink_ctx_new();
```

**Parameters.** 없음.

**Return과 errno.** 성공하면 context handle을, 실패하면 `NULL`을 반환하며 `errno`가
설정된다. 옵션 값은 기본값으로 시작한다(`ZLINK_IO_THREADS_DFLT` = 4,
`ZLINK_MAX_SOCKETS_DFLT` = 4095 등 — Context 스펙의 기본값 표 참고).

**선택 기준.** 프로세스가 필요로 하는 context마다 한 번 호출한다. Context는 I/O thread pool을
관리하며 여러 스레드에서 공유할 수 있다 — 대부분의 application은 정확히 하나만 필요하다.

---

## `zlink_ctx_shutdown` / `zlink_ctx_term`

진행 중인 blocking 호출에 해제 신호를 보낸 뒤, context를 파괴하고 자원을 해제한다.

```c
zlink_ctx_shutdown(ctx);   // non-blocking: 대기 중인 호출을 ETERM으로 해제한다
zlink_ctx_term(ctx);       // context 안의 모든 socket이 닫힐 때까지 block한다
```

**Parameters.** 둘 다 context handle만 받는다.

**Return과 errno.** 둘 다 `zlink_close_result_t`를 반환한다 — 성공하면
`ZLINK_CLOSE_OK`. `term`은 `EFAULT`(잘못된 handle) 또는 `EINTR`(signal에 의한 중단 — 재시도
가능)로 실패한다. `shutdown`은 `EFAULT`로만 실패한다. `term`이 반환된 뒤에는 이 handle을 다시
쓰면 안 된다.

**선택 기준.** 여러 스레드에서 socket을 쓰고 있다면 스레드가 socket 호출에 영원히 block되는
것을 피하려고 `shutdown`을 먼저 호출한다 — context의 socket에 대한 모든 blocking 호출을 즉시
`ETERM`으로 반환시킨다. `term`은 context마다 정확히 한 번, 항상 호출해 자원을 해제한다 —
context가 소유한 모든 socket이 닫힐 때까지 block할 수 있다.

---

## `zlink_ctx_set` / `zlink_ctx_get`

공개 타입이 `int`인 context 옵션을 설정하거나 읽는다.

```c
zlink_ctx_set(ctx, ZLINK_IO_THREADS, 8);

zlink_config_result_t err;
int threads = zlink_ctx_get(ctx, ZLINK_IO_THREADS, &err);
```

**Parameters.** `option_`은 `zlink_ctx_option_t` 값 중 하나다(`ZLINK_IO_THREADS`,
`ZLINK_MAX_SOCKETS`, `ZLINK_THREAD_PRIORITY`, `ZLINK_THREAD_SCHED_POLICY`, `ZLINK_MAX_MSGSZ`,
`ZLINK_THREAD_AFFINITY_CPU_ADD`/`_REMOVE`, `ZLINK_CTX_OPT_BLOCKY`,
`ZLINK_CTX_OPT_AUTO_HWM_ENABLE`, `ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS`,
`ZLINK_CTX_OPT_AUTO_HWM_PROFILE` — 각각의 의미와 기본값은 Context 스펙의 옵션 표 참고).
`zlink_ctx_get`은 추가로 `ZLINK_SOCKET_LIMIT`, `ZLINK_MSG_T_SIZE` 같은 읽기 전용 옵션도
반환한다.

**Return과 errno.** `zlink_ctx_set`은 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`, 알 수 없는 옵션이거나 범위를 벗어난 값이면 `EINVAL`, 잘못된 context면
`EFAULT`(`ZLINK_CONFIG_INVALID_HANDLE`). `zlink_ctx_get`은 성공하면 옵션 값을 직접 반환하고,
실패하면 `-1`을 반환하며 `zlink_config_result_t`를 `error_out_`에 써 준다.

**선택 기준.** 위 `int` 타입 옵션에 쓴다. `ZLINK_CTX_OPT_AUTO_HWM_PROFILE`과
`ZLINK_CTX_OPT_AUTO_HWM_ENABLE`은 실행 중인 context에서 바꿔도 안전하다 — profile 변경은 다음
automatic HWM 재계산에 적용되고, enable 토글은 여전히 automatic HWM을 쓰는 socket에 즉시
적용된다. `ZLINK_SOCKET_LIMIT`과 `ZLINK_THREAD_PRIORITY`는 enum 값 `3`을 공유한다 — lookup은
읽기 전용 `ZLINK_SOCKET_LIMIT`으로 확정되므로, 이 쌍으로는 `ZLINK_THREAD_PRIORITY`를 실제로
설정·조회할 수 없다.

---

## `zlink_ctx_set_data` / `zlink_ctx_get_data`

공개 타입이 단순 `int`가 아닌(byte buffer나 문자열) context 옵션을 설정하거나 읽는다.

```c
uint64_t unit_bytes = 2048;
zlink_ctx_set_data(ctx, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, &unit_bytes, sizeof(unit_bytes));

const char *prefix = "app-io";
zlink_ctx_set_data(ctx, ZLINK_THREAD_NAME_PREFIX, prefix, strlen(prefix) + 1);
```

**Parameters.** `option_`은 `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`(정확히 `sizeof(uint64_t)`
바이트가 필요하다 — `0`이면 socket 타입의 기본 unit을 쓴다) 또는 `ZLINK_THREAD_NAME_PREFIX`(널
종료 문자열 — `optvallen_`은 terminator를 포함하며, 플랫폼 thread-name 상한 때문에 16바이트로
제한된다) 중 하나다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`,
알 수 없는 옵션이거나 잘못된 값이면 `EINVAL`, (HWM unit 옵션의 경우) 정확히
`sizeof(uint64_t)`가 아닌 크기면 — legacy 4바이트 값을 포함해 재해석하지 않고 거부한다.
잘못된 context면 `EFAULT`(`ZLINK_CONFIG_INVALID_HANDLE`).

**선택 기준.** 위 두 옵션에만 쓴다 — 나머지 옵션은 전부 `zlink_ctx_set`/`zlink_ctx_get`을
거친다. `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 automatic HWM planner의 계획 입력값이지
관측된 평균 메시지 크기가 아니다.

---

## `zlink_ctx_auto_hwm_recalculate`

Context 안에서 아직 automatic 정책을 쓰는 모든 socket에 즉시 automatic-HWM 갱신을 강제한다.

```c
zlink_ctx_auto_hwm_recalculate(ctx);
```

**Parameters.** Context handle만 받는다.

**Return과 errno.** `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`, 잘못된
context handle이면 `EFAULT`.

**선택 기준.** Automatic HWM profile이나 message-unit 옵션을 바꾼 뒤, 일반 갱신 경로를
기다리지 않고 새 connection별 크기 조정을 즉시 적용하려고 호출한다. Manual HWM override가
있거나 automatic HWM이 비활성화된 socket에는 영향을 주지 않는다.

---

## `zlink_ctx_get_auto_hwm_budget_snapshot` / `zlink_ctx_reset_auto_hwm_budget_metrics`

Context 전체의 Auto HWM budget 계획과 counter를 versioned snapshot으로 읽거나, 계획은 건드리지
않고 새 측정 구간을 위해 counter만 초기화한다.

```c
zlink_auto_hwm_budget_snapshot_t snap = {0};
snap.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
snap.struct_size = sizeof(snap);
zlink_ctx_get_auto_hwm_budget_snapshot(ctx, &snap);

zlink_ctx_reset_auto_hwm_budget_metrics(ctx);
```

**Parameters.** `zlink_ctx_get_auto_hwm_budget_snapshot`은 context handle과 출력용
`snapshot_` pointer를 받는다 — 호출 전 caller가 구조체를 0으로 초기화하고 `abi_version`을
`ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1`로, `struct_size`를 자신이 할당한 크기로 설정한다.
Core는 caller 크기와 Core v1 크기 중 작은 prefix만 기록하고, `struct_size`에는 Core v1 구조체의
전체 크기를 반환한다. `zlink_ctx_reset_auto_hwm_budget_metrics`는 context handle만 받는다.
Snapshot 구조체는 budget 계획(`configured_memory_limit_bytes`, `runtime_memory_limit_bytes`,
`resolved_memory_limit_bytes`, `configured_core_budget_bytes`, `effective_core_budget_bytes`,
`total_planned_hwm_bytes`, `total_applied_hwm_bytes`, `manual_reserved_hwm_bytes`), accounted-byte
counter(`core_queue_accounted_bytes`, `current_accounted_bytes`, `provisional_accounted_bytes`,
`peak_accounted_bytes`, `completion_*` 계열과 `total_messaging_accounted_bytes`,
`monitor_queue_*`·`total_instance_*` 계열), admission counter(`oversize_admission_count`,
`largest_oversize_message_bytes`, `blocked_ratio_ppm`), queue count
(`active_directional_queue_count`, `active_completion_directional_queue_count`,
`active_send_queue_count`, `active_receive_queue_count`, `unlimited_manual_queue_count`),
generation 표식(`budget_generation`, `measurement_epoch`), `flags` bitfield
(`ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE`/`_INSUFFICIENT`/`_AGGREGATE_HWM_VALID`/
`_AGGREGATE_OVERFLOW`), 그리고 ABI 호환을 위해 남긴 예약 필드
(`application_accounted_bytes`, `outstanding_application_lease_count`, `retired_queue_count`,
`deferred_origin_credit_bytes`, `reserved_u64[8]` — 항상 0)를 담는다. 각 field의 정확한 의미는
[Auto HWM 스펙](../spec/core/systems/06-auto-hwm.ko.md#3-함수)의 field 표를 참고한다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면 `ZLINK_CONFIG_OK`.
`zlink_ctx_get_auto_hwm_budget_snapshot`은 `EINVAL`(null snapshot pointer이거나 `struct_size`가
header 두 field보다 짧음), `ENOTSUP`(지원하지 않는 `abi_version`), `EFAULT`(잘못된 context),
`ETERM`(종료 중인 context)으로 실패한다. `zlink_ctx_reset_auto_hwm_budget_metrics`는 `EFAULT`
또는 `ETERM`으로만 실패한다.

**선택 기준.** Dashboard, health check, 또는 budget 부족 상태를 진단할 때 현재 계획과 회계를
관찰하려고 `zlink_ctx_get_auto_hwm_budget_snapshot`을 호출한다 — 호출 자체는 admission이나
rejection 결과를 바꾸지 않는다. 새 측정 구간을 시작할 때(예: 테스트 케이스 사이, 또는 주기적
모니터링 주기)는 `zlink_ctx_reset_auto_hwm_budget_metrics`를 호출한다 — `measurement_epoch`을
올리고 peak counter를 현재 값으로 다시 기준화하며 blocked-ratio와 oversize counter를 0으로
되돌리지만, `budget_generation`, 계획, 현재 byte, monitor queue field는 그대로 둔다.

---

전체 근거는 [Context 스펙](../spec/core/01-context.ko.md)을 참고한다.
