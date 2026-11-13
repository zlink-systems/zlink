# #4143 요청 할당 증거

Linux 실행은 수행하지 않았다. `/root/jobs/fx/cx/v21/s033/core124b/verdict.md`가 조사 4시간 경과 시점에도 없었다. 지시된 gate 조건을 지키기 위해 빌드·테스트·lock을 시작하지 않았다. 아래 Linux 항목은 미측정이며 추정하지 않았다.

## 1. Windows: Local Actor 샘플

Windows 로그 `win-run42640081756.log`의 이 샘플은 10건, 크기 합계 5,624 B다. 로그에서 각 이벤트의 첫 Framework 프레임과 소유 객체를 적었다.

| 번호 | 크기 | 첫 Framework 프레임 | 소유 객체와 의미 |
|---|---:|---|---|
| 0 | 2,280 B | `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:717` | `receiver_state_t` 공유 상태(`make_shared`) |
| 1 | 88 B | `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:742` | `operation.receiver`의 완료 callback `std::function` 대상. MSVC `functional`의 `_Reset` 프레임도 있음 |
| 2 | 2,192 B | `framework/src/runtime/stateful/public_host_runtime.cpp:8850` | `operation_completion_t` 공유 객체 |
| 3 | 240 B | `framework/src/runtime/stateful/public_host_runtime.cpp:8859` | `local_completion_sink_t` 공유 객체 |
| 4 | 328 B | `framework/src/runtime/foundation/operation_registry.cpp:402` | `operation_completion_item_t` 등록 객체 |
| 5 | 152 B | `framework/src/runtime/foundation/operation_registry.cpp:423` | `_entries`의 operation registry 노드 |
| 6 | 64 B | `framework/src/runtime/execution/state_lane.cpp:129` | `_mailbox` deque의 MSVC map 저장소 성장 (`_Growmap`) |
| 7 | 128 B | `framework/src/runtime/execution/state_lane.cpp:129` | `_mailbox` deque의 원소 블록 할당 (`_Emplace_back_internal`) |
| 8 | 64 B | `framework/src/runtime/dispatch/offload_executor.cpp:105` | `_queue`의 기본 deque map 저장소 성장 (`_Growmap`) |
| 9 | 88 B | `framework/src/runtime/dispatch/offload_executor.cpp:105` | `_queue`의 원소 블록 할당 (`_Emplace_back_internal`) |

MSVC 로그의 #6–7은 `std::deque<state_lane_t::mailbox_item_t>::push_back`; #8–9는 `std::queue<offload_executor_t::work_item_t>::push`에서 각각 발생한다. 소스 소유점은 각각 `state_lane_t::_mailbox` 선언 `state_lane.hpp:534`, `offload_executor_t::_queue` 선언 `offload_executor.hpp:119`다. 두 쌍은 매 push마다 map과 block을 새로 만드는 것으로 확인된 것이 아니라, 이 실행에서 빈 컨테이너가 처음 커질 때 발생한 저장소 할당이다. 컨테이너 수명과 pop 이후 재사용 여부에 따른 반복 빈도는 Linux 관찰 없이 단정하지 않는다.

## 2. Windows: Node 샘플과 기준 샘플

실제 node 요청은 9건이다.

| 번호 | 크기 | 첫 Framework 프레임 | 소유 객체와 의미 |
|---|---:|---|---|
| 0 | 2,064 B | `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:717` | `receiver_state_t` 공유 상태 |
| 1 | 96 B | `framework/include/zlink/framework/contracts/channels/channel.hpp:920` | `request_to_node`에서 erased submit으로 전달되는 metadata map의 MSVC tree head/sentinel |
| 2 | 96 B | `framework/src/runtime/messaging/client_call_codec.cpp:104` | `create_envelope`의 `envelope_header_t` map tree head/sentinel |
| 3 | 96 B | `framework/src/runtime/channels/channel_runtime.cpp:1880` | encode 요청 호출 경계에서 전달되는 envelope map tree head/sentinel |
| 4 | 96 B | `framework/src/runtime/messaging/client_call_codec.cpp:90` | `encode_service_payload_parts` 경계의 envelope map tree head/sentinel |
| 5 | 96 B | `framework/src/runtime/messaging/envelope_codec.cpp:589` | deferred encoder closure의 header map tree head/sentinel |
| 6 | 640 B | `framework/src/runtime/messaging/envelope_codec.cpp:579` | 반환되는 deferred encoder `std::function`의 heap 대상 |
| 7 | 96 B | `framework/src/runtime/messaging/envelope_codec.cpp:589` | 같은 closure 생성·전달 구간의 추가 map tree head/sentinel |
| 8 | 104 B | `framework/include/zlink/framework/contracts/dispatch/task.hpp:799` | terminal callback 등록을 위한 task shared state |

각 96 B map 이벤트에는 MSVC `std::_Tree_node::_Buyheadnode` (`xtree:345`), `std::_Alloc_sentinel_and_proxy` (`xtree:1957`) 또는 tree 생성 프레임이 있다. `client_call_codec.cpp:102-103`의 주석도 MSVC에서 이동한 `std::map`이 새 head node를 할당한다고 명시한다. 이 로그만으로 각 MSVC STL 내부 이동/대입 구현의 세부 규칙 전체를 검증할 수는 없지만, 할당이 tree head/sentinel 계열이라는 사실과 호출 경계는 프레임에서 확인된다. 640 B는 MSVC `std::function`의 `_Global_new<_Func_impl_no_alloc>` (`functional:843`) 및 `_Reset` (`functional:975`) 경로다. 이 프레임은 대상이 인라인 저장되지 않고 heap 대상이 됐음을 보여 준다.

node 테스트가 빼는 기준 샘플의 Windows 이벤트는 다음과 같다.

| 샘플 | 번호 | 크기 | 첫 Framework 프레임 | 소유 객체 |
|---|---:|---:|---|---|
| encode 기준 | 0 | 96 B | `framework/src/runtime/messaging/client_call_codec.cpp:104` | envelope header map tree head/sentinel |
| encode 기준 | 1 | 144 B | `framework/src/runtime/messaging/envelope_codec.cpp:28` | `message_parts_t`의 `std::vector<message_t>` reserve 저장소 |
| target 기준 | — | 0 B | — | 할당 없음 |
| receiver 기준 | 0 | 2,064 B | `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:717` | `receiver_state_t` 공유 상태 |
| receiver 기준 | 1 | 104 B | `framework/include/zlink/framework/contracts/dispatch/task.hpp:799` | terminal callback task state |

## 3. Linux 이벤트와 Windows/Linux 대응

Linux 이벤트 목록은 gate 파일 부재로 측정하지 않았다. 이에 따라 libstdc++ header의 실제 소스 동작과 Linux의 deque/map/`std::function` 할당 건수도 이 보고서에서 인용하거나 추론하지 않는다. 사용자 지시에 따라 Linux Release 단일 타깃 실행과 event dump가 필요하다.

현재 확인 가능한 Windows 전용 후보는 Local Actor의 deque 저장소 4건(64, 128, 64, 88 B)과 node의 map head/sentinel 6건(각 96 B), deferred encoder function 대상 640 B다. Linux에서도 같은 할당이 발생하는지, 생성 시점이 측정 구간 안팎으로 달라지는지는 아직 미확정이다. Windows map 이벤트의 MSVC 근거는 `xtree`와 `map` 프레임 및 위 소스의 명시 주석이다. Linux 표준 라이브러리 소스 대조는 실행이 허용된 뒤에 해야 한다.

## 4. Node의 9 대 4와 기준 민감도

Windows에서 실제 9건은 receiver 기준의 2,064 B와 104 B 두 이벤트, map head/sentinel 96 B 여섯 건, `std::function` heap 대상 640 B 한 건이다. 비교 기준 4건은 encode 기준의 96 B 및 144 B와 receiver 기준 두 건이다. 따라서 실제 경로와 같은 종류의 map 이벤트 하나 및 receiver 2건은 기준 종류와 겹친다. 실제 경로에 추가된 map 이벤트 다섯 건과 640 B 한 건이 있고, 반대로 encode 기준의 144 B vector reserve는 실제 deferred 경로에 없다. 결과적으로 `9 - 4 = 5`다.

baseline 방법은 플랫폼과 경로에 민감하다. 실제 경로는 `encode_service_payload_parts`가 deferred envelope encoder를 반환하지만, encode 기준은 eager `encode_payload_parts`를 통해 `message_parts_t` vector를 만든다. Windows에서는 map tree head가 이동 경계마다 할당되고, eager 기준의 vector reserve 1건은 실제 경로에 없다. Linux 측정 없이 두 플랫폼의 차이 크기는 확정할 수 없다.

## 5. 결정 소유자와 가장 작은 제거 방향

| 이벤트 | 소유자 | 호출당 비용인가 | 가장 작은 제품 변경 방향 |
|---|---|---|---|
| Local #6–7 | `state_lane_t::_mailbox`, `state_lane.cpp:129` | deque 수명 중 성장 비용. 이 push가 처음 성장을 유발했음. 매 호출 비용인지는 미확정 | 필요하면 deque 대신 사전 용량을 가진 고정 ring buffer로 mailbox 저장소 할당 제거 |
| Local #8–9 | `offload_executor_t::_queue`, `offload_executor.cpp:105` | queue 수명 중 성장 비용. 이 push가 처음 성장을 유발했음. 매 호출 비용인지는 미확정 | 필요하면 queue의 deque를 사전 용량 고정 ring buffer로 교체 |
| Node #1–5, #7 | metadata 전달 및 envelope header 경계. 첫 프레임 위치는 표에 기록 | MSVC tree head/sentinel 이동 경계마다 관측된 비용. 모든 호출에서의 정확한 횟수는 해당 경로 기준 | header/metadata map의 불필요한 값 복사·이동 경계를 제거하고 한 소유 map을 재사용. 우선 후보는 metadata by-value erased submit 인자(`channel.hpp:1020-1026`, `channel_runtime.cpp:1729-1738`)와 header 전달/반환 체인이다 |
| Node #6 | `envelope_codec_t::prepare_service_payload`, `envelope_codec.cpp:579` | deferred encoder가 만들어질 때 1회 | closure capture를 작게 만들어 MSVC `std::function`의 inline buffer에 들어가게 하거나, 기존 소유 구조를 재사용. 실제 capture 크기/버퍼 한계는 Linux 대조와 MSVC header 근거 확인 전 확정하지 않음 |
| Node #0, #8 및 Local #0–5 | request receiver, host completion, operation registry의 각 프레임 소유 객체 | 요청·등록·registry 삽입 때 실행되는 명시적 객체/노드 할당 | 이 증거만으로 test limit 변경이라고 판정하지 않음. 각 owner의 제품 비용이며 별도 제거 설계는 이 작업에서 정하지 않음 |

Deque 및 map의 가장 작은 후보는 코드 변경안이지 승인된 설계가 아니다. 제품 변경은 하지 않았다.

## 6. 예방 제안

PR에서 allocation count limit 또는 해당 경로를 바꿀 때 `.github/workflows/framework-cpp.yml`의 `framework-cpp-windows` (`win-x64 / Framework C++ unit and contract tests`) job을 PR check로 실행한다. 현재 workflow의 일반 pull_request trigger는 주석 처리되어 있고 Windows job은 schedule/manual/release 용도이므로, allocation 테스트 파일과 request 경로 소스가 바뀐 PR을 paths 조건으로 자동 트리거하도록 연결해야 한다. check는 두 대상 gtest만 실행하고 각 샘플의 건수·크기·첫 Framework frame을 요약해 보여야 한다. limit assertion이 통과해도 Windows sample event summary를 남겨 신규 이벤트를 리뷰에서 볼 수 있게 해야 한다.

## 7. 리팩토링 점검

request submit 경로에서 map 소유 경계가 겹친다. `route_client_t::request_to_node`는 `channel.hpp:920-922`에서 metadata를 값 인자로 넘기고, erased API도 `channel.hpp:1020-1026`에서 map을 값으로 받는다. `channel_runtime.cpp:1861`은 metadata를 header로 이동시키며, header는 `envelope_codec.hpp:145-157`의 `envelope_header_t::metadata`에 별도 map을 보유한다. 뒤이어 `create_envelope` (`client_call_codec.cpp:94-104`), `encode_service_payload_parts` (`client_call_codec.cpp:83-90`), `prepare_service_payload` (`envelope_codec.cpp:565-589`)를 지나면서 header map 경계가 반복된다. 로그의 96 B 이벤트는 이 중복 소유/이동 경로와 맞는다.

submit wrappers는 `route_client_t::request_to_node` lambda (`channel.hpp:903-923`), `channel_request_call_t::start/async` (`call.hpp:181-208`), `submit_request_reply_message_erased` (`channel_runtime.cpp:1729-1900`), encoder 함수 포인터 호출 (`channel_runtime.cpp:1880-1883`), `encode_service_payload_parts`, deferred `std::function` 생성 (`envelope_codec.cpp:579`) 순으로 이어진다. 이 wrappers 자체가 전부 불필요하다고 단정하지 않는다. 확인된 중복은 map 소유 경계이며, 각 wrapper 책임을 소유하는 자료가 이번 gate 때문에 아직 확인되지 않았다.
