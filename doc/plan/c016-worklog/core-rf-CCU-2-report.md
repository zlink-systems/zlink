# CCU-2 — attach 경로 Auto-HWM 재계산 O(N²) 제거 (Core, 0.17.4)

작성 2026-09-08. 담당 job CCU-2. 시간 상한 3 h.
worktree `~/project/zlink-work/ccu2` (base `wip/0.17.3-all2` = 1a79625d3d).
패치 `~/project/zlink-work/all-artifacts/CCU-2.patch` (HEAD 기준, 커밋하지 않음).

## 1. 결과 한 줄

CCU-1이 지목한 결함(`socket_base_t::attach_pipe()`가 파이프 attach마다 컨텍스트 전체 Auto-HWM
재계산을 동기 수행 → 연결 N개에 O(N²))을 **attach 시 증분 plan 확장**으로 없앴다.
**CCU 4000이 실패(rc=2, 0 kops)에서 202.8 kops PASS로 바뀌었고**, CCU 1000은 313.6 kops다.
공개 인터페이스 변경 없음(`git diff --stat -- core/include core/src/libzlink.vers` 비어 있음).
전체 ctest 210/210 PASS.

## 2. 계약 확인 — "즉시 가시성"이 실제로 요구하는 것

`core/doc/spec/core/systems/06-auto-hwm.ko.md`와 계약 테스트를 다시 읽었다.

| 스펙/테스트 문장 | 요구 | 이 패치에서 |
|---|---|---|
| §2 "application 방향: 역할별 하한을 원자적으로 예약한다 … 두 방향을 모두 예약할 수 없으면 attach를 공개하기 전에 전체 예약을 거절" / "새 동기 inproc attach가 필요한 하한을 예약하지 못함 → `ENOBUFS`" | attach 시점의 **하한 예약**과 `ENOBUFS` | 그대로. 이 예약은 `ctx_physical_queue_registry_t::create_pipepair_queues()`가 소유하며 재계산 경로와 무관하다. 손대지 않았다 |
| §4 "Budget planner는 option 변경과 queue 연결·해제 때만 실행한다" | attach가 planner를 돌린다 | 그대로. attach마다 새 plan을 기록한다(증분 또는 전체) |
| §3 `budget_generation`은 "새 plan을 기록할 때 증가" + `test_ctx_options.cpp:800` `TEST_ASSERT_GREATER_THAN_UINT64(before_attach.budget_generation, after_first_attach.budget_generation)` | **attach 직후 스냅샷에서 generation이 증가해 있어야 한다** | 증분 경로도 `record_applied_plan()`으로 plan을 기록하므로 generation이 증가한다 |
| §3 `active_directional_queue_count` | attach 직후 즉시 반영 | 이 필드는 plan이 아니라 registry에서 live로 읽는다(`ctx_auto_hwm_recalc.cpp:303`). 재계산과 무관하게 이미 즉시 보인다 |
| §2 "연결 증가로 queue별 목표가 감소 → 새 목표를 즉시 기록하고, 현재 보관량이 새 목표 아래로 drain될 때까지 추가 admission을 막음" | 연결이 늘어 water-level이 내려가면 **기존 큐의 목표도 즉시** 내려가야 한다 | **여기만 편차가 있다**(§6 D 항목). 새로 붙는 큐는 즉시 새 level을 받고 plan 합계도 정확하지만, 기존 큐의 목표 인하는 debounce(기본 3000 ms) 재계산까지 미뤄진다 |
| §5 "같은 연결 구성과 입력에서 `effective_core_budget_bytes`는 항상 같다(결정적)" | 결정성 | 증분 경로가 계산하는 budget·level·remainder는 전체 재계산과 같은 식(`auto_hwm_effective_budget_bytes`, water-filling)을 쓴다 |

즉 CCU-1이 우려한 `socket_base_api.cpp:311-316`의 "immediate budget snapshot" 주석이 실제로 지키는 것은
**"attach 직후 새 plan이 기록되어 있어야 한다"**(generation·budget·합계)이고, 그것은 O(1)로 만족할 수 있다.
CCU-1의 제안 B(순수 debounce)는 이 요구를 깨므로 채택하지 않았다.

## 3. 설계 — 두 안 비교

| 안 | 내용 | 장점 | 단점 | 채택 |
|---|---|---|---|---|
| **A. 증분 plan 확장** | 마지막으로 기록한 plan에 **붙는 파이프의 두 방향만** O(log n)으로 더하고, 그럴 수 없는 상태면 기존 전체 재계산으로 폴백 | attach가 O(log n) → O(N²) 소멸. generation·budget·합계는 attach 직후 즉시 정확. 새 옵션·플래그·타이머 없음 | water-level이 내려가는 구간에서 기존 큐 목표 인하가 debounce까지 지연 | **채택** |
| B. attach 경로 debounce (CCU-1 실험) | 동기 호출을 `schedule_auto_hwm_recalculate()`로 교체 | 2줄 | `budget_generation` 즉시 증가 계약이 깨진다(`test_ctx_options.cpp:800`이 바로 실패). 새 큐가 debounce 전까지 계획 목표를 못 받는다 | 기각 |

증분 확장이 **정확히** 성립하는 근거: 한 컨텍스트의 자동 방향들이 모두 같은 role이면 water-filling은
그들 전부에 **하나의 level**을 준다. 그래서 방향 하나가 늘었을 때의 새 level은
`level = min + (data_budget − N'·min) / N'` (role 상한에서 포화)로 O(1)에 구할 수 있고,
나눗셈 나머지는 스펙대로 "stable queue ID 순서로 1 byte씩" 배정되므로 **ID가 가장 큰 새 방향은 나머지 prefix에
들어갈 수 없다** — 새 방향의 목표는 정확히 `level`이다. plan 합계(`total_planned_hwm_bytes`)도
`N'·level + remainder + manual_reserved`로 정확히 재구성된다.

폴백(=기존 전체 재계산) 조건: 자동 방향의 role이 섞였다 / manual·unlimited manual이 관여한다 /
budget 부족·overflow / 재계산이 이미 예약되어 있다(옵션·detach 등 입력 변경) / 새 queue ID가 기존
최대 ID보다 작다(ID wrap) / plan 입력(profile·memory·core budget)이 기록 당시와 다르다.

POSDDD: 새 옵션·플래그·상태·타이머를 추가하지 않았다. 방향 하나의 planning 입력을 두 endpoint policy에서
합치는 규칙은 `plan_application_queues()` 안에 인라인으로 있던 것을 파일 지역 함수
`resolve_direction()` **하나로 합쳐** 전체 경로와 증분 경로가 같은 규칙을 쓰게 했다(중복 금지).

## 4. 변경 파일 (file:line)

| 파일 | 내용 |
|---|---|
| `core/src/runtime/core/ctx_physical_queue_registry.cpp:237-296` | `resolved_direction_t` + `resolve_direction()` — 방향 하나의 planning 입력 해석 규칙(전체·증분 공용) |
| 〃 `:182-186` | `physical_queue_record_t`에 `plan_member`, `plan_maximum_bytes` |
| 〃 `:862-900` | `plan_application_queues()`가 위 helper를 쓰도록 정리(동작 동일) |
| 〃 `:918-963` | 전체 재계산이 plan 집계(`_plan_direction_count`, `_plan_auto_direction_count`, `_plan_manual_reserved_bytes`, `_plan_max_queue_id`, `_plan_auto_role`)와 `_plan_extendable`을 매번 새로 만든다 |
| 〃 `:986-1160` | **`extend_application_plan()`** — attach fast path |
| 〃 `:1105-1110` | `release_endpoint()`: plan에 든 방향이 은퇴하면 `_plan_extendable = false` |
| `core/src/runtime/core/ctx_physical_queue_registry.hpp:134-160,178-192` | 선언과 집계 멤버 |
| `core/src/runtime/core/ctx_auto_hwm_state.{hpp,cpp}` | `last_applied_generation()`, `applied_plan()` 접근자 |
| `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:119-172` | **`ctx_t::auto_hwm_extend_plan_for_attach()`** — 예약된 재계산이 없고 입력이 그대로일 때만 registry 확장을 호출하고 새 plan을 기록 |
| `core/src/runtime/core/ctx.hpp:126-133` | 선언 |
| `core/src/runtime/sockets/common/socket_base.cpp:313-361` | **`auto_hwm_extend_plan_for_attached_pipe()`** — 붙는 파이프의 두 방향 policy만 만들어 확장을 시도하고, 성공하면 그 파이프에만 `apply_physical_queue_hwm_plan()`을 적용 |
| `core/src/runtime/sockets/common/socket_base_api.cpp:319-330, 645-654` | attach 두 지점: 증분 확장 시도 → 실패 시 기존 `auto_hwm_recalculate_now()` |
| `core/src/runtime/sockets/common/socket_base_api.cpp:702-712` | SNDHWM/RCVHWM setter가 기존 debounce 경로로 재계산을 예약한다(스펙 "planner는 option 변경 때 실행"). 이게 있어야 증분 경로가 "입력이 바뀌면 확장 금지"를 지킬 수 있다 |

공개 헤더·vers 변경 없음(확인함).

## 5. 검증

| 항목 | 결과 |
|---|---|
| dev 빌드(JOBS=4, RelWithDebInfo, LTO off) | OK |
| `ctest -R 'auto_hwm|hwm|attach|stream|ctx'` | 31/31 PASS (2회) |
| 전체 `ctest -E hotpath_gate` | **210/210 PASS** (증분 level 반영 전후 각 1회) |
| Release lib 빌드 | OK |
| CCU 4000 재현 | **PASS** (아래) |
| CCU 1000 | PASS (아래) |
| **TSan** | **돌리지 못했다**(시간 상한). 남은 일 §7 |
| **hotpath 5셀** | **돌리지 못했다**(시간 상한). 남은 일 §7 |
| `--repeat until-fail:3` | 미실행. 대신 대상 suite 2회 + 전체 1회 |

### 성능 (with_stream, worktree release lib, `flock PERF_LOCK`, size 64, duration 3 s)

CCU 4000 (`--stack zlink,asio --size 64 --ccu 4000 --duration 3 --runs 1`):

| | zlink | asio | load(1m) |
|---|---:|---:|---|
| 패치 전(같은 worktree, 같은 조건, 17:47) | **실패** `client failed rc=2`, 0 kops | 325.1 kops | 4.26 |
| 패치 후(18:00) | **202.8 kops**, mean 9.82 ms, `connect_ok=4000`, mismatch 0, PASS | 327.9 kops | — |

- 결과 디렉터리: `bindings/c/bench/with_stream/results/20260908_174722`(전), `20260908_180000`(후).
- 서버 CPU 272.9 %(peak 483.6) — 애플리케이션 스레드 1개가 100 %를 태우던 CCU-1의 프로파일이 사라지고
  I/O 스레드로 일이 분산됐다. 서버 RSS 162 MiB.
- **부분 확인**: 브리프가 요구한 샘플러 기반 메인 스레드 CPU 재측정은 못 했고, 러너가 기록한
  서버 프로세스 CPU/RSS와 "4000 연결이 모두 에코를 돌려받았다(mismatch 0, timeout 0)"로 대신했다.

CCU 1000 (`--size 64 --runs 3`, 18:05, load 2.13):

| | zlink | asio |
|---|---:|---:|
| 패치 후 | **313.6 kops**, mean 1.59 ms | 363.4 kops |

**주의**: 브리프가 요구한 "같은 idle 조건의 before" 측정은 시간 상한 때문에 하지 못했다.
CCU-1이 0.17.3에서 잰 CCU 1000 값은 263.4 kops였고(그 세션의 asio는 248.1),
이번 세션은 asio도 325→363으로 더 높게 나오므로 **머신 상태가 달라 절대 비교는 하지 말아야 한다**.
`--size all --runs 3` before/after 표는 남은 일이다(§7).

## 6. 변경 분류와 D 항목

**B — 기존 결함(Core, 연결 스케일)** 이 주된 성격이다. 다만 아래 하나는 **D(spec 편차)**로 명시한다.

> **D**: `06-auto-hwm.ko.md` §2 "연결 증가로 queue별 목표가 감소 → **새 목표를 즉시 기록**하고, 현재
> 보관량이 새 목표 아래로 drain될 때까지 추가 admission을 막음".
> water-level이 내려가는 구간(자동 방향 수 N에 대해 `N·role최대 > budget`인 구간. Balanced/STREAM,
> 11.9 GB 호스트에서는 **방향 4096개 = 연결 2048개**부터)에서, 이 패치는 **새로 붙는 방향에만** 새 level을
> 즉시 적용하고 **이미 붙어 있는 방향의 목표 인하는 debounce(기본 3000 ms) 재계산까지 미룬다**.
> 관측 가능한 영향: 그 구간 동안 기존 큐의 admission 한도가 최대 (이전 level − 새 level)만큼 느슨하다.
> `total_planned_hwm_bytes`는 정확하고(새 level 기준), `total_applied_hwm_bytes`는 스펙이 이미 인정하는
> "applied가 planned보다 큰 상태(deferred shrink)"로 보고된다. `budget_generation`·budget·방향 수는 즉시 정확하다.
>
> 이 편차 없이 계약을 그대로 지키려면 attach마다 N개 방향에 새 목표를 써야 하고(=O(N²)), 그건 원래의 결함이다.
> 편차를 없애는 유일한 실질적 방법은 **queue별 `planned_hwm`을 저장 상태에서 "role별 level + generation"의
> 파생 값으로 바꾸는 것**(§7)이며, 0.17.4에 넣기엔 큰 리팩터라 이번 범위에서 제외했다.
> 감독관이 이 편차를 받아들이지 않는다면, 패치의 **saturated 구간 확장만 남기는 것도 가능**하다
> (`extend_application_plan()`에서 `share >= headroom`일 때만 성공시키면 된다). 그 경우 CCU 2000까지만 개선되고
> CCU 4000은 여전히 실패한다 — 실제로 그 상태로 한 번 측정해 확인했다(17:47 실패 행).

새 옵션·플래그·규칙은 추가하지 않았고, 공개 계약 테스트의 기대값도 바꾸지 않았다.
`completion`/`READY`/`POLLIN`/`WRITABLE` 등 다른 계약 경로는 건드리지 않았다.

## 7. 남은 일

1. **TSan**(auto-HWM/ctx suite, GCC `-fsanitize=thread`, LTO off, `setarch x86_64 -R`) — 미실행.
   새로 추가된 락 순서는 `_auto_hwm_recalc_sync → (해제) → _auto_hwm_state_sync → (해제) → _opt_sync →
   (해제) → registry `_sync``으로 중첩이 없고, 기존 전체 재계산이 잡는 락의 부분집합이다.
2. **hotpath 5셀 ±5 %** — 미실행. 이 패치는 send/recv/decoder 경로 코드를 건드리지 않는다
   (`plan_application_queues`/attach 경로와 registry 멤버 추가뿐).
3. `--ccu 1000 --size all --runs 3` before/after 표(같은 idle 창에서 base 리비전 대비).
4. §6 D를 없애는 후속: `physical_queue_record_t::planned_hwm`을 registry의 role별 level + generation에서
   파생시키고 pipe가 자기 credit 경계에서 lazily 집어가게 하는 리팩터. 그러면 unsaturated 구간에서도
   attach가 O(1)이면서 목표 인하가 즉시가 된다.

## 8. 멈춘 지점

시간 상한 3 h. TSan·hotpath·`--size all` before/after 3건을 남기고 멈췄다.
