# auto-HWM profile 재조정 — 5d2bf1e84f의 20%/1MiB 변경을 대체 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
>
> 브랜치 / HEAD: `codex/bindings-0.12.0-performance` / `a650e68d7f`
> 관련 문서: `doc/plan/core-send-completion-design.ko.md` (같은 사이클의 Core 작업)
> 구현 로그: `log/2026-08-23-core-send-complete-impl.md` §GROUP C

## 1. 이 로그가 기록하는 것

오늘 오전 `5d2bf1e84f` ("perf: finalize byte-HWM regression work and handoff")가
`core/src/runtime/core/auto_hwm_policy.cpp`의 Balanced profile을
`{10%, 64 KiB, 4 MiB, ...}`에서 `{20%, 64 KiB, 1 MiB, ...}`로 바꿨다.
**이 변경은 소유자 결정으로 재조정된 profile 표로 대체(superseded)됐다.**

`5d2bf1e84f`가 함께 넣었던 Balanced `recv_ingress`(SUB/XSUB) 전용 상한 2 MiB
예외는 percent/cap 축과 무관한 역할별 조정이므로 **유지**했고, 이번에 spec
표의 각주로 명문화했다. 이 예외까지 되돌릴지는 소유자 판단이 필요하므로
그대로 두고 기록만 남긴다.

## 2. 대체된 값과 새 값

| Profile | 이전(5d2bf1e84f 이후) 비율 | 새 비율 | 새 고정 cap | 일반 data 하한 | 일반 data 상한(이전 → 새) |
|---|---:|---:|---:|---:|---:|
| Compact | 2% | 2% | 64 MiB | 32 KiB | 1 MiB → 512 KiB |
| LowLatency | 5% | 3% | 256 MiB | 32 KiB | 2 MiB (변화 없음) |
| Balanced | **20%** | **5%** | 512 MiB | 64 KiB | **1 MiB** (5d2bf1e84f 값 유지) |
| Throughput | 20% | 8% | 1024 MiB | 128 KiB | 16 MiB → 8 MiB |

STREAM 하한·상한 열은 소유자가 바꾸지 않았으므로 그대로 유지했다
(Compact 8 KiB/32 KiB, LowLatency 16 KiB/64 KiB, Balanced 64 KiB/128 KiB,
Throughput 256 KiB/512 KiB). 어떤 profile에서도 새 data 상한이 그 profile의
STREAM 상한 아래로 내려가지 않으므로 (최소 격차는 Compact의 512 KiB vs
32 KiB) 비례 조정이 필요한 profile은 없다.

## 3. 새 budget 산식

percent 하나로 budget을 정하던 방식을 **effective cap으로 자르는 방식**으로
바꿨다.

```text
percentShareBytes =
    (resolvedMemoryLimitBytes / 100) * profilePercent
  + ((resolvedMemoryLimitBytes % 100) * profilePercent) / 100

effectiveCapBytes =
    max (profileFixedCapBytes,
         activeDirectionalQueueCount * perQueueMinimumBytes)

effectiveCoreBudgetBytes = min (percentShareBytes, effectiveCapBytes)
```

`perQueueMinimumBytes`는 해당 profile의 일반 data 역할 하한이다.

- **고정 cap만 두면** queue가 많은 배치가 자기 역할 하한 아래로 밀린다.
  Balanced에서 queue 16384개는 하한만으로 1 GiB가 필요한데 고정 cap은
  512 MiB이므로 budget이 부족해진다.
- **queue 바닥값만 두면** 큰 호스트가 몇 개 안 되는 queue에 수 GiB를
  예약한다. 64 GiB 호스트의 5%는 3.2 GiB다.

두 항의 `max`가 이 둘을 동시에 막고, percent와의 `min`이 "호스트가 실제로 준
메모리 이상은 쓰지 않는다"를 유지한다.

## 4. 산식을 어디에서 계산하는가

`activeDirectionalQueueCount`는 `plan_make` 시점에 존재하지 않는다.
`ctx_auto_hwm_recalc.cpp:111`의 `auto_hwm_context_plan_make()`는 budget
scalar만 만들고, 계획 가능한 application direction 전체는
`ctx_physical_queue_registry.cpp`의 `plan_application_queues()`가
`auto_hwm_context_finalize()`를 호출할 때 비로소 확정된다.

따라서 budget은 **두 지점 모두**에서 계산한다.

1. `auto_hwm_context_plan_make()` — queue count를 0으로 놓고 계산한다. 결과는
   profile의 고정 cap이다. finalize되지 않은 plan(예: `create_pipepair_queues`의
   per-pipe-pair plan)도 정상적인 값을 갖게 하기 위한 seed다.
2. `auto_hwm_context_finalize()` — pass-1 루프가
   `active_directional_queue_count`를 다 세고 난 직후, data budget을 나누기
   **전에** 다시 계산한다. `configured_core_budget_bytes > 0`이면 수동 budget이
   우선이므로 재계산하지 않는다.

`plan_application_queues()`가 finalize 직후 `active_directional_queue_count`를
`queue_plans.size()`로 덮어쓰지만, 각 plan이 `send_queue_count=1,
receive_queue_count=0`으로 준비되므로 finalize가 센 값과 같다. 즉 재계산이
쓰는 값과 registry가 최종적으로 기록하는 값이 일치한다.

## 5. 반영 위치

- `core/src/runtime/core/auto_hwm_policy.{hpp,cpp}` — `profile_budget_t`에
  `fixed_cap` 열 추가, 표 교체, `profile_effective_budget()` 추가,
  `plan_make`/`finalize` 배선, 내부 조회 함수 3개
  (`auto_hwm_profile_percent`, `auto_hwm_profile_fixed_cap_bytes`,
  `auto_hwm_effective_budget_bytes`).
- `core/tests/unittest/unittest_auto_hwm_policy.cpp` — 기존 기대값 갱신
  (Balanced 20%→5%: 120→30, 140→35; Compact routed 상한 1 MiB→512 KiB),
  신규 테스트 3개 (`test_profile_percent_and_fixed_caps`,
  `test_effective_budget_is_percent_clamped_by_cap_and_queue_floor`,
  `test_finalize_recomputes_budget_from_queue_count`), LOW_LATENCY·THROUGHPUT
  경계 assertion 추가.
- `core/doc/spec/core/01-context.{ko,en}.md` — profile 표에 고정 cap 열 추가,
  effective cap 산식 명문화, Balanced `recv_ingress` 2 MiB 예외 각주.
  (`doc/site/docs/spec/core`는 같은 파일을 가리키므로 별도 미러링 불필요.)

## 6. 확인

`unittest_auto_hwm_policy` 전체 그린. 결과는 구현 로그
`log/2026-08-23-core-send-complete-impl.md` §테스트 결과에 함께 기록한다.
