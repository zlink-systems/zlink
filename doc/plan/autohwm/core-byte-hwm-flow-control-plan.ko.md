# Core byte HWM과 흐름 제어 작업 계획

> 이 문서는 Core 구현 순서와 계약 변경 후보를 정하는 작업 계획이다. 현재 공개 계약을
> 정의하지 않는다. API와 event 이름은 정식 Core spec과 binding 문서에서 확정하기 전에는
> 사용할 수 없다.

작업 전에 [HWM과 application backpressure 설계 의도](./00-hwm-backpressure-design-intent.ko.md)를
먼저 읽는다. 공통 문서는 전체 책임과 message 흐름을 설명하고, 이 문서는 Core 실행 절차와
완료 증거를 소유한다.

## 1. 작업 범위

현재 작업은 Core 변경과 성능 회복까지만 수행한다. Framework source, Framework public API,
Framework spec과 Framework test는 변경하지 않는다. Core 완료 조건을 통과하면 이번 작업을
종료하고, Framework 작업은
[Framework job backpressure 후속 계획](./framework-job-backpressure-plan.ko.md)에 따라 별도로
시작한다.

Core 작업은 다음 두 결과를 만든다.

1. 기존 count HWM과 같은 pipe-local 차단·재개 동작을 byte 단위로 유지하면서 성능 회귀를
   만든 byte-HWM 내부 구조를 단순화한다.
2. Paired DEALER/ROUTER의 completion lane으로 `PAUSED`와 `RUNNING`을 동기화하고 remote
   PAUSE를 기존 send backpressure와 독립된 차단 원인으로 합성한다.

Binding parity의 범위는 C ABI mirror만이 아니다. cpp, dotnet, go, java, node, python, rust
7개 언어 binding의 flow-state 구현과 focused test까지 이번 Core 작업에 포함한다.

첫 범위에서 PAIR, PUB/SUB 계열과 STREAM에는 remote PAUSE를 추가하지 않는다. 이 socket은
기존 byte HWM과 transport backpressure를 유지한다. 지원하지 않는 socket에 흐름 상태 API를
호출하면 `not supported` 결과를 반환한다.

### 1.1 새 작업자의 시작 절차

이 문서와 아래 파일만 현재 작업의 입력으로 사용한다. 이전 대화의 중간 결론이나 서로 다른
시점의 perf report를 완료 근거로 사용하지 않는다.

1. Repository root에서 `git branch --show-current`와 `git status --short`를 확인한다.
2. 현재 branch는 `codex/bindings-0.11.1-performance`다. 이 branch에서는 사용자 승인 없이
   commit, stash, reset, restore와 origin push를 사용할 수 있다. Branch 전환은 여전히
   사용자 승인이 필요하다.
3. 시작 시점의 dirty worktree snapshot은 tag `backup/autohwm-worktree-20260822`
   (`gmon.out` 제외)에 보관되어 있다. 기존 변경을 제거하거나 덮어쓰는 단계(특히 7장의
   기준점 제거) 전에는 같은 방식으로 현재 상태를 commit 또는 tag로 남긴 뒤 진행한다.
4. 기존 변경과 `gmon.out`은 사용자 작업이다. 이 문서 범위와 겹치지 않으면 수정하지 않는다.
5. 아래 필수 규칙과 계약 문서를 읽고 source symbol을 확인한 뒤에만 코드를 변경한다.
6. 작은 focused test와 한 개 perf case부터 시작한다. 같은 원인으로 전체 matrix를 반복하지
   않는다.

### 1.2 필수 규칙과 기준 문서

| 구분 | 읽을 문서 | 확인할 내용 |
|---|---|---|
| 공통 설계 의도 | `doc/plan/autohwm/00-hwm-backpressure-design-intent.ko.md` | Core HWM과 Framework pressure를 분리한 이유, PAUSE와 heartbeat 흐름 |
| 저장소 작업 규칙 | `AGENTS.md` | Branch, dirty worktree, 검증과 보호 문서 규칙 |
| Plan 작성 규칙 | `doc/AGENTS.md` | Plan과 정식 spec의 역할 분리 |
| 한국어 문서 원칙 | `doc/principal/documentation/documentation-principles.ko.md` | 현재 상태, 독자와 문장 구조 |
| Spec 작성 원칙 | `doc/principal/documentation/spec-writing-guide.ko.md` | 계약, exact interface와 검증 작성법 |
| Perf 디렉터리 규칙 | `bindings/c/perf/AGENTS.md` | `core/build` runtime과 provenance 확인 |
| 공통 perf 정책 | `doc/perf/PERF_POLICY.md` | Paired 측정, workload·metric과 결과 판정 |
| Single 정책 | `doc/perf/PERF_SINGLE_TEST_POLICY.md` | Single pattern, transport, size와 runner 계약 |
| Multi 정책 | `doc/perf/PERF_MULTI_TEST_POLICY.md` | `*_SENDSEND`, CCU와 STREAM runner 계약 |
| C perf 실행기 | `bindings/c/perf/README.md` | Local/release runtime, Auto-HWM과 CLI |
| 기존 성능 handoff | `doc/plan/core-byte-hwm-performance-regression-handoff.ko.md` | 이미 통과한 test, 남은 회귀와 보호 대상 |
| Context 계약 | `core/doc/spec/core/01-context.ko.md`, `.en.md` | Auto-HWM budget과 snapshot |
| Socket 공통 계약 | `core/doc/spec/core/socket/README.ko.md`, `.en.md` | Byte HWM, LWM, oversize와 retained receive |
| Paired socket 계약 | `core/doc/spec/core/socket/06-dealer.ko.md`, `07-router.ko.md`와 영어 mirror | Completion lane과 routed send-ready |
| Error·event·monitor | `core/doc/spec/core/03-errors.ko.md`, `05-events.ko.md`, `07-monitoring.ko.md`와 영어 mirror | Result enum, event namespace와 snapshot |
| Runtime 경계 | `core/doc/spec/core/09-runtime-boundary.ko.md`, `.en.md` | Core 내부 처리와 binding 경계 |
| C binding 계약 | `bindings/doc/spec/c/README.ko.md`, `.en.md` | C ABI와 public type |

정식 Core와 binding spec은 보호 경로다. 구현 중 spec 변경이 필요해도 사용자가 해당 경로와
범위를 승인하기 전에는 수정하지 않는다. 미승인 상태에서는 필요한 `file:line`, 이유와
수정안을 결과에 남긴다.

### 1.3 현재 구현 탐색 지도

| 책임 | 먼저 볼 source | 핵심 symbol |
|---|---|---|
| Pipe byte accounting·credit | `core/src/runtime/core/pipe.hpp`, `pipe.cpp` | `frame_accounted_bytes`, `check_hwm_for_message`, `account_inbound_frame`, `compute_lwm`, `apply_lwm_hint`, `_bytes_written`, `_peers_bytes_read` |
| Auto-HWM 정책 | `core/src/runtime/core/auto_hwm_policy.hpp`, `.cpp` | Manual reservation, minimum·maximum, `budget_insufficient` |
| Context 재계산·snapshot | `core/src/runtime/core/ctx_auto_hwm_recalc.cpp`, `ctx_auto_hwm_state.*` | Queue plan, recalculation과 monitoring snapshot |
| Physical queue·retained credit | `core/src/runtime/core/ctx_physical_queue_registry.hpp`, `.cpp` | Queue generation, decoder reservation, retained origin과 lease |
| Decoder admission | `core/src/runtime/core/session_base_pipe_io.cpp`, `core/src/runtime/protocol/zmp_decoder.*`, `core/src/runtime/engine/asio/asio_zmp_engine.cpp` | Allocation 전 reservation과 commit/release |
| Paired lane 생성 | `core/src/runtime/sockets/common/socket_base_endpoint.cpp` | `transport_lane_application`, `transport_lane_completion`, pair generation |
| Paired 정책 | `core/src/runtime/core/transport_pair_policy.hpp`와 paired DEALER/ROUTER runtime | Completion socket buffer와 lifecycle |
| Core public API | `core/include/`, `core/src/api/core/` | Public enum·function, config result mapping |
| Event·monitoring | `core/src/api/monitoring/`, `core/src/runtime/monitoring/` | Event 배정, snapshot과 reset |
| C binding mirror | `bindings/c/include/` | Core header와 ABI parity |
| 언어 binding parity | `bindings/{cpp,dotnet,go,java,node,python,rust}/` | 각 언어의 flow-state type·method와 오류 mapping |

Source 파일명이나 symbol이 바뀌었으면 `rg`로 새 owner를 찾는다. 같은 의미의 helper나
registry를 새로 추가하기 전에 기존 abstraction이 책임을 소유하는지 확인한다.

## 2. 현재 상태와 기준점

현재 작업 트리에는 byte-HWM 구현이 복구되어 있다. Pipe-local byte counter와 LWM wakeup,
retained-credit, physical queue registry, decoder admission과 관련 lifecycle 코드가 존재한다.

Byte-HWM 전체 제거는 성능 원인을 확인하기 위한 임시 진단이었다. 제거 상태에서 짧은
ROUTER/ROUTER TCP 256 B case의 처리량 회복 경향을 확인한 뒤 소스를 복구했다. 제거 상태를
최종 구현으로 유지하거나 commit하지 않았다. 일부 기존 비교는 HWM과 OS socket buffer
조건이 달랐으므로 최종 성능 판정으로 사용하지 않는다.

첫 구현 단계에서 복잡한 byte-HWM 변경을 다시 제거해 기능·성능 기준점을 재현한다. 그
기준점 위에 이 문서가 유지 대상으로 정한 pipe-local accounting과 wakeup만 최소 구조로
다시 적용한다.

### 2.1 이미 확인된 결과

다음 항목은 이전 handoff에서 통과가 확인됐지만, 최종 변경 뒤 다시 실행해야 한다.

- `test_zmp_request_reply`
- `unittest_auto_hwm_policy`
- `unittest_zmp_decoder`
- `test_ctx_options`
- `test_retained_hwm_credit`
- `test_router_handover`
- `test_connect_rid`
- ASAN PUBSUB TCP 64·256 B focused run

`test_router_mandatory_hwm`은 기존 byte-HWM과 routed multipart 회귀를 소유하지만 위 focused
7개 재실행에는 포함되지 않았다. 이번 작업의 필수 test에 추가한다.

### 2.2 아직 완료되지 않은 결과

- `0.10.1`과 조건을 맞춘 paired median 성능 증거가 없다.
- Multi `ROUTER_ROUTER_SENDSEND / tcp / 256 B`는 알려진 첫 회귀 case다.
- Auto-HWM 계산값을 실제로 채워 `EAGAIN`을 확인하고 drain 뒤 재개하는 작은 end-to-end
  test가 없다.
- Byte-HWM 제거 기준점, 최소 재구현, completion-lane flow state를 단계별로 비교한 결과가
  없다.
- PAUSE/RESUME C API, event와 metric은 아직 구현되지 않았다.

과거 보고서의 단일 수치는 진단 참고일 뿐 완료 증거가 아니다. Local과 release의 HWM,
OS buffer, runtime provenance 또는 실행 시점이 다르면 비교를 폐기한다.

## 3. 유지할 byte-HWM 계약

### 3.1 Byte charge와 admission

`ZLINK_OPT_SNDHWM`과 `ZLINK_OPT_RCVHWM`은 byte 단위이며 `0`은 unlimited다. Application이
설정한 finite HWM은 자동 계산값보다 우선한다.

일반 frame의 charge는 다음과 같다.

```text
normal frame charge = payload bytes + sizeof(msg_t)
```

Delimiter, join과 leave frame은 payload를 더하지 않고 `sizeof(msg_t)` metadata만 계산한다.
Complete message는 모든 frame charge를 합산한다. Routing ID와 credential의 byte charge는
accounting에 포함하지만 application message count에는 포함하지 않는다.

다음 결과를 유지한다.

- Finite HWM을 채우면 nonblocking send는 기존 `EAGAIN`을 반환한다.
- Blocking send는 기존 `ZLINK_OPT_SNDTIMEO`까지 기다린다.
- 비어 있는 pipe의 oversize single-part 또는 total-known message 한 건을 허용하는 기존
  예외와 `ZLINK_OPT_MAXMSGSIZE` 검사를 유지한다.
- 전체 크기를 모르는 incremental multipart는 첫 `MORE` frame부터 일반 byte HWM을
  적용한다.
- 이미 시작한 multipart message의 atomicity와 terminal 규칙을 유지한다.
- Message admission의 `too_large` 결과는 HWM full, transport wait와 구분한다.

### 3.2 Credit과 sender wakeup

일반 `read()`는 complete message를 Core queue에서 꺼냈을 때 consumed byte를 누적한다.
기본 LWM은 다음과 같다.

```text
default byte LWM = ceil(HWM / 2)
```

LWM hint가 양수이면 실제 LWM은 기본값과 hint 중 작은 값이다. Hint가 HWM 이상이면
`HWM - 1`로 제한하고 최솟값은 1 byte다. STREAM은 현재 기본 4 KiB hint를 사용하므로
항상 `ceil(HWM / 2)`만 적용된다고 가정하지 않는다.

Reader는 다음 두 조건 중 하나에서 consumed byte의 누적 절대값을 peer에 전달한다.

1. 마지막 전달 뒤 소비한 byte가 실제 LWM 이상이다.
2. Writer가 byte credit을 기다리고 있고 reader가 현재 보이는 inbound queue를 완전히
   비웠다. 이 경우에는 LWM에 도달하기 전에도 한 번 전달한다.

두 번째 조건은 in-flight byte가 LWM보다 작을 때 blocked writer가 영구 대기하는 것을
막는다. 최소 재구현과 contract test는 두 wakeup 경로를 모두 유지한다.

`read_retained()`와 retained-credit token 수명은 일반 dequeue credit과 별도 경로다. 이번
작업에서 retained receive의 public 계약을 삭제하거나 변경하지 않는다. 필요성과 성능은
별도 작업으로 다룬다.

### 3.3 Core memory budget

Core messaging budget은 자동 HWM을 나누는 planning input이며 context 전체 실제 memory의
hard cap이 아니다. 다음 예외를 유지한다.

- Manual finite HWM은 자동 배분보다 우선하며 합계가 budget을 초과할 수 있다.
- Manual HWM `0` 방향은 unlimited이며 aggregate HWM을 유한 값으로 보장할 수 없다.
- 자동 HWM minimum 합계가 남은 budget보다 크면 minimum을 유지하고
  `budget_insufficient` 상태를 기록한다.
- 한 pipe의 HWM 도달이나 oversize 예외가 다른 pipe의 HWM을 줄이거나 admission을 직접
  중단하지 않는다.

자동 배분은 다음 입력 우선순위와 기존 profile 비율·minimum·maximum을 유지한다.

1. 양수 `ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES`
2. 양수 `ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES`
3. 양수 runtime memory hint와 감지된 finite hard limit의 조합
4. 감지된 finite hard limit
5. 감지된 physical memory

Connection 수가 바뀌면 새 snapshot을 계산한다. 새 HWM보다 이미 많이 쌓인 pipe의 message를
제거하지 않고 이후 admission만 제한한다. 정상 frame마다 context 전역 합계나 전역 mutex를
갱신하지 않는다.

## 4. Paired completion lane 흐름 상태

### 4.1 지원 topology와 용어

이 문서의 `Core 흐름 제어 lane`은 paired DEALER/ROUTER의 두 번째 connection인 기존
completion lane, 즉 `transport_lane_completion`을 뜻한다. 새 범용 control socket이나
Framework payload channel을 만들지 않는다.

첫 계약은 paired DEALER/ROUTER만 지원한다. Completion lane이 없는 PAIR, PUB/SUB 계열과
STREAM은 remote PAUSE 대상이 아니며 기존 byte HWM을 사용한다.

### 4.2 상태 frame

Core가 내부에서 처리하는 frame은 다음 값을 포함한다.

| Field | 역할 |
|---|---|
| Protocol version | 지원하지 않는 형식을 거절한다. |
| Transport pair ID | 상태를 적용할 application/completion connection pair를 찾는다. |
| Connection generation | 이전 physical connection의 늦은 frame을 무시한다. |
| Flow epoch | 같은 generation의 중복·역전 상태를 무시한다. |
| State | `RUNNING` 또는 `PAUSED` 절대 상태다. |

상태는 reference count가 아니다. 같은 state의 반복 적용은 결과가 같은 idempotent
operation이다. 새 pair가 ready가 되면 socket이 저장한 최신 local receive-flow state를
전송한다.

### 4.3 Send admission 합성

Remote PAUSE는 local byte HWM counter를 수정하지 않는다. Pipe의 writable 판단은 다음
독립 원인을 합성한다.

```text
send is blocked when
  pipe is inactive because termination has started
  OR paired transport is waiting
  OR local byte HWM is full
  OR remote flow state is PAUSED
```

각 전이는 자신이 소유한 원인만 제거한다. 모든 원인이 사라졌을 때만 기존 send-ready를
발생시킨다. `pipe_message_admission_too_large`는 writable 상태가 아니라 해당 message의
admission 결과이므로 remote PAUSE와 별도로 판정한다.

PAUSE가 multipart 중간에 도착하면 이미 시작한 message의 기존 atomicity를 유지하고 다음
message부터 차단한다. Remote PAUSE 때문에 새 public send status, exception 또는 우회 send
API를 추가하지 않는다.

## 5. Core API 후보

다음 선언은 contract pseudocode이며 실제 API가 아니다.

```c
typedef enum zlink_receive_flow_state_t {
    ZLINK_RECEIVE_FLOW_RUNNING = 0,
    ZLINK_RECEIVE_FLOW_PAUSED = 1
} zlink_receive_flow_state_t;

zlink_config_result_t zlink_socket_set_receive_flow_state(
    void *socket,
    zlink_receive_flow_state_t state);
```

후보 계약은 다음과 같다.

| 조건 | 결과 후보 |
|---|---|
| 유효한 DEALER/ROUTER와 state | Local state를 저장하고 현재 pair와 이후 새 pair에 동기화한다. 같은 state의 반복 호출도 성공한다. |
| `socket == NULL` 또는 유효하지 않은 handle | `ZLINK_CONFIG_INVALID_HANDLE` |
| Enum 범위 밖의 state | `ZLINK_CONFIG_INVALID_ARGUMENT` |
| Completion lane을 지원하지 않는 socket | `ZLINK_CONFIG_NOT_SUPPORTED` |
| Close가 API의 local-state 저장보다 먼저 확정됨 | `ZLINK_CONFIG_INVALID_STATE` |

API 성공은 remote 적용 완료를 뜻하지 않는다. Socket-owning runtime thread가 local state를
저장한 시점을 완료 경계로 삼고, 현재 pair fanout과 새 pair 동기화는 같은 socket state를
기준으로 직렬화한다. Close가 경쟁하면 local-state 저장과 close 중 먼저 확정된 operation의
결과만 유효하다. 정확한 thread-safety와 linearization 규칙은 Core spec에 고정한다.

Raw control send/recv, data queue의 선택적 receive와 remote PAUSE를 우회하는
`zlink_send_infrastructure`는 추가하지 않는다.

### 5.1 언어 binding 표면 후보

각 언어 binding은 확정된 C ABI를 기준으로 다음 표면을 자국 언어 관례에 맞춰 노출한다.
아래 항목은 표면 후보이며 공개 이름과 exact type은 각 언어 spec에서 확정한다.

- `RUNNING`과 `PAUSED` 두 값을 갖는 receive-flow-state enum. 값은 C enum과 같다.
- Socket의 receive-flow-state 설정 operation 한 개. 같은 state의 반복 호출은 성공한다.
- C config result의 언어별 mapping. Invalid handle·argument·state와 not-supported는
  `bindings/doc/spec/README.ko.md`의 기존 오류 정책을 따른다.
- Flow event와 metric은 기존 monitor·snapshot 표면으로만 노출한다.

다음은 어떤 언어에도 추가하지 않는다.

- Flow frame을 직접 encode·decode하거나 receive하는 API
- Remote PAUSE를 우회하는 send 변형
- C ABI에 없는 언어 전용 flow 상태나 통계

## 6. 관측 후보

| Event 후보 | 발생 조건 | 주요 field |
|---|---|---|
| `ZLINK_EVENT_SEND_FLOW_PAUSED` | Remote `PAUSED`를 현재 application pipe에 처음 적용함 | routing ID, pair ID, generation, epoch |
| `ZLINK_EVENT_SEND_FLOW_RESUMED` | Remote `RUNNING`이 remote-pause 원인을 제거함 | routing ID, pair ID, generation, epoch, 실제 writable 여부 |
| `ZLINK_EVENT_FLOW_STATE_STALE` | 이전 generation·epoch 또는 중복 frame을 무시함 | pair ID, received/current generation과 epoch |

정상 data frame마다 event를 만들지 않는다. Metric 후보는
`flow_paused_connections`, `flow_pause_applied_total`, `flow_resume_applied_total`,
`flow_state_stale_total`, `flow_pause_duration_ms`다. 이름, reset과 snapshot consistency는
정식 monitoring spec에서 확정한다.

## 7. 구현 단계

1. Byte-HWM 제거 기준점에서 Core build와 focused test를 통과시킨다.
2. Pipe-local byte accounting, 실제 LWM과 두 wakeup 조건을 최소 구조로 복구한다.
3. Auto-HWM budget 계산과 snapshot을 hot path 밖에서 복구한다.
4. Paired completion lane의 flow frame, generation과 epoch 검증을 구현한다.
5. Socket-wide local state와 DEALER/ROUTER pair 동기화를 구현한다.
6. Remote PAUSE를 독립 send 차단 원인으로 합성한다.
7. Core event, metric과 C API를 구현한다.
8. cpp, dotnet, go, java, node, python, rust binding의 flow-state parity를 구현한다.

각 단계 뒤 focused test와 한 개 성능 case만 실행한다. 첫 회귀 단계에서 원인을 수정한 뒤
다음 단계로 진행한다.

### 7.1 단계별 변경 경계

| 단계 | 주 변경 위치 | 단계 산출물 | 다음 단계로 넘어가는 조건 |
|---|---|---|---|
| 기준점 | `pipe.*`, `ctx_*hwm*`, physical queue와 decoder 연결 | 제거 전 symbol 목록, 제거 기준점 build와 report | Public HWM option을 unlimited로 바꾸지 않고 알려진 perf case가 회복됨 |
| 최소 byte HWM | `pipe.hpp`, `pipe.cpp` | Pipe-local written/read byte, actual LWM과 두 credit wakeup | HWM·oversize·multipart focused test와 첫 paired perf 통과 |
| Auto-HWM | `auto_hwm_policy.*`, `ctx_auto_hwm_*`, 필요한 registry snapshot | Budget plan, applied HWM과 snapshot | Manual·unlimited·minimum 부족 unit test와 paired perf 통과 |
| Decoder·retained 연결 | `ctx_physical_queue_registry.*`, `session_base_pipe_io.cpp`, decoder | Allocation 전 admission과 기존 lease 수명 | Decoder·retained focused test와 paired perf 통과 |
| Core flow state | Paired socket runtime, completion lane, `pipe.*` admission | Frame, generation·epoch, socket state와 reconnect | Flow state contract test와 항상 RUNNING perf 통과 |
| Public API·관측 | `core/include`, `core/src/api`, monitoring, `bindings/c/include` | C ABI, result, event와 metric | ABI·event·snapshot test 통과 |
| 언어 binding parity | `bindings/{cpp,dotnet,go,java,node,python,rust}/` | 언어별 flow-state type·method, 오류 mapping과 focused test | 7개 언어 binding test 통과 |

기준점 제거는 진단 단계다. 제거된 구현을 그대로 최종 결과로 남기지 않는다. Retained receive
API, public Auto-HWM option, monitoring ABI와 기존 test를 삭제해 기준점을 맞추지 않는다.
성능 회귀의 원인이 registry나 decoder reservation으로 좁혀지면 그 owner에서 비용을 제거하되
pipe-local HWM 계약을 호출자나 perf harness로 우회하지 않는다.

### 7.2 구현 중 기록할 증거

각 단계 결과에는 다음을 짧게 기록한다.

- 변경한 source와 책임
- 실행한 focused test와 첫 실패
- Local runtime absolute path, source revision과 build timestamp
- Pattern, transport, size, HWM, OS buffer, Auto-HWM detail
- Local과 `0.10.1` report 경로 및 각 metric median
- 다음 단계 진행 또는 같은 단계 수정 판단

측정값은 plan 본문에 누적하지 않고 runner report나 별도 실행 log에 보관한다.

### 7.3 언어 binding 작업 순서

언어 binding 작업은 Core C API와 `bindings/c` ABI mirror가 확정되고 focused test를 통과한
뒤에만 시작한다. 언어 간에는 의존이 없으므로 한 언어씩 진행하고, C와 표면이 가장 가까운
cpp를 먼저 구현해 mapping 패턴을 확정한 뒤 나머지 언어에 같은 패턴을 적용한다.

| 언어 | 주 변경 위치 | Test 실행 |
|---|---|---|
| cpp | `bindings/cpp/include/`, `bindings/cpp/src/` | `bindings/cpp/tests/run_tests.sh` |
| dotnet | `bindings/dotnet/src/Zlink/` | `bindings/dotnet/tests/run_tests.sh` |
| go | `bindings/go/` root package와 `contracts/` | `bindings/go/tests/run_tests.sh` |
| java | `bindings/java/src/main/`, `kotlin-contract-test` 정렬 | `bindings/java/tests/run_tests.sh` |
| node | `bindings/node/src/` | `bindings/node/tests/run_tests.sh` |
| python | `bindings/python/src/zlink/` | `bindings/python/tests/run_tests.sh` |
| rust | `bindings/rust/src/` | `bindings/rust/tests/run_tests.sh` |

언어별 단계는 다음 순서를 반복한다.

1. 해당 언어 spec의 기존 socket option·오류 mapping 관례를 확인한다.
2. Flow-state enum, socket operation과 오류 mapping을 구현한다.
3. 지원하지 않는 socket 유형의 not-supported fallback을 구현한다.
4. 아래 8.3의 공통 contract test를 해당 언어 test 규칙으로 추가한다.
5. 해당 언어 `run_tests.sh`를 실행하고 첫 실패를 기록한다.

한 언어의 실패 원인이 C ABI나 Core에 있으면 언어 확장을 멈추고 원인 계층을 먼저 수정한
뒤 그 언어부터 재개한다.

## 8. 검증 계획

### 8.1 Contract test

- 일반 frame, delimiter, join과 leave의 byte charge가 계약과 같다.
- Default LWM은 `ceil(HWM / 2)`이고 hint가 있으면 더 작은 실제 LWM을 사용한다.
- LWM에 도달하면 consumed byte의 누적 절대값을 전달한다.
- LWM 미만이어도 blocked writer가 있고 inbound queue가 완전히 비면 credit을 전달한다.
- Finite HWM, drain과 send-ready가 기존 `EAGAIN`·`SNDTIMEO` 계약을 유지한다.
- Oversize와 incremental multipart admission이 기존 결과를 유지한다.
- Local HWM과 remote PAUSE 중 하나만 해제해도 writable이 되지 않는다.
- PAUSE가 multipart 중간에 도착해도 시작한 message의 atomicity를 깨지 않는다.
- 같은 state의 반복 frame과 이전 generation·epoch frame을 안전하게 무시한다.
- Reconnect한 새 pair에 최신 local state를 전송한다.
- PAIR, PUB/SUB 계열과 STREAM API 호출은 `not supported`이고 기존 send 동작은 변하지
  않는다.
- Application이나 Framework에 flow frame을 반환하는 recv 경로가 없다.
- API 호출과 close가 경쟁해도 성공한 local state나 close 중 하나만 관찰된다.

최소 build와 기존 focused test 명령은 repository root에서 실행한다.

주의: `core/build`는 `ZLINK_BUILD_TESTS=OFF`라 test target이 없고, 그 안의 오래된 test
binary는 현재 source를 반영하지 않는다. Test는 반드시 `ZLINK_BUILD_TESTS=ON`으로 구성한
`core/build-tests`에서 빌드·실행한다. Perf runtime provenance는 계속 `core/build`를 쓴다.

```bash
cmake --build core/build --parallel 2

cmake --build core/build-tests --parallel 2

ctest --test-dir core/build-tests --output-on-failure \
  -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options|test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm)$'
```

새 flow-state test target 이름은 구현 시 기존 test naming 규칙에 맞춰 추가한다. Core source나
header를 바꾼 뒤에는 perf 전에 반드시 `cmake --build core/build`를 다시 실행한다. Runtime이
source보다 오래됐거나 runner가 출력한 `libzlink.so`가 `core/build` 밖의 local 경로면 측정을
중단한다.

### 8.1.1 언어 binding parity test

7개 언어 모두 다음 항목을 해당 언어 test 규칙으로 검증한다.

- Flow-state enum 값이 C ABI 값과 같다.
- DEALER/ROUTER socket에서 설정이 성공하고 같은 state의 반복 호출도 성공한다.
- PAIR, PUB/SUB 계열과 STREAM에서 not-supported 오류 mapping을 반환한다.
- Invalid handle·argument·state가 언어 오류 정책대로 mapping된다.
- Close와 경쟁해도 성공한 설정이나 close 오류 중 하나만 관찰된다.
- 공개 표면 test에 flow frame receive·encode API가 없다.
- 기존 HWM, `EAGAIN` 상당 결과와 send timeout 동작이 변하지 않는다.

언어 binding perf 전체 matrix는 이 작업의 gate가 아니다. 성능 gate는 8.2의 C perf가
소유한다. Binding hot path에 코드가 추가되지 않았음을 변경 검토로 확인하고, 의심되는
언어만 기존 perf 한 case를 flow 기능 추가 전과 비교한다.

### 8.2 짧은 성능 비교

전체 version matrix를 한 번에 실행하지 않는다. 알려진 회귀는 multi sendsend이므로 첫 gate는
`bindings/c/perf/run_benchmarks_multi.sh`를 사용한다. Single runner의 pattern 이름과 multi
runner의 `*_SENDSEND` 이름을 섞지 않는다.

#### 8.2.0 Noise floor 확인

첫 비교 전에 한 번만, 같은 `0.10.1` release runtime끼리 첫 회귀 case를 paired로 세 번
실행해 host 분산을 확인한다. 같은 binary인데도 metric median 차이가 판정을 뒤집을 수준이면
host를 안정화한 뒤 본 측정을 시작한다. 이 결과는 판정 근거가 아니라 host 안정성 확인용이다.

#### 8.2.1 첫 회귀 case

같은 source 변경에 대해 다음 두 명령을 local, release, local, release 순서로 각각 세 번
실행한다. 한 명령의 `--runs`는 1로 유지해 두 version의 실행 시점이 멀어지지 않게 한다.

```bash
ZLINK_CORE_SOURCE=local \
  ./bindings/c/perf/run_benchmarks_multi.sh \
  --pattern ROUTER_ROUTER_SENDSEND \
  --transports tcp --msg-sizes 256 --runs 1 \
  --results-tag autohwm-rr-tcp-256-local

./bindings/c/perf/run_benchmarks_multi.sh \
  --core-version 0.10.1 \
  --pattern ROUTER_ROUTER_SENDSEND \
  --transports tcp --msg-sizes 256 --runs 1 \
  --results-tag autohwm-rr-tcp-256-release-0101
```

각 report에서 resolved `libzlink.so`, Core source/version, pattern, transport, message size,
client 수, duration, Auto-HWM profile, applied HWM과 OS buffer를 확인한다. Release가 optional
Auto-HWM snapshot ABI를 제공하지 않아 detail이 없으면 나머지 workload option이 동일한지
runner metadata로 확인한다.

#### 8.2.2 확장 순서

첫 case가 통과한 뒤 다음 case를 한 항목씩 같은 paired 방식으로 실행한다.

1. Multi `DEALER_ROUTER_SENDSEND / tcp / 256 B`
2. Multi `DEALER_ROUTER_REQREP / tcp / 256 B`
3. Multi `ROUTER_ROUTER_REQREP / tcp / 256 B`
4. Multi `PUBSUB / tcp / 256 B`
5. Single `ROUTER_ROUTER / tcp / 256 B`
6. Single `DEALER_ROUTER / tcp / 256 B`

Single case는 `bindings/c/perf/run_benchmarks.sh`와 single 이름을 사용한다. Multi STREAM은
`bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM`으로 별도 실행하며 single suite에
추가하지 않는다. 한 pattern의 TCP와 256 B가 통과한 뒤에만 같은 pattern의 다음 기본 size와
지원 transport를 하나씩 확장한다.

#### 8.2.3 판정 규칙

같은 case의 세 report에서 throughput, bandwidth, mean latency, p95와 p99의 median을 각각
계산한다. 처리율 95%나 latency 105% 같은 완화 gate를 사용하지 않는다.

- Local throughput·bandwidth median은 `0.10.1` median보다 낮지 않아야 한다.
- Local mean·p95·p99 latency median은 `0.10.1` median보다 높지 않아야 한다.
- 한 metric이라도 미달하면 해당 case는 통과가 아니다.
- 결과 분산 때문에 판정이 바뀌면 workload나 threshold를 완화하지 않는다. Host를
  안정화하고 같은 case를 다시 paired 실행한다.
- 첫 미달 case에서 profiler와 source diff로 원인을 찾고, 수정 뒤 같은 case부터 재실행한다.

모든 기본 pattern·transport·size 확장은 Core 단계의 마지막 gate에서 수행하되 비교 단위는
끝까지 한 case씩 유지한다. 서로 다른 시점의 full-matrix report를 결합해 median을 만들지
않는다.

## 9. 문서 변경 계획

정식 문서는 구현 결과만 설명하고 이 계획의 진단 이력을 옮기지 않는다. 한국어와 영어
mirror는 같은 작업에서 변경한다.

| 문서 | 변경할 계약 |
|---|---|
| `core/doc/spec/core/01-context.*.md` | Budget이 automatic HWM planning input이며 hard cap이 아닌 점, manual·unlimited·minimum 부족 예외와 snapshot을 명시한다. |
| `core/doc/spec/core/socket/README.*.md` | Frame charge, oversize, default·hint LWM과 drain wakeup을 명시한다. |
| `core/doc/spec/core/socket/06-dealer.*.md` | Paired completion lane의 flow state, reconnect와 send 결과를 명시한다. |
| `core/doc/spec/core/socket/07-router.*.md` | Socket-wide state, routed pair fanout, routing ID와 generation 경계를 명시한다. |
| `core/doc/spec/core/socket/01-pair.*.md`, `02-pub.*.md`, `03-sub.*.md`, `04-xpub.*.md`, `05-xsub.*.md`, `08-stream.*.md` | Flow API가 지원되지 않으며 기존 byte HWM을 유지하는 범위를 명시한다. |
| `core/doc/spec/core/03-errors.*.md` | 새 API의 invalid handle·argument·state·not-supported 결과를 명시한다. |
| `core/doc/spec/core/05-events.*.md`, `07-monitoring.*.md` | Flow event, metric, field와 reset을 명시한다. |
| `core/doc/spec/core/09-runtime-boundary.*.md` | Core가 completion-lane frame을 내부 처리하고 raw receive API를 제공하지 않는 경계를 명시한다. |
| `bindings/doc/spec/c/README.*.md` | C enum, 함수 signature, 반환값과 ABI를 명시한다. |
| `bindings/doc/spec/{cpp,dotnet,go,java,node,python,rust}/README.*.md` | Binding이 노출하는 flow-state type·method·오류 mapping과 frame을 직접 encode하지 않는 경계를 명시한다. |

`core/doc/spec/`, `core/doc/internals/`와 `bindings/doc/spec/`는 보호 경로다. 실제 구현 단계에서
사용자의 해당 경로 변경 승인을 다시 확인하고 각 하위 `AGENTS.md`를 적용한다.

## 10. 중단 조건과 결과 인계

다음 조건에서는 범위를 임의로 넓히지 않고 사용자에게 근거와 선택지를 보고한다.

- Dirty worktree의 기존 변경과 같은 source를 수정해야 하는데 의도를 분리할 수 없음
- 새 public API나 protected spec 변경이 필요한데 해당 범위 승인이 없음
- Paired DEALER/ROUTER 밖의 topology 지원이 Core 완료에 필요해짐
- Retained receive 계약을 바꾸지 않고 성능을 회복할 수 없다는 근거가 확인됨
- Local과 `0.10.1`의 workload, HWM, OS buffer 또는 runtime provenance를 맞출 수 없음
- 첫 case의 crash·OOM·timeout이 세 번 재현되고 같은 원인에서 더 진행할 수 없음

완료 또는 중단 보고에는 다음만 남긴다.

```text
Result:
Changed source:
Changed public contract:
Focused tests:
Paired perf reports:
First remaining failure:
Framework work started: no
```

중간 조사 이력을 반복하지 않고 report 경로와 현재 첫 실패를 정확히 적는다.

## 11. 완료 조건

- 기존 byte charge, 실제 LWM, drain wakeup, send 결과와 multipart 계약이 유지된다.
- Auto-HWM budget을 aggregate hard cap으로 잘못 구현하지 않는다.
- Remote PAUSE가 local HWM과 독립적으로 합성되고 application send 계약이 유지된다.
- 첫 범위는 paired DEALER/ROUTER로 제한되고 다른 socket의 fallback이 검증된다.
- Core C API, event와 metric focused test가 통과한다.
- cpp, dotnet, go, java, node, python, rust binding의 flow-state parity 구현과 focused
  test가 통과한다.
- Pattern·transport·message size 한 case씩 `0.10.1`과 인접 비교해 성능 gate를 통과한다.
- 전체 perf matrix를 한 번에 실행하지 않는다.
- Framework source, public API, spec과 test에는 변경이 없다.
- `git diff --check`와 Core 변경 범위의 문서 검사가 통과한다.

위 조건을 통과하면 Core 작업을 종료한다. Framework 작업을 자동으로 이어서 수행하지 않는다.

## 12. 진행 checklist

완료한 항목만 `[x]`로 바꾸고 Evidence 열에 test output, perf report 또는 변경 파일 경로를
기록한다. 실행하지 않은 항목을 추정으로 완료 처리하지 않는다. 진행할 수 없으면 `[ ]`를
유지하고 Evidence에 `BLOCKED:`와 첫 원인을 적는다.

### 12.1 시작과 기준점

| Done | 확인 항목 | Evidence |
|---|---|---|
| [x] | Branch와 dirty worktree를 확인하고 보호할 기존 변경을 기록했다. | `doc/plan/autohwm/worklog/stage0-baseline.md` §1: branch=`codex/bindings-0.11.1-performance`, `git status --short`=59 files, tag `backup/autohwm-worktree-20260822` 존재 확인 |
| [x] | 공통 설계 의도, repository·doc·perf 규칙과 기존 handoff를 읽었다. | `00-hwm-backpressure-design-intent.ko.md`, `core-byte-hwm-performance-regression-handoff.ko.md`, `AGENTS.md`, `bindings/c/perf/AGENTS.md`, `doc/perf/PERF_POLICY.md`, `PERF_MULTI_TEST_POLICY.md`, `bindings/c/perf/README.md` 열람 완료 |
| [x] | `pipe`, Auto-HWM, registry, decoder와 retained-credit symbol inventory를 만들었다. | `doc/plan/autohwm/worklog/stage0-baseline.md` §2 (file:line 목록, HEAD vs dirty worktree 존재 여부 포함) |
| [x] | Local `core/build` runtime provenance와 8개 focused test 기준점을 확인했다. | `stage0-baseline.md` §4-5: `libzlink.so.0.11.1` mtime 03:54:22 > 최신 소스 `pipe.cpp` mtime 03:50:13; ctest 8/8 Passed (56.89s) |
| [x] | Multi `ROUTER_ROUTER_SENDSEND / tcp / 256 B` local·`0.10.1` 최초 paired report를 확보했다. | `stage0-baseline.md` §6-7: noise floor 3회 + local/release 각 3회 paired report 6개, median local 129.677 Kops/s vs 0.10.1 187.935 Kops/s (약 69%, 알려진 회귀 재확인) |

### 12.2 Byte-HWM 성능 회복

| Done | 확인 항목 | Evidence |
|---|---|---|
| [ ] | 제거 기준점에서 public HWM option을 유지한 채 기능 test와 성능 회복을 확인했다. | `BLOCKED:` `doc/plan/autohwm/worklog/stage1-removal-baseline.md` — poll 경로 수정 4건과 codex review 지적 4건 수정 후 최종 gate median local/0.10.1 = throughput·bandwidth 89.5%(시작 69.9%), mean/p95/p99 115~117%(시작 142~147%). 5개 metric 모두 미달. 분해 결과 현재 worktree(86.1%)≈`2728d70d44` 부모(84.2%)로 `3ef4d09a37` 회귀는 회복됐고, 잔여는 main merge `f159a51a99`가 소유한다(`dc9fb69735` 174.5 → `f159a51a99` 159.4 Kops/s, 4/4 pair). **주의: 이전 기록의 8/8 test 통과는 stale binary(01:05 산출물) 결과다. 재빌드 시 baseline은 7/8이고 `test_retained_hwm_credit`은 원본 worktree에서 20/20 결정적 실패한다(기존 결함).** |
| [ ] | 일반·delimiter·join·leave frame charge를 최소 pipe-local 구현으로 복구했다. | |
| [ ] | Default·hint LWM과 blocked-writer drain wakeup을 복구했다. | |
| [ ] | Oversize, incremental multipart, `EAGAIN`, `SNDTIMEO`와 send-ready test가 통과했다. | |
| [ ] | Auto-HWM budget을 planning input으로 복구하고 manual·unlimited·minimum 부족 test가 통과했다. | |
| [ ] | Decoder reservation과 retained lease 수명을 바꾸지 않고 관련 test가 통과했다. | |
| [ ] | 각 복구 단계 뒤 첫 multi paired case의 모든 metric이 `0.10.1`보다 나쁘지 않다. | |

### 12.3 Core PAUSE와 RUNNING

| Done | 확인 항목 | Evidence |
|---|---|---|
| [x] | Completion-lane frame의 version, pair ID, generation, epoch와 state를 구현했다. | `core/src/runtime/core/flow_state_frame.hpp` (35 B command frame). `unittest_flow_state_frame` 9/9, `test_flow_state_paired::test_duplicate_and_stale_frames_are_ignored`. 상세: `worklog/stage3-flow-state.md` §2 |
| [x] | Paired DEALER/ROUTER만 지원하고 다른 socket의 not-supported fallback을 구현했다. | `socket_base_t::socket_type_supports_receive_flow_state ()`. `test_flow_state_paired::test_unsupported_socket_types_report_not_supported` (PAIR·PUB·SUB·XPUB·XSUB·STREAM `ENOTSUP` + PAIR 송수신 불변) |
| [x] | Socket-wide local state, reconnect와 close 경쟁을 구현했다. | `core/src/runtime/sockets/common/socket_base_flow_state.cpp`. Fanout과 새 pair 동기화가 `_transport_pairs_sync` 한 mutex를 공유하고, close 경쟁은 `socket_public_api_scope_t` 승인으로 결정한다. `test_flow_state_paired::test_new_and_reconnected_pairs_receive_the_latest_state`, `::test_invalid_state_is_rejected` |
| [x] | Remote PAUSE를 local HWM과 독립된 send blocker로 합성했다. | `core/src/runtime/core/pipe.cpp:1071,1086,1116,1175,1200,1374`, `socket_base_api.cpp:723`. Byte HWM counter 미수정. `test_flow_state_paired::test_local_hwm_and_remote_pause_are_independent` (양방향) |
| [x] | Multipart 중간 PAUSE와 모든 blocker가 해제된 뒤의 send-ready를 검증했다. | `pipe_t::remote_flow_blocked_unlocked ()`의 `_out_incomplete_bytes == 0` 조건. `test_flow_state_paired::test_pause_mid_multipart_preserves_atomicity`, `::test_remote_pause_blocks_sender_and_resume_releases_it` |
| [ ] | C API, event와 metric focused test가 통과했다. | 7단계 범위. 이번 단계는 내부 C++ 계층만 구현했고 `core/include`와 `bindings/`에는 변경이 없다. |
| [ ] | Flow state가 계속 RUNNING인 paired perf가 기능 추가 전 수준을 유지한다. | 이번 단계에서 perf를 실행하지 않았다(다른 작업자가 같은 host에서 측정 중). |

### 12.4 언어 binding parity

| Done | 확인 항목 | Evidence |
|---|---|---|
| [ ] | cpp의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | dotnet의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | go의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | java(kotlin contract 포함)의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | node의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | python의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | rust의 flow-state 표면, 오류 mapping과 contract test가 통과했다. | |
| [ ] | 어떤 언어에도 flow frame receive·encode와 PAUSE 우회 send를 추가하지 않았다. | |

### 12.5 최종 Core gate

| Done | 확인 항목 | Evidence |
|---|---|---|
| [ ] | Multi sendsend·reqrep·PUBSUB를 pattern·transport·size 한 case씩 paired 비교했다. | |
| [ ] | Single ROUTER_ROUTER·DEALER_ROUTER를 같은 방식으로 비교했다. | |
| [ ] | 필요한 경우 multi STREAM을 single과 섞지 않고 별도로 비교했다. | |
| [ ] | 모든 throughput·bandwidth·mean·p95·p99 median이 `0.10.1`보다 나쁘지 않다. | |
| [ ] | 승인된 Core·binding spec과 영어 mirror가 최종 구현과 일치한다. | |
| [ ] | `git diff --check`와 관련 문서 검사가 통과했다. | |
| [ ] | 완료 보고에 변경 source, test, paired report와 남은 실패를 기록했다. | |
| [ ] | Framework source, public API, spec과 test를 변경하지 않았다. | |
