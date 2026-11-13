추천은 **binding을 통해 실제 native admission을 먼저 확정하고, 이미 수락한 요청의 payload 구성은 기존 실행 자원에서 배치 처리하는 구조**다. Framework에 submit queue를 하나 더 만드는 안은 기각한다. 다만 현재 public binding에는 이 admission 분리 기능이 없다. 따라서 추천은 하위 계층 기능과 계약 변경을 사용자가 선택한다는 조건부이며, 현행 기능만으로 이번 요구를 충족했다고 판정할 설계는 없다.

| 새 caller 경로의 순서 | caller 합격 예산, µs/request | caller 밖에서 할 일과 비용 | 없애거나 bind 때 끝낼 일 |
|---|---:|---|---|
| 1. 준비된 route/codec 참조를 읽고 최초 절대 deadline·flow·ambient context를 캡처한다 | 0.30 | 정적 plan 준비는 bind/첫 사용 때 한다. 동적 target 수락은 여기서 대신 판정하지 않는다 | transport 이름 조회, declared-type codec 선택, 정적 header fragment 준비의 요청별 반복 |
| 2. 소유한 typed 요청과 metadata를 인계할 수 있게 하고, admission에 필요한 정확한 크기·유효성을 확정한다 | 0.25 | typed encode·동적 JSON 작성·service payload 구성은 준비 작업을 맡은 자원으로 옮긴다. 기존 encoding 분류 2.12는 노출 비용이며 이동 뒤 비용의 측정값은 아니다 | serializer를 선택하는 일과 실제 encode를 호출 계층마다 다시 연결하는 작업 |
| 3. 같은 operation owner turn에서 최종 receiver·full OperationId·ReplyRouteId·C1·C2·dispatcher 자리를 등록한다 | 0.55 | C1/C2 등록 자체는 옮기지 않는다. typed reply 변환과 자원 정리는 기존 완료 자원에 둔다 | 등록용 별도 결과/관찰 연결. 단, ccall2에서 이미 제거한 부분을 새 절감으로 계산하지 않는다 |
| 4. public binding으로 실제 native admission을 한 번 시도한다 | 3.70 | 수락된 record의 최종 byte 구성·공개에 추가 비용 G가 생긴다. G는 미측정이며 0으로 두지 않는다. native pending·WRITABLE·reply matching은 기존 binding/Core 소유다 | Framework의 물리 전송·재제출 결정은 추가하지 않는다 |
| 5. 같은 binding operation의 준비 작업을 기존 실행 자원에 공개한다 | 0.30 | 받는 쪽의 인계·깨우기 비용은 H다. H와 batch 크기는 미측정이다. 새 Framework send queue를 두지 않는다 | 요청마다 별도 submit worker를 깨우는 경로를 남기지 않는다 |
| 6. 진짜 admission 결과를 기존 public admitted에 연결하고 반환한다 | 0.29 | binding completion은 기존 단일 poller owner가 연속 drain한다. C1 take 뒤 기존 dispatcher의 독립 turn에서 typed 결과를 전달한다 | bool task → submit-result task → admission observer의 내부 변환 chain |
| 7. application/benchmark의 생성·관찰·검증·회수 비용을 남긴다 | 1.11 | application이 선택한 소비 위치를 바꾸지 않는다. 별도 completion 비용은 아래 전체 CPU 회계에 남긴다 | benchmark 검증을 다른 thread로 옮겨 caller 절감으로 보고하지 않는다 |
| **합계** | **6.50** | **W + G + H는 별도로 지불한다. 기존 receive·completion·I/O 비용도 남는다** | **배치로 사라지는 비용과 위치만 바뀌는 비용을 구분한다** |

이 표는 구현의 합격 예산이다. 함수별 측정값을 더해 얻은 예상 성능이 아니다. 특히 0.25의 정확한 크기 확정, 0.55의 등록, 3.70의 새 admission 호출, 0.30의 인계, 1.11 안에 남겨야 하는 application 관찰 비용은 입증되지 않았다. 기존 binding caller 전체 비용을 새 API 한 호출의 실측 시간으로 대입한 것도 아니다. binding 호출을 포함한 caller 예산을 명시하기 위해 3.70을 배정했다. 이 항목들이 성립하지 않으면 추천안도 기각한다. Framework÷binding 0.6은 유지하며 caller 예산 충족만으로 throughput 달성을 선언하지 않는다.

상단 표를 기존 배타적 분류에 맞춰 보이면 다음과 같다. 이 표는 아직 설명하지 못한 감축을 드러내기 위한 것이다. 추가 binding 기능 하나로 나머지 단계가 자동으로 없어지는 것은 아니다.

| 비용 분류 | ccall2 기준 | 새 caller 예산 | caller에서 빠져야 하는 차이 | 근거의 상태 |
|---|---:|---:|---:|---|
| encoding | 2.12 | 0.25 | 1.87 | encode 전체의 이동이 핵심이다. sizing 0.25가 별도 encode와 같은 비용이면 성립하지 않는다 |
| 등록·owner·wrapper | 5.76 | 0.85 | 4.91 | clock/owner/C1/C2에 0.30+0.55를 남긴다. 나머지 중 무엇이 worker 준비인지, 무엇이 정적 작업/삭제인지 독립 µs 분해가 없다 |
| 결과 준비·관찰 | 2.76 | 0.29 | 2.47 | final receiver와 실제 application 관찰은 남는다. 아래 기타 예산에 담을 application 비용과 이중 계산하지 않아야 한다. ccall2의 실패한 5.38 감축 가정을 재사용할 근거가 없다 |
| binding 분류 | 5.00 | 3.70 | 1.30 | 새 reservation의 실측은 없다. 현재 stage 차이를 native 호출 하나의 제거로 해석하지 않는다 |
| 기타/application 잔여 | 0.97 | 1.11 | −0.14 | 새로운 harness 변경 없이 남길 예산이다 |
| 새 caller 쪽 인계 | 기존 단계에 포함 | 0.30 | −0.30 | 추가 비용이다. 기존 단계의 얼마를 대체하는지 확인해야 한다 |
| **합계** | **16.61** | **6.50** | **10.11** | **전액 삭제도, 전액 이동도 입증되지 않았다** |

특히 R의 4.91과 O의 2.47을 allocator 교체 효과로 채우면 반려된 안을 반복하는 것이다. bind 시점의 불변 경로 연결과 요청 뒤 준비 작업으로 이 둘을 구분해야 한다. 현 자료가 그 구분의 시간을 제공하지 않는다는 점은 설계의 미해결 항목이다.

**자료와 판정 범위**

`F/`는 `/root/worktree/zlink-cdes/framework/languages/cpp/framework/`, `B/`는 같은 worktree의 `bindings/cpp/`, `S/`는 `framework/doc/framework/common/spec/server/`, `bench/`는 `framework/bench/grpc/cpp/`다. 아래 source 위치는 이 worktree에서 직접 읽었다. 기준 revision `main 2f8b8e635a`는 사용자 지정값이며 git으로 재확인하지 않았다. S01은 `S/01-execution/01-submit-and-completion.ko.md`, S05는 같은 디렉터리의 `05-payload-ownership-and-codec.ko.md`, S08은 `08-messaging-hot-path.ko.md`, lane은 `06-state-ownership-and-lanes.ko.md`다. binding async model은 `bindings/doc/spec/async-execution-model.ko.md`다. 단계 분류의 D(binding 기능)와 변경 분류의 D(spec gap)는 각각의 표 안에서만 사용한다.

ccall·ccall2·cbind·ccut·coreio·cthin의 보고서를 읽었다. 기존 측정을 다시 실행하지 않았다. ccall의 함수 시간은 profile 표본이고, 단계 시간은 기준 caller CPU로 정규화한 값이다. ccall2도 다른 실행의 profile 비율을 무프로파일 caller CPU에 정규화했다. 따라서 두 보고서의 차이는 승인 가능한 감축량을 보장하지 않는다. 현재 source의 receiver는 `request_task.hpp:717`의 `make_shared` 구조다. 반려된 PR 1의 task-owner 심볼이 현재 main의 구현이라고 쓰지 않는다. 아래의 16.61 기준은 사용자가 지정한 비용 기준으로 사용한다.

내부 문서 준비 스크립트는 git pull/clone과 파일 복사를 실행하므로 이 작업에서는 실행하지 않았다. 기존 `/root/zlink-docs/zlink/AGENTS.md`를 읽었으며, 단독 작업·읽기 전용이라는 이번 지시를 우선했다. 워크트리 수정, build, runtime test, 신규 측정, git/GitHub 작업, 다른 agent/model 호출은 하지 않았다. `core/include`는 읽지 않았다. 쓰는 산출물은 이 보고서뿐이다.

**현행 caller 단계의 분류**

A는 현행 계약에서 submit 반환 전에 끝내야 하는 작업이다. B는 본래 완료 자원에서 해야 하거나, admission에 필요한 최소 부분을 분리한 뒤 옮길 수 있는 작업이다. C는 없애거나 bind/route/첫 사용 때 끝낼 수 있는 반복이다. D는 binding/Core가 이미 소유하여 재사용해야 하는 작업이다. 한 함수 안에 다른 성질의 일이 섞이면 행을 나눴다. D가 동기 admission에도 필요하면 A/D로 적었다. 추천안의 계약 변경을 승인하지 않은 상태에서 A를 B로 취급하지 않는다.

| 배타적 비용 분류 | ccall, µs/request | ccall2, µs/request | 아래 세부 행의 비용 표기 |
|---|---:|---:|---|
| E: encoding | 2.15 | 2.12 | E 안에 포함한다 |
| R: 등록·owner turn·readiness·일부 wrapper | 4.42 | 5.76 | R 안에 포함한다 |
| O: 결과 준비·관찰·task·일부 harness 소비 | 4.25 | 2.76 | O 안에 포함한다 |
| N: binding/Core로 분류된 caller 작업 | 5.54 | 5.00 | N 안에 포함한다 |
| X: 기타·harness·분류하지 못한 작업 | 1.11 | 0.97 | X 안에 포함한다 |

이 다섯 분류만 서로 배타적이다. 아래의 self/inclusive 값은 ccall 또는 ccall2가 따로 보고한 함수 표본이다. inclusive는 하위 호출과 겹치므로 합산하지 않는다. 독립 시간이 없는 행에 5.76이나 2.76을 반복 배분해 가짜 정밀도를 만들지 않았다. ccut이 ccall 표본을 재분류한 시간은 참고 근거로만 사용한다.

| 순서와 현재 요청별 작업 | 현재 source, file:line | µs 근거 | 분류와 처리 |
|---|---|---|---|
| payload를 만들고 call이 typed 값을 소유한다 | `bench/client/bench_cpp_client.cpp:929`, `F/include/zlink/framework/contracts/channels/channel.hpp:897–921` | X/O에 분산. ccall2 `run_tasks` self 0.9848, inclusive 12.7353 전체와 같지 않다 | A. application 입력 생성은 남긴다. call이 이미 값을 소유하므로 background encode를 위해 원본 application 객체를 빌릴 이유는 없다 |
| public call을 시작하고 최종 typed decoder를 연결한다 | `F/include/zlink/framework/contracts/channels/call.hpp:195–212` | O 안의 일부, 독립 시간 없음 | A. 최종 수신자 준비는 S01:331·424–428이 요구한다. 실제 typed decode는 B다 |
| admitted·public reply·physical reply의 receiver를 준비한다 | `F/include/zlink/framework/contracts/dispatch/request_task.hpp:648–751` | O 안의 일부. PR 1 task-owner self 합 0.6843은 반려된 구현의 비용이다 | A. 수명과 최종 receiver를 남긴다. heap/shared_ptr 교체를 다시 제안하지 않는다 |
| 최초 clock·flow·ambient context를 캡처한다 | `F/src/runtime/channels/channel_runtime.cpp:1742–1749` | R/O/X에 포함, 독립 시간 없음 | A. deadline·flow·cancellation 문맥은 요청별 값이다 |
| host 구성과 transport를 이름으로 찾고 route/mesh variant를 고른다 | 같은 파일 `:1750–1766` | R 안의 일부 | C. 불변 endpoint 참조를 bind/첫 사용에 준비한다. route의 현재 수락 여부는 고정하지 않는다 |
| target·packet/channel 이름·reply decoder의 수명을 연결한다 | 같은 파일 `:1768–1849` | R/O 안의 일부 | A/B/C. target/lifecycle 캡처는 A, reply 해석은 B, 불변 decoder 연결은 C다 |
| 기본 timeout을 고르고 최초 시작 시각에서 절대 deadline을 만든다 | 같은 파일 `:1851–1860` | R 안의 일부 | A. bind 때 기본값을 준비해도 절대 deadline은 요청마다 만든다 |
| declared type으로 serializer를 조회한다 | `F/include/zlink/framework/contracts/channels/channel.hpp:910–916`, `contracts/codecs/serializer.hpp:303–313`, `F/src/runtime/codecs/serializer.cpp:164` | E/R 안의 일부 | C. S05:218–234의 기존 bounded cache와 bind 참조를 사용한다. 새 무제한 cache는 만들지 않는다 |
| 선택한 serializer로 typed 요청을 encode한다 | `channel.hpp:915`, `channel_runtime.cpp:1884`, `serializer.hpp:216–245` | E=2.12 안의 일부, payload 단독 시간 없음 | **현행 A**. S08:68–70·96에 따라 caller에서 끝난다. 승인 뒤 B로 옮길 대상이다 |
| JSON correlation과 UTC deadline 등 동적 envelope 값을 만든다 | `F/src/runtime/messaging/client_call_codec.cpp:16–28,34–56,73–94`, `channel_runtime.cpp:1862–1883` | E 안의 일부. ccall2 create_envelope self/inclusive 0.2671/0.3672 | 현행 A. 정적 이름/fragment만 C다. 동적 값은 요청마다 필요하며 승인 뒤 작성 위치만 B로 바꾼다 |
| protocol header를 검증하고 TLS plan을 비교·필요시 재작성한다 | `F/src/runtime/messaging/envelope_codec.cpp:323–372,425–440` | E 안의 일부 | A/C. 동적 검증은 A다. 이미 정적 cache가 있으므로 cache 도입 효과를 새로 세지 않는다. plan 비교 반복은 C 후보이다 |
| header 크기를 재고 header bytes를 작성한다 | 같은 파일 `:441–447` | E 안의 일부. ccall2 header_writer append self 0.2671 | 현행 A. 최종 service frame에 직접 한 번 작성하여 중간 header message를 없애는 일은 C다. wire 필드 자체는 삭제하지 않는다 |
| header/body를 묶고 상위 receiver projection을 연결한다 | `client_call_codec.cpp:65–70`, `channel_runtime.cpp:1885–1914` | E/R/O에 분산 | byte 의미는 A, 반환 뒤 결과 변환은 B, 계층 간 값 변환만 위한 연결은 C다 |
| mesh → public host → raw owner를 호출한다 | `F/src/runtime/mesh/mesh_node_runtime.cpp:4417–4490`, `F/src/runtime/stateful/public_host_runtime.cpp:3390–3431`, `F/src/runtime/mesh/raw_mesh_node_owner.cpp:1325–1340` | R/O에 분산 | C는 순수 forwarding과 변환 chain뿐이다. target 부재·shutdown·Ready 오류 분류는 소유 모듈에서 유지한다 |
| operation owner의 FIFO turn을 얻고 같은 caller에서 실행한다 | `raw_mesh_node_owner.cpp:1519`, `F/src/runtime/execution/state_lane.hpp:289–350`, `state_lane.cpp:134–158` | R 안에 포함. owner turn inclusive는 요청 본문도 포함한다 | A. 이미 caller-inline 경로다. 다른 worker 왕복이 현재 매번 존재한다고 가정하지 않는다 |
| peer/expected target와 port를 확인한다 | `raw_mesh_node_owner.cpp:1523–1529` | R 안의 일부 | A. logical Ready/descriptor와 close 수락은 Framework 소유다. 이 확인으로 native credit 수락을 대신하지 않는다 |
| nested application payload를 최종 service payload로 pack한다 | `raw_mesh_node_owner.cpp:1526`, `F/src/runtime/protocol/service_wire_codec.cpp:3114–3180` | E/R/N에 분산. ccut의 ccall 재분류 inclusive 0.5280은 중첩값이다 | 현행 A. canonical count/length/byte layout은 유지한다. 승인 뒤 작성은 B, 중간 header 표현은 C다 |
| ReplyRouteId와 full OperationId를 만들고 service header/두 native part를 준비한다 | `raw_mesh_node_owner.cpp:1530–1541` | R/E/N에 분산, identity 단독 시간 없음 | identity는 A. service bytes는 현행 A이며 승인 뒤 B다. 두 outer part는 binding과 같아 part 수 감소를 절감으로 세지 않는다 |
| C1 pending·dispatcher 자리·cancellation을 등록한다 | `F/src/runtime/foundation/operation_registry.cpp:382–474` | R 안의 일부. ccut 재분류 registry self/inclusive 0.0856/0.4852 | A. C1은 native pending map과 다른 사실을 소유한다. D로 없앨 수 없다 |
| C2 deadline index에 연결하고 필요시 관리 방문을 앞당긴다 | 같은 파일 `:428–470,488–516` | R 안의 일부, deadline 단독 시간 없음 | A. 동일 owner turn에서 등록한다. 이미 lane 안이면 추가 post가 없다. 두 번째 timer/index는 만들지 않는다 |
| Ready와 남은 deadline을 읽고 native request를 시작한다 | `raw_mesh_node_owner.cpp:225–241,1550–1563` | R/N 안의 일부 | A. Ready 대기 coroutine은 Ready 성공 경로에서 실행하지 않는다. 없는 대기를 제거한 것으로 계산하지 않는다 |
| target를 public RID로 만들고 socket wrapper 수명 아래 builder를 채운다 | `F/src/runtime/backend/raw_route_port.cpp:369–391` | R/N 안의 일부. ccall2 request_impl self/inclusive 0.2504/0.5341 | A/D. binding builder를 사용한다. RID/endpoint의 불변 변환은 C다. outer mutex의 제거 가능 비용은 미분리다 |
| native state·binding pending·admitted/reply bundle을 연결하고 첫 DONTWAIT를 호출한다 | `B/src/Runtime/Sockets/router.cpp:85`, `B/src/Runtime/Messaging/request_reply.cpp:102–147`, `completion_owner.cpp:585–595`, `operation_submit.hpp:128–196` | N=5.00 안의 일부. ccall2 native zlink_request self/inclusive 0.3004/0.5007은 전체 binding 호출 시간이 아니다 | A/D. 현재 bytes가 모두 준비된 상태에서 caller가 실행한다. native admission과 map은 binding/Core를 재사용한다 |
| result snapshot을 읽고 BACKPRESSURED일 때만 admitted를 소비할 observer를 시작한다 | `F/src/runtime/backend/raw_binding_adapter.hpp:41–61,147–197`, `raw_route_port.cpp:393` | O/R 안의 일부 | A/D와 B. snapshot 판정은 A/D, 이후 admission/reply 처리는 B/D다. 기존 awaitable 계약을 쓴다 |
| raw admission의 bool 결과와 상위 submit-result task를 다시 관찰한다 | `raw_mesh_node_owner.cpp:413,1585–1586`, `public_host_runtime.cpp:3401–3431`, `mesh_node_runtime.cpp:4471–4490` | R/O에 분산, 개별 감축량 없음 | C. 단계 객체만 바꾸는 제안이 아니라 같은 사실의 변환 chain을 삭제할 대상이다. ccall2에서 없앤 chain의 절감은 재계상하지 않는다 |
| owner turn 뒤 mailbox를 drain하고 lane-current scope를 끝낸다 | `state_lane.hpp:318–348`, `state_lane.cpp:152–158,169–190` | R/X에 포함, own work와 tail 단독 시간 없음 | own turn 종료는 A. 남의 관리 work는 B 후보지만 FIFO/다음 caller의 turn을 보존해야 한다. drain inclusive 전체를 삭제 비용으로 잡지 않는다 |
| admitted를 관찰하고 다음 제출 가능 여부를 확인한다 | `bench/client/bench_cpp_client.cpp:911–917,942–946` | O에 포함 | A는 다음 요청을 내기 위한 실제 admission 관측이다. harness 의미는 유지한다. 이를 background task 등록만으로 성공 처리하지 않는다 |
| reply observer를 등록하고 완료된 결과를 읽어 payload를 검증·파괴한다 | 같은 파일 `:948–989`, `bench/common/bench_async.hpp:41–72`, `F/include/zlink/framework/contracts/dispatch/task.hpp:690–697,721–788` | O/X에 포함. ccall2는 이를 독립 비용으로 분리하지 않았다 | caller가 선택한 application 소비다. 제품 경로의 C라고 보지 않는다. Framework typed 변환은 이미 완료 자원에서 하고, benchmark 검증은 그대로 둔다 |

다음 일은 현재 정상 성공 경로에서 caller가 기다려 수행하는 추가 단계가 아니다. BACKPRESSURED 뒤 `completion_owner.cpp:668–756`의 drain·retry는 D, `raw_mesh_node_owner.cpp:1369–1453`의 physical reply 검증은 B, `operation_registry.cpp:539–549`의 C1 take는 알림을 받은 자리의 B, `operation_registry.cpp:269–295`와 `request_task.hpp:682–698`의 dispatcher 전달·typed projection은 B다. 이 비용을 caller 표에서 다시 뺄 수 없다. ccall2의 completion 자원 비용은 오히려 증가했으므로, 단순 이동의 증거를 절감으로 쓰지 않는다.

**binding 기능을 먼저 사용하는 경계**

| 필요한 기능 | 현재 존재 여부와 근거 | 이번 설계의 처리 |
|---|---|---|
| 첫 request를 대신 실행하는 submit queue | **없다.** `B/.../completion_owner.cpp:585–595`는 map에 넣은 직후 같은 호출 stack에서 `start_request()`를 실행한다. `request_reply.cpp:131–142`는 그 결과로 snapshot을 만든다 | Framework queue로 보충하지 않는다. 아래 사용자 결정의 대상으로 남긴다 |
| HWM pending record 보유와 정확한 target의 WRITABLE 재제출 | 있다. `completion_owner.hpp:81,87`, `completion_owner.cpp:279–345,414–442,668–756` | 같은 binding operation과 기존 native token을 재사용한다. pollout hint를 보고 Framework에서 재시도하지 않는다 |
| 여러 요청의 전송 byte를 TCP write에 묶기 | Core의 기존 I/O 경로에 있다. coreio의 두 물리 part·같은 native library 대조가 근거다 | Framework batched-send engine을 만들지 않는다. 기존 queue에 충분한 수락 record를 공급하는 것이 목적이다 |
| public multi-request batch-submit 또는 미완성 record의 admission lease | 현재 계약에 없다. `bindings/doc/spec/cpp/README.ko.md:593–599`, `core/doc/spec/core/socket/README.ko.md:49–53,1091–1099,1191–1196`는 완성된 whole message를 한 번 제출한다 | 단일 message의 multipart와 여러 request의 batch는 다른 기능이다. 기존 multipart를 batch API라고 부르지 않는다 |
| native pending/request matching·stage join·late payload 정리 | 있다. `B/.../completion_owner.hpp:125–134`, `.cpp:279–345,751–759`, binding async model:104–127 | 새 physical pending 표나 observer terminal engine을 만들지 않는다 |
| completion 연속 drain·WRITABLE의 drain 후 재제출 | 있다. `B/src/Runtime/Eventing/poller.cpp:396–404`, `completion_owner.cpp:684–706` | poller 하나를 계속 구동한다. permit 0에서도 completion과 관리 진행은 유지한다 |
| Framework logical C1/C2를 binding pending 하나로 대체하기 | 없다. native map은 OperationId·logical shutdown·Ready 대기 deadline을 소유하지 않는다 | C1·C2는 유지한다. 이 둘은 binding 기능의 중복 구현이 아니다 |
| typed serializer의 exact-size/validate와 write 분리 | 없다. `F/.../serializer.hpp:216–245`의 encoder는 완성된 encoded_payload를 반환한다 | generic serializer가 encode 없이 크기·실패를 싸게 확정한다고 가정하지 않는다. 사용자 결정이 필요하다 |
| user continuation을 binding receive stack에서 실행하기 | 허용하지 않는다. S01:496–519, S08:306–326 | binding은 내부 완료까지만 진행하고, 기존 Framework dispatcher가 독립 application turn을 실행한다 |

binding에는 자동 background completion thread가 없다(binding async model:67–79). 추천안도 binding이 자기 thread를 만들거나 두 번째 poller를 갖는 안이 아니다. Framework가 이미 가진 실행 자원을 사용하며, 해당 기능이 public binding 계약으로 제공되기 전에는 private completion_owner를 호출하지 않는다.

**배치만으로 해결되지 않는 제출 순서**

현행 S08:68–70·98–99와 state lane §5:232–255는 E1 및 등록·캡처와 binding 시작을 반환 전에 끝내도록 한다. 무경합 state lane은 이미 caller가 자기 일을 실행한다. 단일 caller의 1번 호출이 그 일을 끝내기 전에는 2번 호출이 들어오지 않으므로, 동기 owner turn을 다른 thread로 옮기고 caller가 반환을 기다리게 해도 같은 caller의 여러 요청이 그 앞에 쌓이지 않는다. 배치할 요청이 없고 왕복 비용만 늘어난다.

반환만 먼저 하게 바꿔도 현재 harness에서는 같은 문제가 생긴다. `bench_cpp_client.cpp:915–916`은 `preceding_admission->await_ready()`가 false면 다음 제출을 멈춘다. 따라서 queue에 1번 요청을 넣고 실제 native admission 때까지 admitted를 pending으로 남기면 준비 queue에는 이 caller가 만든 미수락 요청이 한 건뿐이다. window는 reply의 개수이며 미수락 요청을 무제한 선제출할 허가가 아니다. 다른 caller의 요청이 모이는 경우를 이번 단일 caller의 근거로 쓰지 않는다.

이 두 방식은 별도 채택 설계로 제안하지 않는다. caller CPU가 낮아져도 admission을 기다리는 경과 시간과 처리율 제한은 남는다. Framework enqueue를 admitted 성공으로 바꾸거나, 실제 BACKPRESSURED 앞에서 뒤 요청을 계속 내게 하거나, 여러 submit caller로 benchmark를 바꾸는 우회도 사용하지 않는다.

**추천안의 전체 경로와 필요한 하위 계층 기능**

추천안은 다음 능력이 public binding/Core 계약으로 제공된다는 조건에서만 성립한다. 완성된 bytes 대신 소유권이 확정된 요청 표현에 대해 target·byte HWM·completion capacity를 **실제로** 수락하고, 그 native operation의 body를 뒤에서 구성할 수 있어야 한다. 이 보고서에서는 이 필요한 능력을 admission reservation이라고 부른다. 현재 내부의 completion slot 예약은 이 능력이 아니다. Core socket spec:1219–1231은 completion slot을 얻은 뒤에도 physical admission이 거절될 수 있음을 명시한다. source도 `core/src/api/socket/socket_request_reply_submit_api.cpp:559–576`에서 correlation 예약과 physical submit을 구분한다.

1. bind 또는 declared type의 첫 사용 때 기존 codec 선택 결과, 정적 envelope fragment, wire layout, transport 참조와 typed projection을 준비한다. S05:218–234의 bounded cache를 따르며, 현재 Ready generation·선택 cursor·deadline·C1 entry를 미리 확정하지 않는다. static fragment 준비는 binding에 없는 Framework codec 책임이다.
2. caller는 이미 call이 소유한 typed 값, metadata, ambient/flow와 단 하나의 절대 deadline을 고정한다. `channel.hpp:912–920`의 stack reference encoder와 selected_serializer를 그대로 queue에 넘기면 dangling reference가 된다. 기존 값 소유를 operation으로 이전하고, serializer descriptor와 content-type 참조의 수명도 그 소유에 포함해야 한다. 소유권이 확정되지 않은 `std::ref`를 보내는 안은 없다. 기존 `call.hpp:181–195`의 async/start는 rvalue 전용 소모 API가 아니므로, 같은 call의 재사용 의미를 확인하지 않고 request_value를 비워서도 안 된다. 기존 closure의 수명을 유지하는 방법과 요청별 새 snapshot 비용을 구분해야 하며, opaque 사용자 값이 얕은 복사나 외부 mutable 참조를 포함하는 경우까지 자동으로 immutable이라고 가정하지 않는다. serializer가 caller thread-local 상태에 의존하던 경우의 실행 문맥 변경도 계약 결정에 포함한다.
3. 같은 operation owner turn에서 최종 receiver, identity, C1, C2와 dispatcher 자리를 등록한다. 등록보다 native reservation이 앞서지 않는다. close와 등록은 기존 owner가 직렬화한다. caller에서 없앨 수 있는 것은 그 주변의 forwarding·정적 조회이며 C1/C2의 권한 자체가 아니다.
4. public binding이 같은 native operation에 대해 actual admission을 한 번 시도한다. 성공이면 이미 native capacity가 charge되고 request identity/target 수명이 고정되어 있어야 한다. Framework가 Ready cache나 빈 queue를 보고 성공을 선언하지 않는다. caller에게 성공을 알린 뒤 body를 만들 때 두 번째 독립 request를 제출하는 구조도 허용하지 않는다.
5. 수락된 operation의 준비 작업만 기존 실행 자원에서 연속 처리한다. 소유 표현은 binding operation 하나에 두고, Framework의 기존 operation work 배치에 참조를 전달한다. binding/Core의 physical queue와 별도의 Framework transmission queue를 만들지 않는다. 준비 작업은 codec 모듈을 호출해 canonical 최종 service buffer를 작성하고 **같은** 수락 record를 공개한다. Core가 이미 하는 TCP batch를 다시 구현하지 않는다. 별도 준비 queue나 lease capacity counter가 추가로 필요하다면, 대체할 기존 메커니즘을 제시하기 전에는 이 설계를 구현하지 않는다.
6. 배치는 이미 실행 가능한 작업을 연속 처리하는 것이다. N건이 찰 때까지 기다리는 timer나 sleep을 두지 않는다. 한 건뿐이면 즉시 처리한다. 다음 준비 record는 admission된 순서를 보존한다. 여러 준비 자원을 쓴다면 같은 route의 native publication 순서는 하위 계층이 소유해야 하며 Framework reorder map을 추가하지 않는다. 우선안은 준비 자원 하나가 수락 순서대로 구성하는 것이다. 그 자원의 서비스 시간이 6.6 µs를 넘으면 caller 외의 새 병목이므로 기각한다.
7. binding의 기존 admitted/reply와 completion drain을 사용한다. terminal 알림에서 기존 C1을 즉시 take하고, 기존 dispatcher 자리로 전달한다. typed decode와 physical 자원 정리는 각 codec/receiver가 한 번 수행한다. public timeout 또는 admission 실패가 먼저 끝나도 physical 수신자의 수명과 route cache 처리는 S01:333대로 끝까지 유지한다. dispatcher 앞에 새 completion lane을 두지 않는다.

`message_t::allocate`나 external message로 아직 작성하지 않은 bytes를 먼저 submit하고 worker가 나중에 채우는 우회는 없다. native admission 뒤 전송 계층이 그 메모리를 이미 읽을 수 있으므로 소유권과 data race를 해결하지 못한다. 기존 whole-message API가 이러한 지연 작성을 제공한다고 해석하지 않는다.

준비 자원은 poller를 계속 소유한 채 arbitrary user codec이나 user callback을 실행하는 자원이 아니다. 수신 역할은 별도 기존 자원에서 계속 진행한다. 같은 자원 재사용을 선택한다면 S08:309–326대로 역할 인계가 성공하고 이전 wait/drain과 lane-current stack이 끝난 뒤에만 다음 turn을 실행한다. 이를 확인하는 추가 대기나 새 thread는 만들지 않는다. 처리 자원 하나를 다른 역할에서 가져오면 그 자원이 하던 CPU도 전체 회계에 남긴다.

admission과 byte 공개 사이의 새 실패 의미는 현재 계약에 없다. serializer가 나중에 실패하면 기존에는 admitted 실패였던 요청이 admitted 성공 뒤 reply 실패가 될 수 있다. byte 공개 전에 취소·timeout·close·handover가 발생했을 때 예약을 반납하는 책임, request timeout의 기산점, 같은 route의 뒤 record 진행도 정해야 한다. 이것이 단순 내부 실행 위치 변경이 아니라 D(spec gap)인 이유다. 이 의미를 확정하지 않고 미완성 record의 native API를 설계한 것으로 간주하지 않는다.

**BACKPRESSURED와 완료 순서**

| 상황 | 보존할 처리 |
|---|---|
| 등록 뒤 첫 native admission 성공 | 원래 submit 결과는 OK이며 admitted가 먼저 성공한다. 그 뒤 reply가 완료될 수 있다. caller는 그 실제 성공 뒤 다음 요청을 낸다. body 준비가 앞 admission을 뒤집지 않는 의미는 사용자 결정이 필요하다 |
| physical credit 부족, nonzero WRITABLE 토큰 | 원래 result는 BACKPRESSURED로 고정한다. 기존 binding entry가 동일 target·payload 표현·deadline을 보유한다. poller가 completion queue를 NO_DATA까지 비운 뒤 해당 token의 요청만 재시도한다. 성공 전에는 admitted를 끝내지 않는다 |
| 재제출도 BACKPRESSURED | 새 native 토큰을 기존 binding lifecycle 안에서 처리한다. Framework가 두 번째 operation·retry queue·poller·pending flag를 만들지 않는다 |
| completion slot 포화, 토큰 0 | 기다릴 completion이 없는 제출 실패로 끝낸다. queue에 숨기거나 Framework retry를 시작하지 않는다(S01:528–531, Core socket:1132–1138) |
| admitted 실패 | admitted와 reply가 같은 원인으로 각각 한 번 실패한다(binding async model:117–122). batch 전체의 일반 오류로 덮지 않고 항목별 원인을 유지한다 |
| reply가 submit 반환과 경합 | binding의 기존 submit/completion join이 stage 순서를 보장한다. Framework C1은 이미 등록되어 있다. 조기 reply용 두 번째 Framework 보관 map을 만들지 않는다 |
| logical timeout·취소·shutdown이 먼저 확정 | 기존 C1 take가 public 결과를 확정한다. C2 참조는 기존 관리 방문에서 정리하고, binding의 late physical 정리를 별도로 끝낸다. 배치 처리 시각에서 timeout을 새로 시작하지 않는다 |
| callback 하나가 막힘 | 다른 completion과 수신·관리 작업이 그 callback 반환을 기다리지 않는다. batch를 한 callback loop로 합쳐 사용자 callback들을 직렬 의존시키지 않는다(S08:324) |

현재 binding이 가진 WRITABLE 재시도와 단계 순서는 그대로 재사용할 수 있다. 다만 새 reservation이 기존 whole-record submit을 대체하는 방식으로 이 계약과 합성되는지는 새 public 계약의 일부다. 현행 API를 한 번 호출하는 것으로 이미 제공된다고 주장하지 않는다.

**인계·깨우기 비용의 근거와 한계**

| 지정 자료의 수치 | 쓸 수 있는 해석 | 쓸 수 없는 해석 |
|---|---|---|
| ccall2 caller scheduler self 0.4507, inclusive 0.5174; pthread mutex lock/unlock self 0.5508/0.2337 µs | 현재 caller에 scheduler와 lock CPU가 존재한다 | 새 queue 인계 한 번의 단가나 제거 가능한 socket lock 하나의 비용이 아니다 |
| ccall2 completion 자원 16.311 → 17.125 µs/request, 증가 0.815 | caller 결과 처리 일부의 이동에 receiving 자원의 비용이 따라온다는 반례다 | 새 배치 인계가 0.815라는 뜻도, 기존 completion 전체를 제거할 수 있다는 뜻도 아니다 |
| cbind가 인용한 기존 source handoff/wake 18.8 µs/request | source 전체에서 이동·통지 계열 비용이 큰 검증 대상이라는 근거다 | 한 번의 wake 비용이 아니다. 요청별 동기화와 여러 역할의 비용을 모두 N으로 나눌 근거가 없다 |
| cbind의 rate별 context switch·eventfd 횟수 | 52k/100k/무제한에서 통지 빈도가 달라진다. 같은 rate로 비교해야 한다 | syscall 횟수에서 새 handoff의 CPU나 wake-to-run 시간을 계산할 수 없다 |
| coreio C++ source I/O eventfd-write CPU bucket 0.66 µs/request | 그 실행의 I/O 알림 CPU 전체를 없애더라도 그 bucket 이상을 절감했다고 할 수 없다 | TCP send/receive 차이까지 notification 절감으로 더할 수 없다 |
| coreio의 epoll·eventfd·mailbox 조사 | 기존 scheduled/pending과 transport batching을 재사용해야 한다 | 정확한 scheduler wake 횟수나 queue-publication-to-run 단가는 측정되지 않았다 |

cthin의 별도 caller handoff 분류도 1.890 µs지만 allocator를 제외한 mutex/futex/scheduler 묶음이다. 이번 binding API 인계의 단가로 전용하지 않는다. 지정 자료에는 요청한 새 인계의 고립 단가가 없다. 따라서 예산표의 0.30은 실측 근거가 있는 예측이 아니며 구현 뒤 반드시 확인할 상한이다.

배치 N의 비용은 `항목별 작업 + 항목별 queue 연결 + batch 게시/깨우기 ÷ 실제 N`이다. typed encode, C1 take, 각 callback의 독립 실행은 항목별 작업이므로 N으로 나누지 않는다. 18.8 전부가 배치 고정비라고 극단적으로 가정해도 N=32일 때 0.5875, N=64일 때 0.29375 µs다. 이 계산은 0.30의 **가능성 증명도 아니다**. 실제로는 18.8의 얼마가 같은 batch의 고정비인지 모르고, 빈 queue의 batch는 1건이며, worker 쪽 wake 비용과 기존 완료 통지가 남는다. raw binding의 적은 eventfd 횟수만으로 N=64를 가정하지 않는다.

**150k에서 source 전체 CPU의 합격 조건**

source process의 합격 조건은 다음과 같이 두어야 한다. `B150`은 동일 조건·동일 150k rate로 pacing한 binding source 전체 CPU, `F0`는 같은 rate에서 현재 Framework 경로가 더 쓰는 CPU다. `I`는 같은 기능을 보존하는 데 필요한 Framework 추가 작업의 독립적으로 검토한 예산이다. `D`는 현재 경로에서 실제로 삭제한 CPU, `W`는 caller에서 준비 자원으로 옮긴 CPU, `G`는 분리된 native admission 뒤 최종 공개에 추가된 CPU, `H`는 새 인계의 양쪽 CPU다. `I`에는 필요한 encode/decode·logical C1/C2·typed delivery·application 검증 차이와 실제 유지해야 하는 byte 비용을 포함한다. 없앨 수도 있는 dispatcher 전체나 wake 비용을 모두 I로 이름 붙여 합격시키지 않는다.

기존 경로를 같은 rate에서 다시 배치한 회계는 `C_source,new = B150 + F0 − D + G + H`다. W는 caller에서 빼고 worker에서 더하므로 합계에서 상쇄된다. 사용자의 전체 조건 `C_source,new ≤ B150 + I`에는 **`D ≥ F0 − I + G + H`**가 필요하다. 적어도 기존 합계보다 낮추려면 `D > G + H`여야 한다. 필요한 G/H를 I에 포함해 예산을 정할 수는 있지만, 그때 같은 비용을 식 밖에 다시 더하지 않고 I도 사전에 근거를 제시해야 한다. 어느 방법이든 I를 사후에 실측 전체 차이와 같게 정의하는 방식은 허용하지 않는다.

| 비교 항목 | 가능한 계산 | 판정 |
|---|---|---|
| caller 16.61을 6.50으로 만들되 감소분을 모두 옮기는 경우 | off-caller에 10.11 µs/request를 더한다. 150k에서 약 1.52 core를 추가로 쓴다. G/H가 있으면 더 늘어난다 | 단독으로는 이번 요구에서 기각한다 |
| 기존 binding 기능을 재사용하는 경우 | 새 native pending map·물리 retry engine·두 번째 poller의 **추가 비용을 피한다** | 이미 사용 중인 map/drain 자체를 삭제 절감으로 계산할 수 없다 |
| C1/C2·typed receiver를 유지하는 경우 | 필요한 I는 남는다 | 같은 기능의 binding 기준에 없는 logical authority를 지웠다고 주장하지 않는다 |
| static plan·중간 header 표현·순수 forwarding을 실제 삭제하는 경우 | D의 후보지만 독립 µs와 150k에서의 효과는 미측정이다. ccall2가 이미 수행한 변경은 새 D에서 제외한다 | D가 G/H와 남은 불필요 작업보다 크다는 증거는 아직 없다 |
| TCP 묶음이 rate 상승으로 커지는 경우 | B150와 같은 rate에서 한 번 반영한다 | caller 개선, 준비 batch, completion batch 각각에 I/O 절감을 중복 배정하지 않는다 |
| 150k의 binding 기준 | cbind에는 그 조건이 없다. 보고서의 약 21.3 µs/source는 단순 보간값이다 | 실측 B150라고 쓰거나 새로운 성능 확인 없이 전체 조건 통과를 선언할 수 없다 |

예를 들어 cbind의 동률 실행에서 나온 source 추가 CPU 42.18을 150k에서도 그대로 남는다고 **가정**하고, B150에 보간값 21.3을 쓰면 source는 63.48 µs/request, 약 9.52 core다. source affinity 8 CPU의 산술 한도는 53.33 µs/request이므로, 이 민감도 계산만으로도 최소 약 10.15 µs/request를 없애야 한다. G/H는 그 위에 더해진다. 42.18이 rate에 따라 그대로 유지된다는 측정은 없으므로 이는 예측이 아니라 **이동만으로 충분하다고 할 수 없는 수치 예시**다. 더구나 이번 사용자의 `B150 + I` 조건은 단순히 8 core 안에 들어오는 것보다 엄격하다.

받는 준비 자원도 한 core당 서비스 시간 한도를 지켜야 한다. 10.11을 모두 준비 자원 하나에 옮기면 그 자원의 서비스 시간도 10.11 이상이다. 6.6 이하로 만들려면 적어도 3.51 µs/request를 삭제하거나 기존 독립 자원에서 이미 수행하던 작업임을 구분해야 하며, 추가 G/H는 그 여유를 더 줄인다. encoding의 노출 비용 2.12를 그대로 남긴다고 가정하면 그 자원의 나머지 작업·최종 공개·인계에 쓸 수 있는 예산은 4.48 이하이다. 이 수치는 worker 실측이 아니라 수용 가능한 상한 계산이다. W에 registration/observation 묶음 전체를 무조건 옮겨 넣으면 그 자원이 새 bottleneck이 된다. 2.12의 encoding조차 worker에서의 비용과 동일하다는 보장은 없다. 새 admission 후 공개 비용 G, 양쪽 H, 기존 completion 17.125가 각각 어디서 실행되는지 전체 role 회계로 확인해야 한다. 기존 completion thread에 이 일을 전부 얹는 설계는 추천하지 않는다.

따라서 이번 읽기 전용 자료만으로 source 전체 조건을 만족한다는 수치 증명은 할 수 없다. 부족한 값은 B150, I의 배타적 분해, 새 admission 비용, G/H, 실제 batch 크기와 D다. 이를 임의 수치로 채운 총합을 제시하지 않는다. 이 한계 때문에 추천안은 구현 완료안이 아니라 사용자 결정이 필요한 설계 후보이며, 전체 조건을 통과한 설계 수는 0이다.

**스펙 충돌과 사용자 결정**

| 변경하려는 경계 | 충돌하거나 새 계약이 필요한 조항 | 필요한 사용자 결정 | 노출된 caller µs와 비용 의미 |
|---|---|---|---|
| E1 typed encode를 caller 뒤로 옮긴다 | S08 `01-execution/08-messaging-hot-path.ko.md:68–70,96`; 기존 serializer `F/.../serializer.hpp:216–245` | 소유된 typed 값의 encode를 admission 뒤 자원에서 허용할지, serializer 실행 문맥·실패 timing을 어떻게 규정할지 선택해야 한다 | E=2.12 안의 일부 또는 전체 이동 대상. R/N에 섞인 pack 비용과 합쳐 전액 절감으로 세지 않는다 |
| encode 없이 정확한 admission byte 크기와 validation을 확정한다 | Core socket `README.ko.md:49–53,1091–1099`; S08:96; serializer:216–245 | 기존 serializer 계약을 확장할지, 현행 serializer는 caller encode를 유지할지 선택해야 한다. 전체 encode를 sizing으로 다시 실행하는 안은 목적을 충족하지 못한다 | 예산 0.25와 E=2.12 사이의 차이가 쟁점이다. size/validation 단독 실측은 없다 |
| 완성되지 않은 body를 실제 admission한다 | Core socket `README.ko.md:1091–1099,1219–1245`; binding C++ README:593–599 | body 준비 전의 실제 HWM/target/completion 수락과 같은 operation의 최종 공개를 public 하위 계층 기능으로 만들지 선택해야 한다. Framework queue 수락을 native admission으로 바꾸는 선택은 추천하지 않는다 | 기존 binding 비용을 caller에서 0으로 만들 수 없다. 새 reservation ≤3.70과 추가 G가 둘 다 쟁점이다 |
| binding에 첫 제출 준비·배치 기능을 추가한다 | binding async model `:67–79,117–125`; binding C++ README `:380–385,556–561` | 기존 poller·pending lifecycle에 기능을 넣는 public 계약을 승인할지 선택한다. 자체 background thread·private header 호출·두 번째 owner는 제외한다 | caller에서 옮길 W와 새 H/G는 미측정이다. 새 queue만 만들면 합계는 증가한다 |
| E3/E4를 enqueue 후에 실행하고 submit은 먼저 반환한다 | S08 `:98–113`; lane `06-state-ownership-and-lanes.ko.md:230–255`; S01 `:424–428` | 이 완화는 추천하지 않는다. 승인하더라도 현재 admitted 직렬화를 유지하면 batching 효과가 없다 | R=5.76과 N=5.00의 일부가 이동 대상일 뿐이다. 이 두 묶음의 합을 절감으로 선언할 수 없다 |
| serializer 실패를 admission 뒤에 관측한다 | S01 `:331–333,335–365`; binding async model `:119–122` | 기존 admitted 실패를 admitted 성공 뒤 reply 실패로 바꿀 수 있는지, 수락된 미공개 record의 오류 terminal을 무엇으로 둘지 정해야 한다 | E의 이동과 연결된 의미 변화다. 별도 µs 절감으로 더하지 않는다 |
| 준비 대기 동안 deadline·close·handover를 처리한다 | S01 `:357–365,447–488`; Core socket `:1242–1250`; lane §5 | 원 절대 deadline을 유지하며 native 예약과 미공개 body를 누가 끝내는지 확정해야 한다. Framework deadline index를 추가하지 않는다 | deadline 단독 시간은 미분리다. 새 per-request timer를 더하면 비용도 규칙도 늘어난다 |
| C1을 native pending으로 합치거나 C2를 없앤다 | S01 `:392–428,447–473` | 추천안에서는 변경하지 않는다. 사용자가 요구한 단일 logical terminal/deadline owner를 유지한다 | R=5.76 전체를 회수할 수 없다. registry 단독 참고 inclusive 0.4852도 모두 삭제 가능한 시간이 아니다 |
| binding 알림을 받기 전에 별도 Framework completion batch queue로 보낸다 | S01 `:462`; S08 `:100,136–139` | 추천안에서는 하지 않는다. C1 take를 받은 자리에서 수행한다 | 기존 completion CPU를 caller 절감으로 쓸 수 없다 |
| callback 여러 개를 한 serial drain으로 실행하거나 receive 역할 중 실행한다 | S01 `:490–519`; S08 `:300–328`, 특히 `:309–326` | 추천안에서는 완화하지 않는다. 역할 인계 또는 기존 독립 worker turn을 사용한다 | wake/dispatcher 비용의 전액 제거는 불가하다. ccall2 completion 17.125는 제거 가능 시간이 아니다 |
| nested wire를 없애거나 protobuf만 보낸다 | S08:96 및 `S/02-channel-transport/06-wire-protocol.ko.md:186–211` | 추천하지 않는다. schema와 네 언어 wire를 유지한다 | encoding/pack과 byte 비용의 일부가 노출되지만 동일 기능 비교가 아니게 된다 |

현재 계약 안에서 할 수 있는 정적 참조 준비와 중간 표현 삭제는 A(계약 적응)/B(기존 결함) 후보이다. 이번 목표를 위해 처음으로 제안한 admission/encode 분리는 D다. 이를 A/B라고 재분류하거나 현행 spec이 성능 목표를 논리적으로 금지한다는 결론으로 확대하지 않는다. 현행 spec을 지키는 다른 알고리즘이 6.6에 도달할 수 없다는 CPU 하한을 측정한 적도 없다.

사용자의 우선 결정은 다음 둘이다. 실제 native admission과 payload 준비를 분리하는 public binding/Core 기능을 검토할지, 그리고 그 기능에 필요한 serializer 크기·validation·실패 timing 변경을 허용할지다. 둘 중 하나를 허용하지 않으면 이번 추천 경로는 닫힌다. 그 경우 객체 경량화나 Framework queue를 대안으로 다시 제출하지 않으며, 현행 계약 내 성능 달성은 별도 입증이 필요한 상태로 남긴다.

**결정 소유와 언어 대조**

| 결정 | C++ 현재 위치 | .NET 현재 위치 | Java/Kotlin 현재 위치 | Node 현재 위치 | 추천안의 단일 소유자 |
|---|---|---|---|---|---|
| C1 logical terminal | `F/src/runtime/foundation/operation_registry.cpp:539–549` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5838–5848` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407–422,511–519` | `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:434–441` | Framework registry의 기존 atomic take 하나 |
| deadline·등록·close 수락 | C++ `operation_registry.cpp:382–474,488–516`, `raw_mesh_node_owner.cpp:1519–1564` | ManagedMeshNode `:86,5628,5669–5703` | OperationRegistry `:329–369` | operation-registry `:240–261` | 절대 deadline 값과 기존 C2 owner/index 하나 |
| physical admission·WRITABLE·native completion | `B/src/Runtime/Messaging/completion_owner.cpp:279–345,585–595,668–756` | public binding submission을 소비한다 | public binding submission을 소비한다. Kotlin은 같은 Java runtime을 쓴다 | public binding submission을 소비한다 | binding/Core의 기존 owner 하나. 새 reservation도 여기로 수렴해야 한다 |
| typed 최종 전달과 새 turn | C++ `request_task.hpp:682–698`, `operation_registry.cpp:269–295`, `provider_task_context.cpp:174–191` | `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13–16` | `runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51–74` | 기존 Node runtime의 application turn | 기존 최종 typed receiver와 process dispatcher. native poller는 사용자 callback owner가 아니다 |

.NET·Java/Kotlin·Node의 registry 등록과 terminal 제거, .NET/Java의 독립 dispatcher 게시를 직접 대조했다. 이 소유 경계는 C++만 제거할 이유가 없다. 이번에는 새 native admission 기능에 대한 다른 언어의 구현이 존재한다고 주장하지 않는다. 공통으로 관찰되는 admitted/serializer/error timing을 바꾼다면 C++만 변경하는 PR은 완료가 아니며, 감독자가 공통 계약을 확정한 뒤 같은 작업 범위에서 네 언어의 관찰 결과를 맞춰야 한다. 이번 C++ 설계 연구는 그 구현이나 교차언어 검증을 수행하지 않았다.

**PR 분할과 각 합격 예산**

아래는 사용자가 D 결정을 승인했을 때의 분할이다. 계약 승인 전에 runtime 구현을 시작하라는 제안이 아니다. 수치 목표를 만족시키지 못한 PR의 절감을 다음 PR에서 이미 얻은 것으로 취급하지 않는다.

| PR | 내용 | caller µs/request 합격 목표 | 별도 완료 조건 |
|---|---|---:|---|
| 1. 감독자의 admission 분리 계약 PR | 실제 native 수락·body 준비·동일 operation 공개·stage 실패·exact size·수명·FIFO·deadline을 한 소유 경계로 확정한다 | **16.61 이하 유지**. 문서 PR 자체의 감축 목표는 0이다 | 아래 D 결정을 미해결로 남기면 뒤 구현을 승인하지 않는다. C1/C2/typed owner와 WRITABLE 결과를 유지한다 |
| 2. Core/binding의 단일 staged operation | 새 public 기능을 기존 native queue·completion reservation·binding entry·poller에 넣는다. 기존 완성 message terminal과 경쟁하는 두 번째 물리 engine을 만들지 않는다 | 기존 Framework는 **16.61 이하**. 새 경로의 admission 호출 예산은 **3.70 이하**, caller 쪽 게시 예산은 **0.30 이하** | 추가 공개 비용 G, 인계 양쪽 H, 실제 batch를 모두 보고한다. 새 counter/flag/queue가 기존 것과 병행하면 반려한다 |
| 3. Framework의 caller 최소화와 batched preparation | 상단 caller 경로를 연결한다. E1의 실제 구성 작업을 통째로 옮기고 정적 조회를 bind/첫 사용으로 옮긴다. 현재 최종 receiver와 C1/C2를 유지하고 중간 변환 chain은 삭제한다 | **6.50 이하**, binding 호출·harness 소비 포함 | 실제 150k에서 source ≤ B150+I, errors/abandoned 0, 수락·완료 수 일치. receiving serial 자원도 6.6 이하. 공통 관찰 계약은 네 언어 대조로 확인한다 |

PR 1을 작고 수치가 좋아 보이는 객체 교체 PR로 시작하지 않는다. 필요한 public 계약이 없는데 Framework 구현부터 하면 새로운 queue·상태와 재시도 책임이 위 계층에 생기기 때문이다. 예산 6.50은 PR 3의 종료 조건이며, PR 1·2에 거짓 caller 감축을 배정하지 않는다. 별도 wire 변경 PR이나 C1 삭제 PR은 없다.

첫 PR의 파일 목록은 다음과 같다. **감독자가 정확한 경로·범위를 승인한 뒤** 수정할 후보이며 이번 연구에서는 수정하지 않았다. 공통 의미의 소유 문서를 바꾸고 언어별 문서는 exact signature만 연결해야 한다. 같은 규칙을 각 문서에 다시 쓰지 않는다.

| PR 1 파일 | 범위 |
|---|---|
| `core/doc/spec/core/socket/README.ko.md` | 완성 record admission과 새 admission 분리 능력의 경계, capacity·FIFO·failure·timeout의 단일 소유 조항 |
| `bindings/doc/spec/async-execution-model.ko.md` | 새 native 능력의 stage 합류와 lifetime. 기존 admitted/reply 각각 한 번·실패 동일 원인을 유지한다 |
| `bindings/doc/spec/async-coroutine-policy.ko.md` | public terminal 형태가 달라지는 경우에만 정본 표면을 반영한다. request callback 우회를 추가하지 않는다 |
| `bindings/doc/spec/cpp/README.ko.md` | 승인된 C++ public signature와 poller 구동 조건. private completion entry/lease의 노출은 금지한다 |
| `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md` | E1의 caller 수행 범위와 E3/E4의 native admission 경계. 기존 반환 전 등록·완료 역할 분리를 유지한다 |
| `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` | encode 실패 timing·admitted 의미·physical 수신자 lifetime과 절대 deadline을 기존 소유 조항에서 확정한다 |
| `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md` | owned typed 값·exact sizing/validation·기존 serializer의 동작을 codec 소유 조항에서 확정한다 |

state lane §5의 반환 전 등록 보장은 변경 대상이 아니다. 한·영 mirror와 다른 언어 exact interface 변경은 계약의 최종 signature를 고른 뒤 이 PR의 동일 범위로 포함해야 한다. 지금 미정인 signature를 임의로 만든 언어별 파일 목록으로 구현 범위가 확정된 것처럼 쓰지 않는다. 위 목록만 승인됐다고 네 언어의 새 API signature까지 승인됐다고 해석하지 않는다.

PR 3의 첫 C++ 구현 검토 범위는 `contracts/channels/{channel.hpp,call.hpp}`, `runtime/channels/channel_runtime.cpp`, `runtime/messaging/{client_call_codec,envelope_codec}.{hpp,cpp}`, `runtime/protocol/service_wire_codec.{hpp,cpp}`, `runtime/mesh/{raw_mesh_node_owner,mesh_node_runtime}.cpp`, `runtime/stateful/public_host_runtime.cpp`, `runtime/backend/{raw_route_port.cpp,raw_binding_adapter.hpp}`다. C1/C2 알고리즘과 task allocator를 바꾸는 것을 기본 범위로 넣지 않는다. 새 binding 계약을 통해 기존 최종 receiver로 연결할 수 없는 곳은 구현 전에 blocker로 보고해야 한다.

**리팩토링 점검과 삭제 목록**

| 삭제할 기존 요청별 메커니즘 | 남길 소유자 | 계상 원칙 |
|---|---|---|
| transport 이름/variant lookup과 불변 declared-type serializer 선택의 반복 | 기존 bind endpoint와 bounded serializer cache | 정적 lookup 자체만 삭제한다. 현재 target 수락과 generation 검증은 남긴다 |
| TLS 한 항목 header plan의 반복 matching과 정적 fragment 재선택 | codec의 불변 bind/route plan | 이미 있는 cache를 두 번째 cache로 감싸지 않는다 |
| JSON header message를 만든 뒤 service payload에 다시 넣는 중간 표현 | 기존 service wire writer와 codec 조합 하나 | canonical byte 기록은 남긴다. nested wire를 없애는 절감이 아니다 |
| raw bool admission → host submit result → mesh observer의 값 변환 chain | binding stage를 연결하는 기존 최종 receiver | ccall2에서 제거한 부분은 새 절감 0으로 계산한다. intrusive owner/pool 교체를 제안하지 않는다 |
| caller stack의 실제 dynamic encode·pack 실행 | 기존 codec 소유 모듈을 실행하는 준비 자원 | 실행 위치만 바뀐 W이며 CPU 삭제가 아니다 |
| per-request submit worker 게시를 더하는 우회 | 해당 경로를 만들지 않고 하위 계층 기능을 사용한다 | 존재하지 않는 queue를 삭제 절감으로 세지 않는다 |

outer `_socket_mutex`는 binding의 physical 직렬화와 wrapper 객체 수명 보호를 구분해야 한다. 현재 `raw_route_port.cpp:375`의 잠금을 Core lifecycle gate가 있으니 무조건 지운다고 하지 않는다. wrapper 파괴와 빌더 접근 수명을 기존 socket owner 하나로 증명한 경우에만 삭제할 수 있고, 이번 caller 예산의 확정 절감에는 넣지 않았다.

수정 전/후 규칙 수의 목표는 C1 terminal **1→1**, deadline 만료 **1→1**, physical admission·재제출 **1→1**, 최종 typed 전달 **1→1**이다. static header plan 선택 위치는 요청별 TLS 선택에서 bind/route codec 소유 한 곳으로 바꾼다. 새 Framework terminal flag, lease-valid flag, inflight counter, capacity counter, native pending map, retry queue, poller는 **추가 0개**가 설계 조건이다. 새 reservation의 identity는 기존 native operation/completion identity를 대체해야 하며 두 번째 ID 표나 수락 여부 flag를 병행하지 않는다. Core/body 준비 상태를 표현하기 위해 새 flag/counter가 필요하다면 무엇을 지우는지 제시하기 전에는 구현 승인 대상이 아니다. 이번 연구가 그 미구현 상태 표현을 검증했다고 주장하지 않는다.

리팩토링 점검은 요청별 메커니즘 6개를 분류했고, 실제 수정 0개다. 삭제 후보 4개, 이동만 하는 것 1개, 새로 만들지 않을 우회 1개다. 새 상태·flag·counter를 기존 것과 함께 더하는 안은 없다. source 총 CPU의 D/G/H와 native admission 분리의 구현 가능성은 미해결이며, 이를 별도 이슈·ledger·spec 수정으로 넘기지 않고 이 보고서에 남겼다.

**구현 뒤 필요한 검증과 이번 연구의 종료 범위**

기존 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_{operation_registry,state_lane,request_transport,request_native_counts}.cpp`, `Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp`, `bindings/cpp/tests/contract/test_cpp_contract_request_reply.cpp`의 계약을 출발점으로 사용한다. 이 목록은 실행 결과가 아니다.

정상 입력은 즉시 admission, 즉시 reply, BACKPRESSURED→WRITABLE→admission, 동일 full OperationId 경쟁, callback 차단 중 다른 완료 진행, permit 0의 completion, timeout/close와 미공개 body 경쟁을 확인해야 한다. 반대 입력은 등록 전 공개, enqueue를 admitted 성공으로 치환, 다른 target의 WRITABLE로 재시도, admitted 실패와 다른 원인의 reply 실패, source 수락 순서 역전, deadline 재시작, public 완료 뒤 physical receiver 조기 해제다. 정상 입력의 성공과 반대 입력의 거부를 함께 보고해야 한다. 단순 재실행 통과로 고립 실패를 지우지 않는다.

성능 판정은 동일 harness의 caller뿐 아니라 준비 자원·receive·completion·I/O·source process 합계를 함께 봐야 한다. 150k로 pacing한 binding과 정확히 같은 rate를 쓰며, I/O rate 효과를 한 번만 반영한다. 이 연구에서는 build·test·측정 실행이 금지되어 있으므로 이 검증을 하지 않았다. 결과는 코드 구현이 아닌 조건부 설계와 반려 근거다. 남은 실패는 runtime test 실패가 아니라 **현행 binding의 기능 부재, D 결정 미확정, 새 비용 G/H 및 전체 CPU 조건 미입증**이다.

보고서의 산술만 확인했다. caller 7단계 합계 6.50과 기존 단계 합계 16.61을 확인했고, 첫 단계 또는 binding 예산을 0.01씩 변조한 두 입력은 합계 불일치로 거부했다. 이는 runtime 검증이나 인계 비용 측정이 아니다.
