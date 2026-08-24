# Monitoring 스펙-구현 gap 감사

> 감사 도구: codex (gpt-5.6, 정적 대조) · 2026-08-24
> 범위: `core/doc/spec/core/06-monitoring.ko.md`와 `core/include/`·`core/src/` 공개 monitor 구현

판정: **구현/문서 gap 3건, 요확인 1건**. 코드·스펙은 수정하지 않았고, 지침에 따라 테스트는 실행하지 않았다.

## 대조 완료 계약군

- 공개 함수 signature, status/event/open-options ABI layout과 enum·flag 값: 일치
- `events == 0` 선택 없음, `EVENT_ALL == 0x7FFFF`, event record의 caller-owned recv 복사: 일치
- `monitor_hwm_bytes == 0`의 overflow-safe `4096 * (internal event + zlink_msg_t)` 기본값과 양수의 양방향 SNDHWM·RCVHWM 적용: 일치
- paired DEALER/ROUTER 전용 receive-flow detail bit, flow counter와 Auto HWM metrics reset의 분리: 일치
- paired transport의 두 lane 준비 후 한 번만 CONNECTION_READY edge를 내는 식별·집계 경로: 일치
- handler callback의 stack-local borrowed event와 `zlink_monitor_ignore_handler`의 no-op 소비 동작: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `06-monitoring.ko.md:42-44`, `460-462`, `484-488`, `561` — handler/recv 중 하나를 활성화한 뒤 다른 mode를 시도하면 `EBUSY` | `core/src/api/monitoring/monitor_api.cpp:193-215`; `core/src/api/monitoring/monitor_query_api.cpp:157-165`; `core/src/api/monitoring/monitor_socket_api.cpp:14-45` | recv 경로는 handler 포인터가 이미 있는지만 검사할 뿐 recv mode를 기록하지 않는다. 따라서 recv를 한 번 이상 성공한 monitor에도 `zlink_socket_monitor_handler`가 handler 포인터가 비어 있으면 성공한다. handler→recv만 `EBUSY`이고 recv→handler 상호 배타 계약은 구현되지 않았다. |
| B. 구현 gap | `06-monitoring.ko.md:122-124`, `566-567` — queue 포화 시 같은 high-frequency event를 aggregate하고 connection/protocol/lifecycle event를 우선 보존하며 aggregate 수를 다음 status에 반영 | `core/src/api/monitoring/monitor_socket_api.cpp:136-144`; `core/src/runtime/sockets/common/socket_base_monitor.cpp:280-282`; `core/src/runtime/sockets/common/socket_monitor_runtime.cpp:243-264`; `core/src/runtime/sockets/common/socket_base_monitor.cpp:37-143` | 공개 open은 event version 3을 고정해 `lossy=true`로 만든다. worker queue가 찬 경우 `enqueue_worker_event`는 어떤 event인지 구별·aggregate·우선 보존·계수도 하지 않고 새 record를 버린다. status 작성 경로에도 aggregate/drop 수를 기록하는 field나 값이 없다. |
| B. 구현 gap | `06-monitoring.ko.md:229-233`, `509-511`, `583` — 한 status 호출의 모든 field는 같은 snapshot 경계에서 읽힘 | `core/src/runtime/sockets/common/socket_base_monitor.cpp:44-113`, `114-140`; `core/src/runtime/sockets/common/socket_base_flow_state.cpp:404-422` | pipe 합계는 `monitor_runtime().sync` 아래에서 읽지만 lock을 푼 뒤 Auto HWM 상태와 개별 relaxed atomic flow counters를 차례로 읽는다. 하나의 snapshot lock/epoch/copy가 없어 pipe 상태·Auto HWM 값·flow counter가 서로 다른 시점의 조합이 될 수 있으므로 단일 snapshot 경계를 보장하지 못한다. |

## 요확인

- `06-monitoring.ko.md:126-127`, `466-467`, `492-493`, `529-530`의 handler·recv·close single-consumer thread 규칙은 API가 호출자 의무를 서술한 것인지, 동시 recv/close 자체를 Core가 검출·직렬화해야 한다는 보장인지 정적 코드만으로 확정할 수 없다. 현재 `require_monitor_recv_model`은 handler 등록만 검사하고 동시 recv owner를 기록하지 않으며, close 경로는 registry pin으로 handler-state lifetime만 보호한다(`core/src/api/monitoring/monitor_api.cpp:193-215,403-499`). 동시 recv/close contract test 또는 명시적 소유권 문구가 필요하다.
