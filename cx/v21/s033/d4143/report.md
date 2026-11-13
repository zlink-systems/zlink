# #4143 설계 조사

## 결과

- 코드와 테스트는 수정하지 않았다. worktree는 `HEAD (no branch)`이며 시작 시 변경 파일이 없었다.
- Windows attribution은 `/root/jobs/fx/cx/v21/s033/w4143c/report.md`의 이벤트 표를 사용했다. issue의 마지막 두 댓글은 요청 본문에 실제로 포함되지 않았지만, 본문에 있는 PM 방향은 반영했다.
- Linux 수치는 미측정이다. 지정 configure가 현재 main의 C++ binding `1.24.0`과 지정 Core prefix `1.23.0` 불일치로 실패했다. 1.24.0 prefix도 `/root/.cache/zlink/core`에 없다. 다른 Core 버전으로 바꿔 측정하지 않았다.

## 판정 위치(전/후 수)

`envelope_header_t`와 metadata는 wire 내용과 불변 metadata snapshot을 표현한다. payload 소유권의 중복을 없애고 실행 중인 encoder가 참조할 소유자를 하나로 만드는 설계는 `05-payload-ownership-and-codec.ko.md:32-43`, `08-messaging-hot-path.ko.md:96-100`, Channel Messaging의 Application metadata 절(`02-channel-messaging.ko.md:617-690`)을 따른다. registration 경로와 실행 순서는 `08-messaging-hot-path.ko.md:98-100`, state lane의 mutable collection 소유는 `06-state-ownership-and-lanes.ko.md:16-19, 30-44`가 규정한다. 이 조항들은 map 전달 ABI, `std::map` 이동 횟수 또는 deque map/block 성장 규칙을 직접 정하지 않는다. deque 저장소 생성 시점은 현재 `state_lane.hpp:534`, `offload_executor.hpp:119`의 컨테이너 선언과 해당 push 위치가 결정한다.

| 결정/할당 | 현재 결정 위치(file:line) | 현재 판정자 수 | 변경 후 단일 owner 및 위치 | 변경 후 수 |
|---|---|---:|---|---:|
| metadata map을 erased submit 경계에서 값으로 복사 | `channel.hpp:920-922, 1020-1027`; `channel_runtime.cpp:1729-1737` | 3개 경계 | request의 deferred envelope owner가 metadata를 보유. `channel.hpp`에서 참조 전달, owner 생성 시 map을 복사/이동하지 않고 한 번만 소유하도록 구성 | 1 |
| service header map의 값 이동(4회, Windows attribution) | `channel_runtime.cpp:1880-1883`; `client_call_codec.cpp:83-91`; `envelope_codec.cpp:565-570, 579-580` | 4개 경계 | deferred encoder가 소유하는 안정 주소의 envelope state. header/map은 그 state 안에서 만들고 encoder는 state 포인터만 캡처 | 1 |
| mailbox deque map/block의 생성 시점 | `_mailbox.push_back`: `state_lane.cpp:121-130`; 소유 컨테이너: `state_lane.hpp:532-535` | 1 | 컨테이너 owner는 그대로. 측정 fixture에서 cold/warm 조건을 양쪽 샘플에 일치 | 1 |
| executor deque map/block의 생성 시점 | `_queue.push`: `offload_executor.cpp:95-107`; 소유 컨테이너: `offload_executor.hpp:116-120` | 1 | 컨테이너 owner는 그대로. 측정 fixture에서 cold/warm 조건을 양쪽 샘플에 일치 | 1 |

위 수는 allocation-owning 경계를 세었다. metadata의 현재 값 인자 선언/정의/호출은 세 곳이고 header 이동은 네 경계다. 전후 수는 **총 9 → 4 판정 경계**(header 4→1, metadata 3→1, 두 deque 1+1 유지)다. deque 컨테이너의 성능 개선은 제안하지 않는다.

## 변경안(파일:줄별)

- `framework/include/zlink/framework/contracts/channels/channel.hpp:920-922, 1020-1027`: erased submit metadata 인자를 값에서 `const metadata_map_t&`로 바꾸고, call metadata가 request 생명보다 짧게 살지 않도록 deferred envelope owner로 수명을 넘긴다. 현재 `channel_request_call_t::start()`가 `_metadata`를 callback에 넘기므로(`call.hpp:195-209`) call 객체가 임시일 수 있어 이 참조를 encoder가 직접 보관하면 안 된다.
- `framework/src/runtime/channels/channel_runtime.cpp:1729-1737, 1857-1883`: 값 인자를 없애고 header를 deferred state에 직접 구성한다. payload encode와 `parts` 생성은 계속 첫 suspension 전에 한다. 현재 metadata는 `header.metadata = std::move(metadata)`에서 header로 이동한다(`channel_runtime.cpp:1861`). 설계에서는 이 전달을 request owner의 metadata 소유로 합쳐 값 복사 경계를 없앤다.
- `framework/src/runtime/messaging/client_call_codec.hpp/.cpp:83-91`: `encode_service_payload_parts(envelope_header_t, ...)` 값 전달을 owner/state 참조 전달로 바꾼다. owner 안의 header를 직접 갱신하므로 `std::move(header)`를 다음 계층으로 전달하지 않는다.
- `framework/src/runtime/messaging/envelope_codec.hpp/.cpp:258, 565-589`: `prepare_service_payload(envelope_header_t, ...)`와 `[header = std::move(header)]` 캡처를 없앤다. body/type/channel/header를 소유하는 deferred state를 만든 뒤 encoder는 그 state 주소를 캡처한다. `message_parts_t`가 state 수명을 보장해야 한다. 별도 캡처 포인터만 두고 현재 stack의 `header`를 참조하는 안은 금지한다.
- 수명 근거: 현재 submit은 `parts`를 만든 다음 `(*mesh)->request(..., std::move(parts), ...)`에 전달한다(`channel_runtime.cpp:1880-1900`). `mesh_node_runtime.cpp:4425-4427`에서 service payload가 transport에 넘어가며, `raw_mesh_node_owner.cpp:1365-1373` 이후 wire materialization 시 `service_wire_codec.cpp:3224-3228`가 encoder를 호출한다. 따라서 encoder 호출은 submit frame이 반환된 뒤일 수 있다. `message_parts_t`/transport가 보유하는 owner가 유일하게 안전한 수명 경계다.
- 정확한 metadata 저장 변경은 `channel_request_call_t` 재사용 후에도 metadata를 보존해야 하는지 public C++ 계약을 확인한 뒤 선택해야 한다. 현재 C++ header는 `async()`를 다시 호출할 수 있는 형태지만 명시적 one-shot 계약은 이 조사에서 찾지 못했다. 소비성 이동은 승인된 설계로 간주하지 않는다.
- Registration 테스트(`test_cpp_framework_request_call_allocations.cpp:185-230`): 고정 `EXPECT_LE(sample.total, 8)` 대신 cold 초기화 이벤트를 동일하게 포함한 paired reference/target 측정으로 바꾸는 안을 제안한다. MSVC deque는 map과 요소 block을 따로 할당한다. 두 타입은 16바이트보다 커 한 block당 원소가 하나인 크기다. 첫 enqueue는 map과 block을 만들지만, 마지막 원소가 빠지면 block은 해제될 수 있고 map은 남을 수 있으므로 **warm-up 한 번만으로 다음 표본의 block 할당이 사라진다고 보장할 수 없다**. 같은 초기 상태의 두 host/queue 표본을 비교하거나 각 표본에 동일한 enqueue/dequeue 수를 주어야 한다. 크기와 실제 해제 조건은 attribution 로그의 MSVC STL 버전으로 재확인해야 한다.

## 테스트 변경안

- Node 기준 표본은 기존 eager `encode_payload_parts`(현재 test file `:709-722`)가 아니라, request와 같은 `create_envelope` → metadata 설정 → serializer payload → `encode_service_payload_parts` 호출을 측정한다. 즉 `client_call_codec.cpp:83-91`의 #4014 service encoder 경로를 기준으로 삼는다.
- node assertion은 `actual.total == real_service_encoder_reference.total + target_owner.total + receiver.total`을 유지한다. 이 표본은 eager vector reserve를 실제 요청의 deferred encoder 비용으로 오인하지 않는다.
- Linux libstdc++에서 `std::map` 이동이 할당을 만들지 않으면 Linux는 Windows의 MSVC map-head allocation 실패를 재현하지 못한다. 이 경우 Linux assertion은 실제 경로와 새 기준 표본의 equality만 검증하고, MSVC의 네 이동 allocation 제거를 증명하는 수치가 아니다. OS 독립 테스트를 유지하되 해당 회귀 증거는 Windows STL 실행 로그가 필요하다.
- Registration도 Linux에서 기존 8-call 상한을 넘어 실패한다고 단정할 근거가 없다. 지정 환경에서 실행하지 못해 Linux 수치와 실패 여부는 미확정이다. paired 조건에서 어떤 정상 경로 비용을 기준선으로 삼을지는 구현 전에 테스트 설계 승인 필요.

## 검증

- preset 확인: `framework/languages/cpp/CMakePresets.json:109-120`에 `linux-ninja-release`가 있다. 실행 위치는 `framework/languages/cpp`.
- configure 명령은 요청에 적힌 그대로 실행했다. 로그: `/root/jobs/fx/cx/v21/s033/d4143/configure.log`. 실패 요약: `Core release prefix version 1.23.0 does not match required Core 1.24.0 for zlink_cpp 1.24.0` (`bindings/cpp/CMakeLists.txt:135`).
- 지정된 `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build ... -j3 --target test_cpp_framework_request_call_allocations` 명령은 호출했지만 build-slot의 MemAvailable ≥ 12 GiB gate에서 대기했다. configure 실패로 빌드가 불가능해 대기 프로세스를 종료했다. `/root/jobs/fx/cx/v21/s033/d4143/build.log`는 비어 있으며 test 바이너리와 sample별 Linux count/size는 생성·수집되지 않았다. 작업 build dir는 삭제했다.
- 원시 Windows 근거: `/root/jobs/fx/cx/v21/s033/w4143c/report.md`. node는 metadata 1건 및 header map 이동 4건, local registration은 두 deque의 cold map/block 합계 4건으로 attribution되어 있다.

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 2건(요청 경로 map owner와 allocation-test 기준 경로; 설계 승인 후 같은 작업에서 처리).

## 남은 문제

- 필수 Linux 수치 없음: 현재 main이 요구하는 Core 1.24.0이 지정 캐시에 없어 빌드가 막혔다. 정확한 1.24.0 Linux Core prefix가 제공되면 동일 명령에서 그 prefix만 지정하도록 지시를 갱신해야 한다.
- issue 마지막 두 댓글은 사용자 본문에 첨부되지 않았다.
- supervisor 승인 전에는 제품/test 수정 및 구현을 진행하지 않는다.
