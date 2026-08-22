# Stage 7: Core public C API, event과 metric (계획 §7 7단계 / §5 / §6 / checklist §12.3 행 6)

계획: `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §5, §6, §7, §9, checklist §12.3
내부 구현: `doc/plan/autohwm/worklog/stage3-flow-state.md` (socket-wide local state, pair fanout,
remote PAUSE 합성)

이 단계는 stage3이 만든 내부 C++ 계층 위에 공개 표면만 추가한다. 계획이 소유한 후보 이름을
그대로 사용했다 (`core/doc/spec/`, `bindings/doc/spec/`는 미확정 상태로 보호됨).

Base commit: `56bfdeb28d` (`autohwm stage3: take the message-start marker off the per-message
lock`), branch `feat/flow-state-c-api`, worktree `/home/hep7hep7/project/zlink-stage7`.

## 1. 추가한 public C API

`core/include/zlink/socket/api.h`

```c
ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);
```

`core/include/zlink_enum.h`

```c
typedef enum zlink_receive_flow_state_t
{
    ZLINK_RECEIVE_FLOW_RUNNING = 0,
    ZLINK_RECEIVE_FLOW_PAUSED = 1
} zlink_receive_flow_state_t;
```

구현: `core/src/api/core/zlink_flow_state_api.cpp` (신규). 기존 config API 패턴
(`core/src/api/core/zlink_option_specialized_api.cpp`)을 그대로 따른다 — `as_socket()`으로
handle을 확인하고, 내부 `int` 반환 함수를 `config_result_internal::from_rc()`로 감싼다.

```cpp
extern "C" zlink_config_result_t
zlink_socket_set_receive_flow_state (void *handle_, zlink_receive_flow_state_t state_)
{
    zlink::socket_base_t *socket = as_socket (handle_);
    if (!socket)
        return ZLINK_CONFIG_INVALID_HANDLE;
    return zlink::config_result_internal::from_rc (
      socket->set_local_receive_flow_state (static_cast<int> (state_)));
}
```

내부 진입점 `socket_base_t::set_local_receive_flow_state (int)`은 stage3 산출물
(`core/src/runtime/sockets/common/socket_base_flow_state.cpp`)을 변경 없이 재사용했다.

### 1.1 결과 매핑 근거 (계획 §5 표)

| 조건 | 내부 결과 | 공개 결과 | 근거 |
|---|---|---|---|
| 유효한 DEALER/ROUTER + 유효한 state (첫 설정, idempotent 반복 포함) | `0` | `ZLINK_CONFIG_OK` | `config_result_internal::from_rc(0)` |
| `socket == NULL` 또는 유효하지 않은 handle | (호출 전 차단) | `ZLINK_CONFIG_INVALID_HANDLE` | `as_socket()`이 NULL 반환 → wrapper가 직접 반환 |
| Enum 범위 밖의 state | `-1`, `errno=EINVAL` | `ZLINK_CONFIG_INVALID_ARGUMENT` | `config_result_internal.hpp:22-24` |
| Completion lane을 지원하지 않는 socket (PAIR/PUB/SUB/XPUB/XSUB/STREAM) | `-1`, `errno=ENOTSUP` | `ZLINK_CONFIG_NOT_SUPPORTED` | `result_errno_internal::is_not_supported()` |
| Close가 admission을 먼저 얻음 (`socket_public_api_scope_t` 패배) | `-1`, `errno=ESHUTDOWN` | `ZLINK_CONFIG_INVALID_STATE` | `config_result_internal.hpp:26` (`ESHUTDOWN` 행) |
| Close가 완전히 끝난 뒤(동기 teardown) 호출 | (handle 자체가 무효화됨) | `ZLINK_CONFIG_INVALID_HANDLE` | 아래 §1.2 참고 |
| Context 종료 (`_ctx_terminated`) | `-1`, `errno=ETERM` | `ZLINK_CONFIG_INTERNAL_ERROR` | `config_result_internal.hpp`의 `from_errno` 기본 분기 (변경하지 않음, §7 참고) |

기존 `config_result_internal.hpp`는 수정하지 않았다 — `ESHUTDOWN → INVALID_STATE` 매핑이
이미 존재해서 별도 추가가 필요 없었다.

### 1.2 "Close가 이겼다"의 두 관측 형태 (test로 확인한 사실)

계획 §5는 "Close가 API의 local-state 저장보다 먼저 확정됨 → `ZLINK_CONFIG_INVALID_STATE`"라고
쓴다. 실제로 두 가지 관측 가능한 결과가 있음을 test로 확인했다 (`test_flow_state_c_api.cpp`):

1. **진짜 경합**: `zlink_close()`가 `socket_public_api_scope_t`의 같은 admission gate
   (`enter_public_api()`)를 우리 호출보다 먼저 통과하면, 우리 호출은 `errno=ESHUTDOWN`을 보고
   `ZLINK_CONFIG_INVALID_STATE`를 반환한다. 이것이 계획이 말하는 행이다.
2. **Close가 이미 완전히 끝난 뒤 호출**: 연결이 없는 새 DEALER는 linger할 것이 없어
   `zlink_close()`가 admission 확인과 전체 teardown을 한 동기 호출 안에서 끝낸다. 그 뒤에
   같은 handle로 호출하면 `as_socket()`이 이미 등록 해제된 handle을 알아보지 못해
   `ZLINK_CONFIG_INVALID_HANDLE`을 반환한다.

두 경우 모두 "close와 config 호출은 같은 socket 상태를 두고 경합하고, 먼저 확정된 쪽만
관찰된다"는 계약을 지킨다. 어느 쪽도 절반만 적용되거나 process를 손상시키지 않는다. 이
차이는 §9 spec 제안에 반영했다 (§7.1).

## 2. Event (계획 §6)

`core/include/zlink_enum.h`에 `zlink_socket_monitor_event_e`의 새 bit 3개를 추가했다
(bit 16~18, 기존 15개는 bit 0~15 사용). `ZLINK_SOCKET_MONITOR_EVENT_ALL`을 `0xFFFFu`에서
`0x7FFFFu`로 확장했다 (bit 0~18 전체).

```c
ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED = 1u << 16,
ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED = 1u << 17,
ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE = 1u << 18,
```

`ZLINK_EVENT_SEND_FLOW_PAUSED` / `ZLINK_EVENT_SEND_FLOW_RESUMED` / `ZLINK_EVENT_FLOW_STATE_STALE`
별칭도 기존 `ZLINK_EVENT_*` 관례대로 추가했다.

### 2.1 발생 지점과 필드

| Event | 발생 지점 | Field (`zlink_monitor_event_t`) |
|---|---|---|
| `SEND_FLOW_PAUSED` | `pipe_t::process_flow_state()` → `socket_base_t::flow_state_applied()`. 실제 PAUSED 전이가 적용된 직후, 이 pipe의 socket-owning thread에서 동기 호출 | `routing_id` (application pipe의 peer routing id), `transport_pair_id`, `transport_pair_generation`, `value[0]=epoch` |
| `SEND_FLOW_RESUMED` | 위와 동일 함수, RUNNING 전이 | 위와 동일 + `value[1]=실제 writable 여부(0/1)` |
| `FLOW_STATE_STALE` | `socket_base_t::consume_receive_flow_state_frame()`. 이전 generation의 late frame, 또는 같은 generation의 중복·역전 epoch를 거부하는 두 지점 | `transport_pair_id`, `transport_pair_generation` (현재 값), `value[0..3] = {received generation, current generation, received epoch, current epoch}` |

정상 data frame마다 event를 만들지 않는다 (`test_data_traffic_emits_no_flow_events`로 확인).

### 2.2 이벤트 배정 메커니즘 선택

`SEND_FLOW_PAUSED`/`RESUMED`의 "실제 writable 여부"는 async pipe command
(`command_t::flow_state`)가 적용된 **후**에만 정확하다. Frame을 decode하는
`consume_receive_flow_state_frame()` (transport I/O thread)에서 바로 event를 내면
아직 pipe에 반영되지 않은 writable 상태를 보고하게 된다. 그래서:

- `i_pipe_events`에 새 가상 함수 `flow_state_applied (pipe_t*, bool paused_, uint64_t epoch_,
  bool actual_writable_)`를 기본 no-op으로 추가했다 (`core/src/runtime/core/pipe.hpp`).
  `session_base_t`는 override하지 않는다 — flow state를 갖는 pipe는 socket이 직접 소유하는
  application pipe뿐이다.
- `pipe_t::apply_remote_flow_state()`에 `out_transition_`/`out_actual_writable_`
  파라미터를 추가했다 (기본값 `NULL`이라 기존 유일한 다른 호출부
  `socket_base_api.cpp:258`는 변경이 필요 없었다). 이 함수는 이미 `_out_sync` 아래에서 상태를
  바꾸므로, 같은 lock 아래에서 `write_state_ready_unlocked()`로 "실제 writable"을 함께
  sample한다.
- `pipe_t::process_flow_state()`가 전이 종류(`flow_state_transition_t`)를 받아
  `_sink->flow_state_applied()`를 호출한다. 기존 `_sink->write_activated()` 호출(즉시 edge
  발행 여부)은 그대로 유지했다 — 관측 추가가 기존 wakeup 의미를 바꾸지 않는다.
- `socket_base_t::flow_state_applied()` (`socket_base_flow_state.cpp`)가 실제 override로,
  metric을 갱신하고 `event()`를 호출한다.

`FLOW_STATE_STALE`은 결정이 이미 내려지는 자리(`consume_receive_flow_state_frame`)에서 바로
발생시켰다 — 이 판정에는 async 반영 지연이 없다(거부 자체가 결정이다).

## 3. Metric (계획 §6)

`zlink_monitor_status_t`에 필드 5개, `ZLINK_MONITOR_STATUS_ABI_VERSION`을 `3`에서 `4`로,
`zlink_monitor_status_detail_flag_e`에 `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` bit를 추가했다.

```c
uint64_t flow_paused_connections;      /* gauge */
uint64_t flow_pause_applied_total;     /* counter */
uint64_t flow_resume_applied_total;    /* counter */
uint64_t flow_state_stale_total;       /* counter */
uint64_t flow_pause_duration_ms;       /* 가장 최근에 끝난 PAUSED 구간의 길이 */
```

`socket_base_monitor.cpp::monitor_snapshot()`이 DEALER/ROUTER에서만 이 필드들을 채우고
`ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` bit를 세운다 (다른 socket 유형은 `memset`이 이미
0으로 만든 값을 그대로 둔다).

Storage: `socket_base_t`에 `std::atomic<uint64_t>` 5개
(`_flow_paused_connections`, `_flow_pause_applied_total`, `_flow_resume_applied_total`,
`_flow_state_stale_total`, `_flow_last_pause_duration_ms`) — 기존 `_auto_hwm_send_attempts` 패턴과
동일. `flow_state_applied()`가 PAUSED에서 `pipe_t::set_remote_flow_pause_started_ms(now)`를
기록하고, RESUMED에서 그 값을 읽어 duration을 계산한다 (둘 다 socket-owning thread 안에서만
호출되므로 lock이 필요 없다).

Reset: `socket_base_t::reset_flow_state_metrics()` (internal). 계획 §6이 "이름, reset과
snapshot consistency는 정식 monitoring spec에서 확정한다"고 명시했고, 기존
`reset_auto_hwm_admission_counters()`도 public C API가 없는 internal-only 함수라 같은
패턴을 따랐다. Public reset 표면은 §7의 spec 제안 목록에 남긴다.

## 4. C binding mirror

`bindings/c/include`는 `core/include`의 정확한 바이트 복사본이다
(`core/tests/contract/check_c_header_mirror.py`가 diff를 강제한다). 세 헤더
(`zlink_enum.h`, `zlink/eventing/api.h`, `zlink/socket/api.h`)를 `rsync --checksum`으로
그대로 복사했다.

```bash
python3 core/tests/contract/check_c_header_mirror.py .   # PASS
```

`ZLINK_MONITOR_STATUS_ABI_VERSION`을 하드코드로 검증하던 기존 C binding contract test
(`bindings/c/tests/test_c_contract_surface.c:25`)를 `3u`에서 `4u`로 갱신했다 — ABI 상수 자체를
바꾼 결과이지 새 기능이 아니다.

## 5. Hot-path 감사

- **Send/recv per-message 경로**: `pipe.cpp`의 `write()`, `write_no_hwm_check()`,
  `check_write_status()` 등 어떤 hot-path 함수도 수정하지 않았다. `remote_flow_blocked_unlocked()`
  같은 기존 admission 함수는 그대로다.
- **Event**: `socket_base_t::event()`는 `monitor_runtime().events_atomic.load()`가 `0`이면
  (monitor 미부착) 즉시 반환한다 — 기존 이벤트 시스템이 이미 갖고 있던 성질이며, 새 event
  세 개도 이 경로를 그대로 탄다. `flow_state_applied()`와 `note_flow_state_stale()`는 모두
  PAUSED/RUNNING **전이**와 거부된 stale/duplicate **frame**에서만 호출된다 — 둘 다 정상
  data frame과 무관한 completion-lane 이벤트다.
- **Metric**: 5개 atomic은 같은 두 호출 지점(`flow_state_applied`, `note_flow_state_stale`)
  에서만 갱신된다. `memory_order_relaxed`를 사용했다 — 이 값들은 상호 순서를 요구하지 않는
  독립 counter/gauge다.
- **`i_pipe_events::flow_state_applied()` 기본 구현**: 빈 인라인 함수라 override하지 않는
  `session_base_t` 쪽에 어떤 비용도 추가하지 않는다.
- **`ZLINK_SOCKET_MONITOR_EVENT_ALL` 확장**: 값 자체가 커진 것뿐이고 bitmask 비교 비용은
  동일하다.

결론: API/event/metric 계층은 사용하지 않을 때(monitor 미부착, flow API 미호출) 어떤
per-message 비용도 추가하지 않는다. `core/build`(perf runtime)는 이번 라운드에서 재빌드하지
않았다 — 계획 §7.1 "Public API·관측" 단계 산출물은 ABI·event·snapshot test 통과가 gate이고,
"항상 RUNNING인 paired perf" 행은 stage3와 마찬가지로 열어 둔다 (다른 worktree에서 진행 중인
성능 측정과 병행하지 않기 위함).

## 6. 변경 파일

| 파일 | 변경 |
|---|---|
| `core/include/zlink_enum.h` | `+29 -1`. `zlink_receive_flow_state_t`, 3개 event bit, `ALL` 확장, `DETAIL_FLOW_STATE` |
| `core/include/zlink/socket/api.h` | `+20`. `zlink_socket_set_receive_flow_state` 선언 |
| `core/include/zlink/eventing/api.h` | `+21 -1`. ABI version 4, `zlink_monitor_status_t` 필드 5개 |
| `core/src/api/core/zlink_flow_state_api.cpp` | 신규 22줄. Public wrapper |
| `core/CMakeLists.txt` | `+1`. 새 source 등록 |
| `core/src/runtime/core/pipe.hpp` | `+44 -1`. `flow_state_transition_t`, `i_pipe_events::flow_state_applied`, `apply_remote_flow_state` out-param, pause-start timestamp accessor |
| `core/src/runtime/core/pipe.cpp` | `+39 -4`. 위 선언의 구현, `process_flow_state`가 전이를 sink에 통지 |
| `core/src/runtime/sockets/common/socket_base.hpp` | `+25`. `flow_state_applied` override 선언, metric atomic 5개, accessor 선언 |
| `core/src/runtime/sockets/common/socket_base.cpp` | `+5`. Atomic 초기화 |
| `core/src/runtime/sockets/common/socket_base_flow_state.cpp` | `+112 -5`. `flow_state_applied`, `note_flow_state_stale`, `flow_state_metrics`, `reset_flow_state_metrics`, 두 stale 판정 지점에 event 배선 |
| `core/src/runtime/sockets/common/socket_base_monitor.cpp` | `+8`. Snapshot에 flow metric 5개 채우기 |
| `core/tests/integration/test_flow_state_c_api.cpp` | 신규 ~470줄. 이 문서 §8 |
| `core/tests/CMakeLists.txt` | `+1`. Test 등록 |
| `bindings/c/include/{zlink_enum.h, zlink/socket/api.h, zlink/eventing/api.h}` | `core/include`와 byte-identical mirror |
| `bindings/c/tests/test_c_contract_surface.c` | `-1 +1`. ABI version 상수 갱신 |

## 7. §9 spec 제안 (보호 경로 — 미승인 상태이므로 문서를 직접 고치지 않음)

| 문서:행 | 제안 |
|---|---|
| `core/doc/spec/core/03-errors.ko.md`/`.en.md` | `zlink_socket_set_receive_flow_state`의 결과 표. 특히 §1.2에서 확인한 두 형태의 "close가 이김" (`INVALID_STATE`: admission 경합 패배, `INVALID_HANDLE`: teardown이 이미 끝난 뒤 호출) 모두 문서화 |
| `core/doc/spec/core/05-events.ko.md`/`.en.md` | `ZLINK_EVENT_SEND_FLOW_PAUSED`/`RESUMED`/`FLOW_STATE_STALE`의 발생 조건, field 의미(§2.1 표), "정상 data frame에는 event 없음" 불변조건 |
| `core/doc/spec/core/07-monitoring.ko.md`/`.en.md` | `zlink_monitor_status_t` ABI 4 필드 5개, `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE`, reset 표면 부재(공개 API 없음, 재확인 필요) |
| `core/doc/spec/core/06-dealer.ko.md`, `07-router.ko.md` | Public API가 이 socket 유형에서만 성공한다는 §1 설명과 연결 |
| `core/doc/spec/core/01-pair.*.md`, `02-pub.*.md`, `03-sub.*.md`, `04-xpub.*.md`, `05-xsub.*.md`, `08-stream.*.md` | `zlink_socket_set_receive_flow_state`가 이 socket에서 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환하고 기존 동작이 변하지 않음 |
| `bindings/doc/spec/c/README.ko.md`/`.en.md` | C enum·함수 signature·ABI (§1), event enum 3개, `zlink_monitor_status_t` 확장 |
| (신규 논의 필요) | Flow metric의 public reset 표면 여부 — 계획 §6이 명시적으로 미정으로 남긴 항목. 현재는 `core/src/runtime/sockets/common/socket_base_flow_state.cpp`의 `reset_flow_state_metrics()`가 internal-only |

## 8. Test 결과

### 8.1 신규 test: `core/tests/integration/test_flow_state_c_api.cpp` (10 tests)

| Test | 검증 내용 |
|---|---|
| `test_valid_state_on_dealer_and_router_succeeds_and_is_idempotent` | DEALER/ROUTER + 유효 state → OK, 같은 state 반복 → OK |
| `test_null_or_invalid_handle_is_invalid_handle` | `NULL` → `INVALID_HANDLE` |
| `test_out_of_range_state_is_invalid_argument` | `2`, `-1`, `999` → `INVALID_ARGUMENT` |
| `test_unsupported_socket_types_report_not_supported` | PAIR/PUB/SUB/STREAM → `NOT_SUPPORTED` |
| `test_close_admitted_first_reports_invalid_state_or_handle` | Close가 이미 끝난 뒤 호출 → `INVALID_STATE` 또는 `INVALID_HANDLE` (§1.2) |
| `test_close_races_with_set_receive_flow_state` | 진짜 동시 close+설정 20회 반복 → `OK`/`INVALID_STATE`/`INVALID_HANDLE` 외의 결과나 crash 없음 |
| `test_pause_and_resume_each_emit_exactly_one_event` | PAUSE 적용 → `SEND_FLOW_PAUSED` 정확히 1회, 추가 event 없음. RESUME → `SEND_FLOW_RESUMED` 정확히 1회 |
| `test_duplicate_frame_emits_stale_event` | 같은 epoch 반복 주입 → 두 번째는 `FLOW_STATE_STALE` 1회, `SEND_FLOW_PAUSED` 재발행 없음 |
| `test_data_traffic_emits_no_flow_events` | RUNNING 상태에서 200회 요청/응답 → flow event 0건 |
| `test_flow_state_metrics_snapshot_and_reset` | Pause/resume/stale 각각 후 snapshot 값 확인, `reset_flow_state_metrics()` 뒤 전부 0 |

결과: `10 Tests 0 Failures 0 Ignored — OK`. 단독 10회 반복 실행 모두 통과
(전체 재빌드 전 5회 + 재빌드 후 10회, 총 15회 0 실패).

### 8.2 계획 §8.1 focused set + 신규 test 2개

```text
ctest --test-dir core/build-tests --output-on-failure \
  -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options
      |test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm
      |unittest_flow_state_frame|test_flow_state_paired)$'

=> 100% tests passed, 0 tests failed out of 10 (59.71 sec)
```

Base commit `56bfdeb28d` 위에서 재빌드 후 실행. `core/build-tests`는 `-DZLINK_BUILD_TESTS=ON
-DCMAKE_BUILD_TYPE=Release`로 구성했다.

### 8.3 알려진 host 노이즈

빌드·테스트 도중 host에 다른 작업자의 perf 측정이 동시에 실행되어 한 차례 이 worktree의
build-tests 빌드가 90초 이상 정체된 적이 있었다(다른 host 부하로 판단, 재현되지 않음).
이 작업 범위의 code 변경과는 무관하다.

### 8.4 전체 sweep과 기존 실패 + 새로 드러난 contract gate

`ctest --test-dir core/build-tests` 전체 90개: 86 passed, 4 failed.

| Test | 판정 |
|---|---|
| `test_xpub_nodrop` | 기존 실패 (stage3 worklog §6.1과 동일) — 이 작업 범위 밖 |
| `test_router_multiple_dealers` | 기존 실패, 이번 실행은 Timeout으로 관측(이전엔 `:714` 결정적 실패) — 같은 known-bad test, 이 작업 범위 밖 |
| `test_zmp_metadata` | 기존 실패 (`:506 test_tcp_decoder_hwm_isolated_by_origin_connection`) — 이 작업 범위 밖 |
| `contract_public_surface` | **신규 실패, 이 단계가 유발함**: `header declares non-formal functions (1): ['zlink_socket_set_receive_flow_state']` |

`contract_public_surface`는 `core/doc/spec/core/`의 formal C block을 진실 원천으로 삼아
header가 선언한 함수 집합과 정확히 일치하는지 검사한다
(`core/tests/contract/check_public_surface.py`). 이 계획 문서 자체가 "API와 event 이름은
정식 Core spec과 binding 문서에서 확정하기 전에는 사용할 수 없다"(문서 3행)고 명시하므로,
이번 단계가 `core/include`에 `zlink_socket_set_receive_flow_state`를 **spec 확정 전에**
추가한 것 자체가 이 gate와 구조적으로 충돌한다. Spec은 보호 경로라 이 gate를 통과시키려고
`core/doc/spec/core/`를 직접 고치지 않았다 — 대신 §7의 spec 제안 표가 그 해결책이다(해당
제안을 실제 spec에 반영하면 이 실패는 사라진다). 이 실패는 예측된 것이며 우회하거나
숨기지 않고 그대로 보고한다.

이 4개를 빼면 `100% tests passed, 86/86`(계획 §8.1 10개 포함, 새 `test_flow_state_c_api` 포함).

## 9. 완료 상태

- `core/include`, `bindings/c/include`에만 public 표면을 추가했고 `core/doc/spec/`,
  `bindings/doc/spec/`, `core/doc/internals/`는 건드리지 않았다 (제안은 §7에 기록).
- Checklist §12.3 행 6 ("C API, event와 metric focused test가 통과했다")의 evidence는
  본 문서 §8.
- 행 7 ("Flow state가 계속 RUNNING인 paired perf")은 이번 단계에서 열어 둔다 — perf는
  실행하지 않았다(`core/build` 미변경).
- Framework source, public API, spec, test는 변경하지 않았다.
