1. 판정: 수신 역할 인계만으로 여섯 셀 모두 0.6을 달성한다는 권고는 할 수 없다. 포화 요청 두 셀과 1KiB send는 caller 비용이 유지되는 모델에서 미달한다.
2. request-backpressure 1KiB: 보수적 계획값 0.205, caller 용량 상한 0.226. 요청 site 수정 뒤 CPU를 대입해도 상한은 0.232다.
3. request-backpressure 4KiB: 보수적 계획값 0.294, caller 용량 상한 0.319다. 전달 제거만으로 0.6에 도달하지 않는다.
4. request-serial 1KiB: 이득을 계상하지 않은 계획값 0.514. 왕복 순지연 5–15µs 감소를 가정하면 0.535–0.582이며, 0.6에는 18.416µs 감소가 필요하다.
5. request-serial 4KiB: 이득을 계상하지 않은 계획값 0.549. 같은 5–15µs 가정이면 0.571–0.620이며, 0.6에는 11.128µs 감소가 필요하다. 도달 가능성은 있으나 미검증이다.
6. send-saturation 1KiB: 보수적 계획값 0.280, caller 용량 상한 0.329. 요청 site 수정 뒤 CPU를 대입해도 상한은 0.331이다.
7. send-saturation 4KiB: 보수적 계획값 0.513, caller 용량 상한 0.624. 여유가 작고 수신·전송 자원의 비용이 미분해 상태여서 0.6 달성은 미확인이다.
8. 명세 충돌: 조건을 지킨 수신 역할 인계안은 0건이다. 별도로 검토한 단축 변형 7개는 각각 명시 조항과 충돌한다. 동일 stack의 즉시 사용자 호출은 허용되지 않는다.
9. 추천: target만 바꾸는 작은 배치 실험으로 직렬 요청 지연을 먼저 확인한다. 여섯 셀 0.6을 전제로 한 통합 실행기 재작성은 추천하지 않는다.
10. 남은 비용: caller의 encode·등록·binding submit·결과 관찰, Core 전송, 역할 인계 통지가 남는다. 아래 수치는 실측, 조건부 상한, 가정별 계산을 구별한다.

# C++ 요청 수신 역할 인계 검토

작성일은 2026-11-07이다. 대상은 issue #1639의 0.33.0 성능 조사이며, `/root/worktree/zlink-3867h`의 현재 파일을 읽었다. 코드는 바꾸지 않았다. 이 문서에서 수신 역할 인계는 수신을 맡았던 자원이 역할을 다른 빈 자원에 넘기고, 기존 owner queue 또는 completion dispatcher가 허용한 새 실행 turn을 직접 실행하는 배치 변경을 뜻한다. Core의 입출력 thread와 caller의 송신 경로는 유지한다.

## 근거와 적용 범위

먼저 읽은 자료는 [기존 실행 경로 조언](../cadvice/report.md), [기존 C++ CPU 프로파일](../cprof/report.md), [요청 site 수정 결과](../csites/report.md)다. 기존 조언은 `zlink-3867a`의 소스와 main의 프로파일을 사용했다. 그 줄 번호와 23.405µs caller 비용을 현재 트리의 측정치로 쓰지 않았다. 현재 트리의 함수·줄 번호를 다시 확인했다.

추가로 사용자가 제시한 여섯 비율의 원본인 `/root/jobs/fx/cx/ratio044cpp/table.md`와 `/root/worktree/zlink-ratio040/framework/bench/grpc/log/ratio044cpp/cpp/run{1,2,3}/*/results.json`을 읽었다. 세 번의 결과에는 caller thread CPU 사용량, source 전체 CPU, target 전체 CPU가 있다. 별도 프로파일 없이 여섯 셀의 caller 수요를 다시 계산할 수 있다. [계산과 원본 경로](calculations.json)에 각 실행과 계산식을 보존했다.

자료의 계보는 구별해야 한다. `ratio044cpp/preflight.log`는 측정 사본의 commit을 `00f9963cc9`로 적고, `csites/provenance.json`은 `perf/3867-cpp-request-sites`의 기준을 `0cd1eac0c5fb367d0373f882094363a556b47478`로 적는다. 요청 site 수정 뒤의 별도 결과는 1KiB request-backpressure 0.208714, send-saturation 0.280564다. 따라서 여섯 baseline 비율을 #3957 실행 파일의 동일성 증거로 사용하지 않는다. 사용자가 제공한 비교 기준으로 사용하고, #3957의 측정 근거가 있는 두 셀에는 그 CPU도 별도로 대입했다. 나머지 네 셀의 #3957 전용 thread 프로파일은 제공 자료에서 확인하지 못했다. 작업 중 git 명령은 사용하지 않았다. 읽은 핵심 구현의 [SHA-256](source-sha256.json)을 기록했다.

새 benchmark, 프로파일, build, test, GitHub 작업, 다른 모델 호출과 sub-agent 실행은 하지 않았다. `measure.lock`도 획득하지 않았다. 내부 문서 동기화 스크립트는 git pull과 파일 복사를 수행하므로 실행하지 않고, 기존 `/root/zlink-docs/zlink/AGENTS.md`를 읽었다. 쓰기는 이 `crtc` 디렉터리 안의 보고서·계산·파일 해시에 한정했다. 조사 중 확인한 MemAvailable은 약 25.75–25.88GiB였으며, 큰 프로파일을 메모리에 올리지 않았다. 연속 메모리 계측을 수행했다는 뜻은 아니다.

명세 경로는 아래에서 `framework/doc/framework/common/spec/server/`를 기준으로 적는다. C++ 구현 경로는 별도 표시가 없으면 `/root/worktree/zlink-3867h/framework/languages/cpp/framework/`를 기준으로 적는다. 코드의 함수명·타입명은 그대로 쓴다. `C1`은 원자적으로 terminal 권한을 꺼내는 표, `C2`는 state lane이 소유하는 deadline 인덱스 등 여러 필드 상태라는 명세의 이름이며, 두 표가 같은 완료를 각각 결정한다는 뜻이 아니다.

## 현재 실행 경로와 제거 대상

### 포화 요청: request-backpressure

성립한 peer의 node routing ID를 직접 지정하는 성공 경로다. ChannelName의 가중 선택, local target, 이동, 오류 처리는 이 정상 경로에 없다. 1KiB와 4KiB는 같은 함수들을 지난다.

```text
source caller
  typed encode → operation owner turn → 등록 → binding request.async()
  → Core 송신 → network → Core 수신
  → target Framework 수신 역할
  → [Framework 전달 1] application worker
  → typed decode → handler → reply encode → binding reply.submit()
  → Core 송신 → network → Core 수신
  → source Framework 수신 역할이 binding poller completion을 drain
  → physical 분류 → logical terminal 권한 꺼내기
  → [Framework 전달 2] process completion worker
  → service wire·envelope·typed reply 변환 → 최종 task 확정
  → [caller 관찰 전달 3] benchmark의 완료 queue·조건 변수 → source caller의 검증·다음 제출
```

| 단계 | 현재 파일:줄과 함수 | 실행 자원과 전달 여부 |
|---|---|---|
| Caller 제출 | 저장소 기준 `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:878` `submit`, `:889` `run_tasks`, `:940` 실제 제출; `src/runtime/channels/channel_runtime.cpp:1732` 요청 제출 진입 | payload 구성, serializer, timeout 설정은 caller에서 시작한다. Framework 송신 worker로의 필수 전달은 없다. |
| 최종 수신자 준비 | `include/zlink/framework/contracts/dispatch/request_task.hpp:480` `receiver`; `src/runtime/mesh/mesh_node_runtime.cpp:4407` `submit_node_request_reply`, `:4414` callback capture | 제출 전에 최종 typed 수신자를 준비한다. #3957의 descriptor·completion 저장소 재사용이 들어간 부분이다. 별도 decode executor가 아니다. |
| 소유 turn·등록·submit | `src/runtime/mesh/raw_mesh_node_owner.cpp:1498` `request_with_header`, `:1519` `run_sync`, `:1543` `register_operation`, `:1555` `start_admitted_request`; `src/runtime/backend/raw_route_port.cpp:351` `request_impl`, `:375` socket mutex, `:390` `submit_request_once` | 정상 시 같은 caller가 turn을 얻어 binding을 호출한다. 경합 시 FIFO turn 승인을 기다릴 수 있으나, 이것을 매 요청 caller→송신 worker 전달로 세면 안 된다. |
| Target 수신·queue 등록 | `src/runtime/mesh/mesh_node_host_service.cpp:2123` `node_receive_role_t::run`, `:2132` `rotate`, `:2176` `dispatch_hosted_ready`, `:2257` application record 분기, `:2266` mailbox enqueue | 수신 역할이 header를 분류하고 owner queue에 record와 permit을 넘긴다. typed handler는 이 분기에서 실행하지 않는다. |
| 전달 1 게시 | `src/runtime/mesh/mesh_node_host_service.cpp:2318` ready 묶음 추출, `:2363` 알림; `src/runtime/mesh/service_mailbox.cpp:497` `take_application_receive_turn`, `:516` `notify_application_ready`; host service `:3319` `submit_owner`, `:3323` `try_submit_internal` | **Framework가 추가한 자원 전달이다.** Owner별 drain을 application executor에 게시한다. record마다 별도 handler task를 만든다는 뜻은 아니다. |
| Handler 실행 | host service `:2090` `make_application_owner_work`, `:3805` `drain_application_owner`, `:3821` drain 획득, `:3828` `dispatch_application`; `src/runtime/mesh/mesh_record_dispatcher.cpp:31`; `src/runtime/channels/route_packet_dispatcher.cpp:348` `dispatch_request`, `:416` completion frame; `route_handler_invoker.cpp:10` `invoke`, `:26` handler entry, `:41` filter pipeline | 기존 application 문맥, scope와 permit 반환 경로를 사용한다. 수신 역할 인계 후에도 이 진입을 그대로 사용해야 한다. |
| Handler의 reply | `route_handler_invoker.cpp:63` 완료 turn의 reply 처리; `route_packet_dispatcher.cpp:427` `route_request_completion_t::operator()`; `channel_reply_writer.cpp:12`; `src/runtime/mesh/mesh_record_dispatcher.cpp:61` reply callback → `src/runtime/stateful/public_host_runtime.cpp:9918` `host::reply`, `:8686` `reply_parts` → `src/runtime/mesh/raw_mesh_node_owner.cpp:1989` `reply`, `:1997` port snapshot, `:2004` port 호출; `src/runtime/backend/raw_route_port.cpp:501` `reply`, `:516` `submit` | **이미 같은 handler 완료 자원에서 호출한다.** 별도 reply worker는 없다. 비동기 handler가 suspend하면 정상 continuation이 완료한 자원에서 이 단계를 실행한다. |
| Source binding 완료 | 저장소 기준 `bindings/cpp/src/Runtime/Eventing/poller.cpp:404` `completion_owner->drain`; `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:668` `drain_impl`; Framework `src/runtime/backend/raw_binding_adapter.hpp:147` `observe_request_completion`, `:186` reply 관찰 | Poller를 구동하는 수신 자원에서 binding observer가 실행된다. 이 내부 observer의 실행은 사용자 continuation 실행 권한을 주지 않는다. |
| 첫 terminal 확정 | `src/runtime/mesh/raw_mesh_node_owner.cpp:1369` `observe_request`, `:245` `deliver_request_terminal`; `src/runtime/foundation/operation_registry.cpp:539` `take`, `:557` `complete` | 완료 알림 자리에서 기존 원자적 꺼내기가 이긴 결과를 정한다. operation lane으로 왕복하지 않는다. |
| 전달 2 게시 | `operation_registry.cpp:269` `post_completions`, `:281` `dispatch_completion(false, owner, dispatcher)`; `src/runtime/dispatch/provider_task_context.cpp:174` `dispatch_completion`, `:187` 게시, `:188` completion 문맥 | **Framework가 추가한 자원 전달이다.** 이미 등록된 item 하나가 독립된 completion turn을 얻는다. |
| Typed 결과 확정 | `mesh_node_runtime.cpp:4417` callback, `:4430` `decode_application_part_views`; `request_task.hpp:486` projection, `:495` physical task 확정, `:496` public task 확정 | 같은 callback 안의 값 변환 경로다. task가 두 개 보인다는 이유로 terminal 판정자가 두 개라고 세지 않는다. 논리 terminal 판정은 기존 registry가 소유한다. |
| 전달 3·caller 관찰 | `include/zlink/framework/contracts/dispatch/task.hpp:462` `deliver`, `:622` `complete`, `:721` `on_completed`, `:1421` `observe_task_completion`; 저장소 기준 `framework/bench/grpc/cpp/common/bench_async.hpp:41` `notify`, `:53` queue·조건 변수 알림; client `:953` 결과 검증 | 이 benchmark에서는 completion turn 안에서 observer를 처리할 수 있고, 그 observer가 caller queue를 깨운다. **별도 Framework decoder worker 전달은 아니다.** 마지막 queue는 benchmark가 소유한다. application이 선택한 scheduler의 전달도 별도 기록 대상이다. |

입장 완료 단계의 부수 경로도 있다. Client `:942`–`:946`은 앞 요청의 `admitted`를 이미 준비됐는지와 무관하게 관찰해 같은 완료 signal에 알린다. 준비된 값이면 `task.hpp:778`–`:787`에서 그 값을 등록한 문맥에서 직접 소비할 수 있고, pending이면 captured scheduler 또는 completion dispatcher 경로를 사용한다. 이 알림을 제거하려면 benchmark 또는 해당 관찰 연결의 변경이 필요하다. 요청을 socket에 보내기 위해 반드시 거쳐야 하는 worker 전달로 세지 않는다.

### 직렬 요청: request-serial

Client `bench_cpp_client.cpp:1014`의 window 1을 사용하며, 위 요청 경로와 함수가 같다. 차이는 하나를 제출한 caller가 다음 요청을 제출하지 못하고 `:995`–`:996`의 `completion_signal_t::wait_until`에서 대기한다는 점이다. 따라서 전달 1과 2의 queue·wake 지연이 왕복 지연에 더 직접적으로 나타나고, 전달 3의 caller wake도 왕복에 남는다. 포화 요청에서 측정한 스레드 전환 횟수를 직렬 요청에 그대로 대입하지 않는다.

Target에서 역할 인계가 성공해 전달 1을 없애고 source에서도 전달 2를 없애더라도, 실제 benchmark caller는 여전히 다른 thread다. 최종 typed task 확정과 함께 signal queue를 알리는 일은 한 completion turn에서 처리할 수 있지만, caller가 잠들어 있다면 그 wake 자체는 필요하다. “source 수신 thread가 caller에게 바로 전달”은 그 thread가 caller의 다음 제출·검증 코드를 대신 실행한다는 뜻이 아니다.

### One-way send: send-saturation

```text
caller encode → operation owner turn → binding send.async()
  → Core 송신 → network → Core 수신 → target 수신 역할
  → [Framework 전달 1] application worker → decode·one-way handler

즉시 admission: caller가 ready 결과 소비; reply·pending registry·completion 전달 없음
pending admission일 때만: source 수신 역할의 binding 완료
  → 기존 registry → [Framework 전달 2] completion turn → pending send 관찰
```

`src/runtime/mesh/raw_mesh_node_owner.cpp:1599` `start_send`, `:1612` `start_send_on_lane`, `:1702` `send_with_header_result`, `:1738` 제출과 `src/runtime/backend/raw_route_port.cpp:150` `submit_send_impl`, `:183` socket mutex, `:199` binding async가 caller 경로다. Target은 같은 mailbox·drain 경로를 지나 `route_packet_dispatcher.cpp:342` 부근 send invoker를 실행한다. Client `bench_async.hpp:46`은 ready send를 caller의 완료 목록에 바로 넣는다. 모든 send에 요청과 같은 completion worker 비용을 붙이는 계산은 잘못이다. 기존 프로파일에서 pending 비율을 별도로 측정하지 않았으므로 0이라고도 단정하지 않는다.

### 제안 배치의 정확한 의미

Target은 수신 회전의 claim·분류·queue 등록·관리 검사를 끝낸다. 기존 executor가 지금 idle인 자원 하나를 다음 수신 역할에 독점 배정한 경우에만, 이전 수신 자원이 기존 worker의 새 application turn으로 들어간다. 방금 받은 record를 owner queue 밖에 보관했다가 먼저 실행하지 않는다. queue에서 실제로 획득한 record가 그 자원에서 실행된다. 선행 record나 다른 worker와의 경쟁이 있으므로 “항상 내가 방금 받은 request를 실행한다”는 보장은 만들지 않는다.

Source도 binding drain과 완료 권한 확정·게시 호출이 반환된 뒤 같은 방법으로 역할을 넘긴다. 기존 dispatcher에 등록한 독립 item을 이전 수신 자원이 새 completion turn에서 실행하면 service wire·typed decode와 task 확정을 그 자원에서 끝낼 수 있다. 다른 completion item은 계속 다른 자원이 실행할 수 있어야 한다. 수신 stack 안에서 callback을 호출하는 변경이 아니다.

동기적으로 끝나는 benchmark handler라면 이전 수신 자원이 handler·reply encode·reply socket 호출을 끝낼 수 있다. 일반 비동기 handler 전체를 최초 수신 OS thread에 고정한다는 보장은 제안하지 않는다. Suspend 뒤에는 기존 continuation·gate 경로가 책임을 잇고, 그 완료 turn에서 reply를 시작한다. 이를 넘어선 thread affinity를 새로 넣으면 별도 scheduler와 규칙이 생긴다.

현재 `src/runtime/execution/state_lane.hpp:289` `run_sync`는 caller가 자기 동기 작업을 FIFO turn에서 실행하는 경로다. 이를 socket mutex만으로 대체하지 않는다. 현재 `offload_executor.cpp:172` `start_role`은 role을 queue에 넣고 알릴 뿐이다. `:330`–`:370`에서 worker가 나중에 가져간다. `_idle_workers`가 양수라는 사실, `try_submit_internal` 성공, role 게시 성공은 빈 자원의 독점 배정 증거가 아니다. 현재 `node_receive_role_t::run:2125`–`:2128`은 stop까지 역할을 유지한다. 선택적 인계가 구현돼 있지 않다는 사실 자체는 명세 위반이 아니다. 명세는 배정을 보장할 수 없으면 기존 worker 전달을 허용한다.

## 명세 조항별 판정

판정은 다음 세 가지다. **그대로 유지**는 원래 소유자와 규칙을 바꾸지 않는다는 뜻이다. **해석을 명시해 유지**는 같은 thread·직접 전달 같은 표현의 범위를 정해야 한다는 뜻이다. **충돌**은 아래 별도 변형 표에 원문을 인용한다. 범위의 첫 줄 번호를 적고 관련 끝 줄을 설명에 표시했다. “새 실행 turn”은 논리 실행 경계이고 OS context switch와 같지 않다.

| 접점 | 명세 파일:줄 | Target 판정 | Source 판정과 필요한 해석 |
|---|---|---|---|
| Runtime과 binding 책임 | `01-execution/08-messaging-hot-path.ko.md:23`–`:33`; `bindings/doc/spec/async-execution-model.ko.md:67`–`:75`(저장소 기준) | 그대로 유지. Framework 배치만 바꾼다. | 그대로 유지. binding이 물리 완료·재제출을 소유한다. Poller 수신 자원만 인계한다. |
| 제출 자원·등록·전송 순서 | `01-execution/08-messaging-hot-path.ko.md:68`, `:96`–`:100`, `:102`–`:120`, `:125`–`:129`; `01-execution/06-state-ownership-and-lanes.ko.md:230`–`:255` | 그대로 유지. handler가 새 요청을 보낼 때도 같은 규칙이다. | 그대로 유지. caller의 encode, 필수 소유 turn, 등록 후 submit, ready admission 소비는 이동하지 않는다. |
| Admission 단계·결과 격리 | `01-execution/01-submit-and-completion.ko.md:61`–`:83`, `:118`–`:125` | 그대로 유지. 수신 역할 배치가 원격 handler 완료를 one-way admission 조건으로 만들지 않는다. | 그대로 유지. admitted와 reply는 각각 한 번 끝나고 admission 실패는 reply에도 같은 원인으로 전달한다. 호출 간 취소·소비·관찰 상태를 공유하지 않는다. Application 완료 소스의 재개는 기다린 문맥을 따른다. |
| HWM·연결 준비·오류 분류 | `01-execution/01-submit-and-completion.ko.md:182`–`:245`, `:569`–`:589` | 그대로 유지. ordinary ingress와 source-local admission은 서로 다른 authority다. | 그대로 유지. binding operation을 한 번 시작하고 BACKPRESSURED일 때만 admitted를 소비한다. request slot 포화의 Unavailable, shutdown의 ShuttingDown, 초기 연결 준비와 잔여 deadline을 유지한다. 새 retry waiter·send backlog·target 재선택 없음. |
| 직렬 실행기 진입과 수명 | `01-execution/07-serial-executor-layers.ko.md:38`–`:105`, `:110`–`:150` | 그대로 유지. Spot·Actor·session 조율자와 queue 수명, 실행 mode별 진입점을 재사용한다. 배치 소유자가 새 Actor queue를 보유하지 않는다. | 그대로 유지. Yield 재개도 기존 조율자와 queue를 사용한다. |
| 최종 typed 수신자와 late physical 결과 | `01-execution/01-submit-and-completion.ko.md:331`–`:340` | 그대로 유지. target의 reply 생성·수명도 소유 경로를 유지한다. | 그대로 유지. public 결과가 먼저 끝났어도 인도된 physical 결과의 codec·route 처리와 한 번의 해제는 수행한다. callback 배치 변경으로 이를 생략하지 않는다. |
| 한 절대 deadline | `01-execution/01-submit-and-completion.ko.md:357`–`:365`, `:453`–`:456`, `:475`–`:488`; `08-messaging-hot-path.ko.md:197`–`:205` | 그대로 유지. handler 지연도 원 timeout 안이다. | 그대로 유지. 인계 대기나 local 실행 때문에 deadline을 재시작하지 않는다. 한 인덱스가 관리 방문을 정하고 reply마다 별도 만료 판정을 하지 않는다. |
| 첫 결과·identity·등록 선행 | `01-execution/01-submit-and-completion.ko.md:371`–`:373`, `:392`–`:405`, `:412`–`:429` | 그대로 유지. reply route와 operation identity를 보존한다. | 그대로 유지. 원자적 take만 terminal 권한을 결정한다. 같은 thread에서 decode한다고 두 번째 completed flag나 pending table을 만들지 않는다. |
| Terminal 표와 deadline 인덱스의 분리 | `01-execution/01-submit-and-completion.ko.md:447`–`:473` | 그대로 유지. | 그대로 유지. terminal take는 완료 알림 자리, 인덱스 정리는 operation lane이다. callback 반환까지 dispatcher 자리를 유지하고 그 뒤 인덱스 참조가 callback·payload를 붙잡지 않게 한다. |
| 잠금·게시·새 completion turn | `01-execution/01-submit-and-completion.ko.md:492`–`:519`; `01-execution/06-state-ownership-and-lanes.ko.md:214`–`:217` | 해석을 명시해 유지. handler 진입 전 수신 turn·lane scope를 끝낸다. | 해석을 명시해 유지. 같은 OS thread에서 새 completion turn은 가능하다. producer stack과 lane-current scope가 반환된 뒤 시작해야 한다. task 확정이 사용자 continuation을 깨울 수 있다는 점까지 적용한다. |
| Dispatcher의 무실패 수락·독립·수명 | `01-execution/01-submit-and-completion.ko.md:521`–`:544`; `08-messaging-hot-path.ko.md:324` | 그대로 유지. application 수용 한도와 outbound 완료 자리를 합치지 않는다. | 그대로 유지. 이미 예약한 item을 사용하고 siblings를 독립 게시한다. 한 callback의 blocking·예외로 뒤 완료가 묶이면 안 된다. 수락한 callback 반환 후 drain 예약을 해제한다. |
| Handler turn·gate 범위 | `01-execution/02-handler-turn-and-execution-gate.ko.md:78`–`:108` | 그대로 유지. Node·Channel에는 gate를 추가하지 않는다. SpotWide·PerActor 등 기존 gate 범위를 유지한다. | 그대로 유지. 일반 await는 기존 handler turn의 권한을 유지한다. completion 자원의 변경은 gate 획득을 대신하지 못한다. |
| Owner별 순서 | `01-execution/02-handler-turn-and-execution-gate.ko.md:97`–`:105`; `04-application-job-queue-and-backpressure.ko.md:189`–`:224`; `08-messaging-hot-path.ko.md:229`–`:241` | 해석을 명시해 유지. FIFO는 Node·Channel queue에서 꺼내는 순서다. filter·handler 본문 진입과 완료 순서까지 FIFO로 강화하지 않는다. 다른 worker의 병렬 진입을 막지 않는다. | 그대로 유지. 요청 간 결과 완료 순서를 새로 보장하지 않는다. 원 caller의 gate·queue·scheduler 규칙만 적용한다. |
| Ordinary admission·permit 반환 | `01-execution/04-application-job-queue-and-backpressure.ko.md:114`–`:125`, `:264`–`:285`; `08-messaging-hot-path.ko.md:184`–`:187`, `:222` | 그대로 유지. receive 전 permit, owner queue로 이전, 실제 handler 첫 instruction 직전 반환이다. 인계 성공 시점에 먼저 반환하지 않는다. | 그대로 유지. pre-receive에 식별된 terminal 완료는 ordinary permit과 독립이다. deadline 처리는 application gate·permit을 기다리지 않는다. |
| 수신 회전·관리·cursor | `01-execution/08-messaging-hot-path.ko.md:183`–`:211`, `:325`; `04-application-job-queue-and-backpressure.ko.md:293`–`:309` | 그대로 유지. 최대 64건, byte·시간 한도, cursor, 회전당 한 번 관리 검사를 보존한다. | 그대로 유지. 인계는 회전 끝에서 한 번 판단한다. completion마다 자원을 돌리지 않는다. |
| Decode와 payload 수명 | `01-execution/08-messaging-hot-path.ko.md:220`–`:223`; `05-payload-ownership-and-codec.ko.md:132`–`:173`; `04-application-job-queue-and-backpressure.ko.md:320`–`:329` | 해석을 명시해 유지. 수신 callback은 소유권을 옮기고 반환한다. 그 뒤 역할을 놓은 같은 자원이 기존 application 진입에서 권한을 얻고 decode한다. | 해석을 명시해 유지. requester의 reply decode는 completion 자원에서 가능하다. completion 자원은 역할을 놓은 이전 수신 자원일 수 있다. 각 codec은 한 번만 해석한다. |
| Reply 위치 | `01-execution/08-messaging-hot-path.ko.md:223`, `:239`–`:241` | 그대로 유지. completing handler turn에서 encode와 binding reply를 시작한다. 새 reply queue는 없다. | 그대로 유지. reply socket과 별도 Completion connection의 의미를 바꾸지 않는다. |
| Scope·handler 수명·동시성 | `01-execution/08-messaging-hot-path.ko.md:243`–`:289`; `00-foundation/06-framework-api.ko.md:449`–`:498`, `:500`–`:569` | 그대로 유지. 공유 drain, 요청별 독립 invocation scope, handler·filter의 기존 생성 계획과 수명을 유지한다. 실행 자원 수를 줄이려고 다른 gate의 동시성을 줄이지 않는다. | 그대로 유지. callback 저장소의 해제는 예약·수신자 수명에 따른다. 서로 다른 요청의 결과 상태를 공유하지 않는다. |
| Yield | `01-execution/02-handler-turn-and-execution-gate.ko.md:113`–`:131`, `:154`–`:197`, `:565`–`:580`; `08-messaging-hot-path.ko.md:119`–`:120` | 그대로 유지. 허용된 SpotWide·Instance만 shared gate를 놓는다. Actor claim은 유지하고 재개 record 하나를 원 queue 뒤에 넣는다. Node·Channel에서 Yield를 새로 허용하지 않는다. | 그대로 유지. 완료 수신 자원이 Yield continuation을 직접 우회 실행하지 않는다. queue·gate 재획득은 기존 scheduler가 소유한다. |
| 재진입·handler 내부 request·blocking | `01-execution/02-handler-turn-and-execution-gate.ko.md:295`–`:314`; `01-submit-and-completion.ko.md:142`–`:146`; `06-state-ownership-and-lanes.ko.md:170`–`:173` | 그대로 유지. 다른 gate·remote로의 허용된 async request는 가능하다. 자기 Actor 또는 같은 gate 결과를 현재 turn을 유지해 기다리면 submit 전에 InvalidOperation이다. 동기 blocking terminal도 runtime 문맥에서 즉시 거부한다. | 그대로 유지. 기존 ambient context를 복원해야 같은 검사를 수행한다. 임의 사용자 blocking이 생겨도 그 worker만 점유하고 수신 역할은 후속 자원에서 계속 진행해야 한다. |
| Gate 뒤처리·공정성 | `01-execution/02-handler-turn-and-execution-gate.ko.md:319`–`:379`, `:394`–`:417` | 그대로 유지. application/lifecycle FIFO·우선순위·양보 부채·기본 10ms 예산, gate 해제 전 뒤처리를 보존한다. 같은 owner 재선택으로 worker 예산을 초기화하지 않는다. | 그대로 유지. continuation도 원래 turn·lifecycle 순서를 우회하지 않는다. |
| Application과 infrastructure 분리 | `01-execution/02-handler-turn-and-execution-gate.ko.md:23`–`:39`, `:431`–`:455`, `:490`–`:515`; `08-messaging-hot-path.ko.md:322`–`:324` | 해석을 명시해 유지. thread 종류로 영역을 정하지 않는다. 수신 역할을 놓고 기존 문맥 진입을 거쳐야 application을 실행한다. | 해석을 명시해 유지. blocking callback이 있어도 새 수신 자원의 완료 확정·관리 진행은 계속된다. 모든 application 자원이 blocking일 때 새 사용자 callback까지 진행한다는 보장은 추가하지 않는다. |
| Cancellation과 send 특례 | `01-execution/03-cancellation-and-shutdown.ko.md:19`–`:35`, `:39`–`:59`, `:63`–`:76`; `01-submit-and-completion.ko.md:280`–`:295`; 저장소 기준 `bindings/doc/spec/async-execution-model.ko.md:129`–`:152` | 그대로 유지. 원격 handler 업무를 rollback하지 않는다. | 그대로 유지. C++ public cancellation 입력은 새로 만들지 않는다. owner 종료 cancellation·timeout·shutdown은 기존 terminal 경쟁을 쓴다. task를 버리는 것은 cancellation이 아니다. send에는 timeout·caller cancellation을 추가하지 않는다. |
| Shutdown·drain | `01-execution/03-cancellation-and-shutdown.ko.md:115`–`:150`; `05-location-relocation/05-host-relocation-flow.ko.md:737`–`:771`, `:798`–`:815`; `01-submit-and-completion.ko.md:539`–`:544` | 그대로 유지. host seal → accepted handler·completion drain → closing callback → scope·record·descriptor·listener·transport 정리 순서다. | 그대로 유지. 인계 중인 role과 아직 게시하지 않은 완료도 accepted work다. 최종 수신자의 physical 처리가 끝나거나 취소·정리된 뒤 자원을 해제한다. poller·transport callback 수명 종료 전에 executor를 없애지 않는다. |
| 인계의 성공·실패·수명 | `01-execution/08-messaging-hot-path.ko.md:302`–`:338` | 해석을 명시해 유지. 기존 배치 소유자가 idle 자원을 독점 배정해야 성공이다. 실패하면 수신을 유지하고 owner queue 작업은 worker에 남긴다. | 같은 판정이다. wait·drain 반환 후 다음 자원이 같은 poller를 구동한다. poller·cursor·permit·management·shutdown 수명은 role에 남긴다. 추가 thread, 상한 증가, 새 자원 시작 확인 대기는 없다. |
| 언어 재량과 성능 기준 | `01-execution/08-messaging-hot-path.ko.md:363`–`:382`, `:396`–`:405` | 그대로 유지. 언어별 물리 자원은 달라도 된다. | 그대로 유지. 0.6은 이번 중간 목표이며 공통 명세의 0.90 기준을 바꾸지 않는다. Node는 기존 event-loop 경로를 유지한다. |

추가로 C++의 `languages/cpp/interfaces/01-common-runtime.ko.md:260`–`:306`은 process completion dispatcher 하나, host와 standalone owner의 공유, 마지막 owner의 accepted callback·후속 재개 drain과 worker join, completion turn 밖의 생성·close를 정한다. 이 조항도 그대로 유지한다. 공통 자원 배치로 바꾸더라도 standalone completion owner가 host 없이 동작하고 기존 runtime 밖 blocking 반환이 가능한 계약을 보존해야 한다. 마지막 owner가 살아 있는 외부 producer를 중단하기 전에 close하는 새 경로를 만들지 않는다.

### 충돌하는 단축 변형 7개

다음은 조건부 인계안의 필수 충돌이 아니라, 구현할 때 선택해서는 안 되는 변형이다. 각 행을 한 충돌 사례로 센다. 같은 문장이 다른 행의 이유를 함께 설명하더라도 중복 합산하지 않았다.

| 변형 | 충돌 파일:줄과 원문 | 결론 |
|---|---|---|
| binding drain callback에서 typed task를 확정하고 사용자 재개까지 즉시 실행 | `01-execution/01-submit-and-completion.ko.md:508`: “같은 실행 자원에서 callback을 실행할 때는 완료 확정·게시 처리의 호출 stack과 lane-current scope가 반환된 뒤 다음 turn에서 시작한다.” | 잠금만 풀어서는 부족하다. 기존 dispatcher item을 새 turn으로 실행한다. |
| 역할을 가진 채 handler 또는 continuation 실행 | `01-execution/08-messaging-hot-path.ko.md:309`: “수신 역할을 맡은 실행 자원은 그 역할을 다른 실행 자원에 넘긴 뒤에만 그 turn을 직접 실행한다.” | 성공한 독점 배정 없이는 local 실행하지 않는다. |
| 방금 받은 record를 owner queue·gate 밖에서 먼저 실행 | `01-execution/08-messaging-hot-path.ko.md:323`: “배치만으로 실행 권한을 얻거나 queue 순서를 건너뛰지 않으며, application 문맥에는 기존 진입 경로로 들어간다” | 기존 queue의 take·gate·claim을 사용한다. |
| 한 수신 batch의 completion callback들을 하나의 직렬 callback loop로 합침 | `01-execution/08-messaging-hot-path.ko.md:324`: “배치는 서로 무관한 완료 callback을 한 callback의 반환에 의존하는 drain으로 합치지 않는다.” | 한 callback을 붙잡아도 sibling이 진행하도록 독립 item을 먼저 공개한다. |
| 요청 또는 완료마다 역할 인계 | `01-execution/08-messaging-hot-path.ko.md:325`: “인계는 수신 회전이 끝난 경계에서 한 번 판단한다. Record마다 인계하지 않으며 I2의 연속 claim과 회전당 관리 작업을 그대로 둔다.” | 최대 64건의 기존 회전 경계를 사용한다. |
| caller가 두 번째 poller 또는 동시 drain으로 자기 완료만 읽음 | `01-execution/08-messaging-hot-path.ko.md:326`: “Socket마다 동시에 수신을 진행하는 자원은 하나다.” 저장소 기준 `bindings/doc/spec/async-execution-model.ko.md:67`도 completion owner 하나만 허용한다. | 같은 poller의 역할 이전이 필요하다. native completion을 Framework가 따로 읽지 않는다. |
| caller가 선택한 실행 context를 무시하고 수신 자원에서 continuation을 실행 | `01-execution/08-messaging-hot-path.ko.md:111`: “§4.4는 caller가 고른 실행 context를 바꾸지 않는다.” | 명시 scheduler와 Yield 재획득을 유지한다. 이때 마지막 전달은 남을 수 있다. |

### Caller가 직접 drain하는 대안

Binding 수준에서는 같은 poller의 현재 wait·drain 반환 뒤 다른 자원이 다음 wait를 구동할 수 있다(`bindings/doc/spec/async-execution-model.ko.md:70`). 따라서 “caller가 drain” 자체를 금지하는 규칙은 없다. 다만 현재 Framework의 `configuration/app.hpp:95`는 hosted `run`을 제공하고, request task나 route client는 host 소유 poller를 application caller에게 넘기는 공개 pump API를 제공하지 않는다. Benchmark binding 경로의 ready queue를 Framework sample에 복사해 host socket을 따로 구동하는 변경은 기존 공개 호출 경로가 아니다.

이를 일반 설계로 만들려면 caller가 언제 수신 역할을 독점 소유하는지, caller가 제출·업무·blocking을 하는 동안 누가 infrastructure를 진행하는지, 외부 scheduler·여러 caller·shutdown을 어떻게 처리하는지를 먼저 정해야 한다. Caller를 영구 수신 owner로 만들면 source receive·decode 비용이 현재 병목 caller로 이동한다. 새 public API가 필요하다면 공통 계약에 없는 설계 변경으로 별도 검토해야 한다. 이 보고서는 이를 바로 구현할 대안으로 추천하지 않는다. 일반적인 내부 구현 가능성까지 불가능하다고 판정하는 것은 아니다.

## CPU·전환 측정과 비율 계산

### 현재 여섯 셀의 caller와 endpoint 비용

원본 `results.json`에서 `caller CPU µs/건 = submit_thread_cores × 10^6 / throughput_per_second`로 계산했다. Source 전체는 `client_cores`에 같은 식을 적용했고, target은 `server_cpu_percent × logical_cores / 100`을 core 수로 바꿨다. 비율은 처리량의 3-run 중앙값끼리 나눴다. CPU는 각 실행에서 계산한 값의 중앙값이다. 서로 다른 중앙값이므로 합계나 utilization을 정확히 재구성하지는 않는다. 이 계수는 전체 active 구간 CPU와 active close 처리량 기준이며 message별 계측이 아니다.

| 패턴 | 크기 | Framework 건/s | Binding 건/s | 비율 | Caller CPU µs/건 | Caller core 사용 | Source 전체 CPU µs/건 | Target 전체 CPU µs/건 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| request-backpressure | 1KiB | 51,942.433 | 253,036.206 | 0.2053 | 17.480 | 0.908 | 80.896 | 60.952 |
| request-backpressure | 4KiB | 46,123.685 | 157,140.373 | 0.2935 | 19.961 | 0.915 | 89.987 | 68.039 |
| request-serial | 1KiB | 7,773.379 | 15,120.198 | 0.5141 | 19.924 | 0.154 | 82.792 | 83.356 |
| request-serial | 4KiB | 7,658.715 | 13,953.760 | 0.5489 | 20.022 | 0.154 | 89.411 | 76.526 |
| send-saturation | 1KiB | 181,239.182 | 646,563.761 | 0.2803 | 4.702 | 0.851 | 9.803 | 13.540 |
| send-saturation | 4KiB | 159,356.911 | 310,662.001 | 0.5130 | 5.156 | 0.824 | 11.408 | 16.050 |

#3957의 요청 site 수정 뒤 자료를 대입하면 1KiB request caller는 **17.047**, source 전체 **79.559**, target 전체 **57.596µs/건**이다. 1KiB send caller는 **4.673**, source 전체 **9.716**, target 전체 **13.082µs/건**이다. 기존 cprof의 23.405와 cadvice의 5.414를 여기에 다시 더하거나, 이미 줄어든 allocation을 한 번 더 절감으로 빼지 않았다. 이 두 cohort의 차이를 수신 역할 인계 효과로 해석하지 않는다.

### 기존 상세 thread 프로파일의 의미

다음 표는 이전 main의 1KiB 상세 근거다. 현재 트리의 함수 배치는 대조했지만 수치의 재측정은 하지 않았다. Queue 지연 또는 한 전달의 지연 측정값이 아니라 각 자원의 CPU 수요와 OS 스케줄링 전환 횟수다.

| 경로·자원 | CPU µs/건 | 전환/건 | 인계로 남거나 이동하는 일 |
|---|---:|---:|---|
| 요청 source caller | 23.405 | 0.0481 | encode·등록·submit·관찰 유지. 현재 자료에서는 약 17µs로 줄어 있다. |
| 요청 target 수신 | 16.986 | 0.4572 | poll·claim·관리 유지. 일부 게시 비용만 제거 후보다. |
| 요청 target application worker 합계 | 22.764 | 0.2056 | decode·scope·handler·reply가 이전 수신 자원으로 이동한다. 전체가 없어지는 것이 아니다. |
| 요청 source 수신 | 18.350 | 0.3799 | binding drain·분류·logical take 유지. 일부 dispatcher 게시만 제거 후보다. |
| 요청 source completion worker 8개 합계 | 25.984 | 1.6338 | typed decode·결과 확정·알림·해제는 이동한다. 전체를 caller CPU에서 빼면 안 된다. |
| 요청 source Core 송신/완료 수신 | 19.494 / 15.247 | 0.3598 / 0.4772 | Core 경로 유지. Framework 자원 배치가 batching·경합에 미칠 간접 효과는 미측정이다. |
| 요청 target Core 수신/송신 | 15.533 / 18.884 | 0.4613 / 0.3713 | 같은 제한이다. |
| Send source caller | 5.414 | 0.000354 | 유지. 현재 별도 자료는 4.673µs다. |
| Send target 수신 | 4.284 | 0.11521 | 수신·분류·관리 유지. |
| Send target application worker 합계 | 5.522 | 0.01034 | handler·decode는 이동한다. 원래 wake가 상당히 묶여 있다. |
| Send source Core 송신 | 4.990 | 0.09296 | 유지. |

상세 CSV의 exclusive 동기화 분류를 합하면, 요청 source 수신+completion workers **13.159µs/건**, target 수신+application workers **5.293µs/건**이다. 두 합은 **18.452µs/건**이다. 여기에는 유지할 mutex·task notification·poller 동기화도 포함된다. 이 전부를 없앨 수 있다고 가정해도 “옮기는 CPU”를 “절약하는 CPU”로 바꿀 수는 없다. Send target의 같은 합은 **0.450µs/건**이다. Source caller의 동기화 분류는 요청 **1.473**, send **0.217µs/건**이다.

이전 별도 counter 실행은 두 endpoint 전체에서 요청 **3.7749 OS 전환/건**, binding **0.2002**, futex 호출 **11.4117 / 1.6203**을 관측했다. CPU capture의 전환과 별도 counter 구간의 합은 섞지 않는다. 특히 `3.7749 × 임의 wake 지연`을 절약 시간으로 사용할 수 없다. 전환에는 Core·관리·caller wake가 들어 있고, 한 전환은 한 message의 한 edge와 일대일 대응하지 않는다. `cond_signal` 호출도 실제 OS 전환과 같지 않다.

기존 [수신 인계 진단](../chand/report.md)의 같은 계열 실행은 **241,991건 / 195,683회전 = 1.237건/회전**을 관측했고 인계 시도·성공은 구현 부재로 0이었다. 매 회전 성공하면 후속 수신 자원 알림이 약 **0.809회/건** 생길 수 있다. Source completion post 1회/건을 없애더라도 수신 인계 wake가 이를 대체할 수 있다. 이 비율은 다른 실행의 값이고 #3957의 배치 분포가 아니다. 최대 64건이므로 항상 1/64 wake라고 가정해서도 안 된다.

한 고정 thread에 수신과 모든 application을 직렬로 합치는 변형은 제외한다. 이전 요청 target 수요는 **16.986+22.764=39.750µs/건**, source 수신+completion은 **18.350+25.984=44.334µs/건**이다. 각각 단일 자원이 되면 약 25.2k/22.6k건/s에 불과하다. Send target도 **4.284+5.522=9.806µs/건**, 약 102k건/s다. 역할 인계의 목적은 수신과 application의 겹친 진행을 유지하면서 message의 자원 이전을 줄이는 것이다.

### 포화 경로의 용량 상한

Binding 처리량을 B, caller의 건당 CPU를 C, 자원 i의 건당 CPU를 d, 같은 역할의 병렬 자원 수를 n이라고 하면 다음 조건부 상한을 사용한다.

```text
비율 ≤ min(10^6/(B×C), n×10^6/(B×d), endpoint CPU 총량 상한, ...)
0.6의 caller 예산 = 10^6/(0.6×B) µs/건
0.6의 endpoint 예산 = 8×10^6/(0.6×B) CPU µs/건
```

이 계산은 서비스 수요와 B를 고정한다. 인계로 경합·배치·cache·transport syscall 비용이 달라지면 수요도 바뀐다. 따라서 물리적으로 절대 넘을 수 없는 불변 상한이라고 주장하지 않는다. 그러한 간접 절감은 먼저 측정해야 하며, 이 보고서에서 예상 이득으로 미리 계상하지 않는다.

| 포화 셀 | Caller CPU | 0.6 caller 예산 | Caller 고정 비율 상한 | 추가로 필요한 caller CPU 감소 |
|---|---:|---:|---:|---:|
| request-backpressure 1KiB | 17.480 | 6.587 | 0.2261 | 62.3% |
| request-backpressure 4KiB | 19.961 | 10.606 | 0.3188 | 46.9% |
| send-saturation 1KiB | 4.702 | 2.578 | 0.3289 | 45.2% |
| send-saturation 4KiB | 5.156 | 5.365 | 0.6243 | caller CPU만으로는 배제하지 않음 |

#3957의 직접 자료가 있는 1KiB 두 셀의 C로 바꿔도 request **0.2318**, send **0.3310**이다. 이 caller 비용은 제거 대상인 두 전달의 worker CPU가 아니다. 수신 역할 인계가 성공해 caller가 현재보다 덜 기다리더라도 CPU가 그대로라면 그 상한을 넘지 못한다.

민감도를 보기 위해 이전 프로파일에서 caller의 **동기화 비용 전부**를 현 caller CPU에서 지운, 실제보다 과도한 가정을 추가하면 request-backpressure 1KiB **0.2469**, 4KiB **0.3442**, send 1KiB **0.3448**이다. 서로 다른 cohort의 stage 수치를 빌린 계산이고, 실제 인계 효과 추정이 아니다. 그래도 0.6과 거리가 크다.

요청은 endpoint 전체도 중요하다. 1KiB 0.6에서는 endpoint당 **52.693 CPU µs/건** 안에 들어야 한다. baseline source **80.896**, target **60.952**이므로 각각 약 **34.9% / 13.5%** 감소가 필요하다. #3957 자료를 대입해도 source **79.559**는 예산을 넘는다. 4KiB 예산은 **84.850**이고 source **89.987**이 약 5.7% 초과한다. 단순히 completion worker CPU를 receive CPU로 옮겨서는 총량이 줄지 않는다.

이전 1KiB source Core 송신 **19.494µs/건**을 그대로 유지한다고 가정하면 현재 B에 대한 용량 비율은 **0.203**이고, 이전 send Core 송신 **4.990µs/건**이면 **0.310**이다. 이 값은 최신 성능과 시점이 달라 현재 상한에 다시 강제 적용하지 않았다. 이미 이전 자료보다 높은 처리량이 나오는 cohort도 있어, 이를 immutable Core 성능 한계라고 부르면 안 된다. 다만 caller만 줄여도 충분하다고 주장할 근거가 없다는 점은 남는다.

### 직렬 요청의 지연 계산

직렬 요청은 한 번에 하나이므로 전체 endpoint CPU 합을 왕복 wall time으로 쓰지 않는다. 기본 관측 간격은 `L=10^6/F`다. 인계로 절약한 게시·wake·cache 비용에서 새 역할 배정·수신 통지 비용을 뺀 **순 왕복 감소량**을 Δ라고 두면 `새 비율=10^6/[B×(L−Δ)]`다. 이 Δ의 causal 측정은 현재 자료에 없다.

| 셀 | 현재 간격 L µs | 0.6 간격 µs | 필요한 순감소 Δ µs | Δ=5µs 가정 | Δ=10µs 가정 | Δ=15µs 가정 | Δ=20µs 가정 |
|---|---:|---:|---:|---:|---:|---:|---:|
| request-serial 1KiB | 128.644 | 110.228 | 18.416 | 0.535 | 0.557 | 0.582 | 0.609 |
| request-serial 4KiB | 130.570 | 119.442 | 11.128 | 0.571 | 0.594 | 0.620 | 0.648 |

여기서 5–15µs는 측정 신뢰구간도, 전환당 비용에서 얻은 예측도 아니다. 단지 구현 판단에 사용할 민감도 범위다. 기존 포화 요청의 관련 동기화 CPU 합 18.452µs가 있다고 해서 직렬 요청에서 그만큼 wall time을 줄인다고 말할 수 없다. 반대로 off-CPU queue 대기가 더 클 수도 있어, 그 합이 직렬 지연 감소의 엄밀한 상한인 것도 아니다. Stage CPU와 지연을 분리해야 한다.

직렬 caller는 약 0.154 core만 사용한다. 이 두 셀에서는 23.4µs라는 이전 포화 caller 수치나 0.23이라는 포화 상한을 가져와 0.6 불가능이라고 결론 내릴 수 없다. 4KiB의 11.128µs 감소는 조사할 가치가 있다. 1KiB의 18.416µs 감소는 더 큰 요구이며, 기존의 남는 caller wake와 두 endpoint 역할 인계 비용을 함께 고려해야 한다.

### 여섯 셀의 예상값 판정

실험 전 계획에는 확인하지 않은 이득을 0으로 둔다. 따라서 보수적 점추정은 **0.205 / 0.294 / 0.514 / 0.549 / 0.280 / 0.513**이다. 이는 개선이 전혀 없을 것이라는 통계적 예측이 아니라, 제거 비용의 인과 측정이 없는 상황에서 약속할 수 있는 기준값이다. 인계 실패가 잦으면 이 기준 근처에 머물거나 새 bookkeeping 때문에 낮아질 수 있다.

포화 요청 두 셀과 1KiB send에서 개선될 수 있는 공간은 caller 고정 모델의 약 **0.23 / 0.32 / 0.33**까지다. 4KiB send는 **0.624**까지 caller 용량이 허용하지만, 0.6 속도에서는 caller가 약 **0.961 core**를 사용해야 한다. 현재 약 0.824에서 늘어날 여유가 매우 작다. Target 최종 소비율·source Core 송신·socket gate 경합이 이 속도를 실제로 허용하는지는 아직 분해하지 못했다. 이전 1KiB target 동기화 0.450µs를 4KiB의 약 0.910µs 간격 개선 요구에 그대로 적용해 성공을 예측할 수 없다.

따라서 **전체 목표는 미달**, **직렬 요청과 4KiB send의 개별 0.6 가능성은 열려 있으나 현재 근거로는 확인할 수 없다**는 판정이다. 특히 직렬 4KiB의 조건부 0.620을 “예상 달성값”으로 보고하면 안 된다.

0.6 전체를 추진하려면 다음 비용이 실제로 줄어야 한다. Caller의 encode·envelope·등록·task 관찰·binding submit은 현재 소유 모듈 안에서 측정해 줄이고, source 전체 CPU와 Core 전송의 batching·syscall·경합도 재측정해야 한다. 기존 공통 명세가 소유한 인덱스·terminal·admission을 생략하거나, 호출자를 여러 개로 늘려 benchmark의 한 caller 조건을 바꾸는 것은 이 목표의 해결책으로 계산하지 않는다. Core 비용이 병목으로 남으면 public binding 재현으로 하위 계층 문제를 분리해야 하며 Framework에 두 번째 전송·retry 경로를 두지 않는다.

## 다른 언어의 현재 배치

기존 조언의 언어별 설명을 현재 트리에 다시 대조했다. 특히 .NET은 이미 수신 역할 인계 구현이 있어 이전 조언보다 구체적인 비교가 가능하다.

| 언어 | Target 수신·handler·reply | Source 완료·caller 재개 |
|---|---|---|
| .NET | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:6198` `ReceiveLoopAsync`가 process receive source의 WaitAsync 뒤에 수신·관리를 진행한다. `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:116`–`:127`은 application source를 공유 실행 pool에 등록한다. `Runtime/Execution/ZLinkProcessExecutionPool.cs:578`–`:592`는 idle worker를 제거하고 `replacement.Receiving=true`, 기존 worker를 false로 바꿔 독점 배정한다. `:389`–`:402`는 ReceiveUntilHandoff가 반환되고 receive 표시가 해제된 뒤 application을 실행한다. 즉 **target 배치의 직접 비교 대상이 이미 있다.** Handler·reply는 정상 application continuation에서 진행한다. | ManagedMeshNode `:11115`의 atomic removal 후 `:11124`의 DispatchReply, `:14167`–`:14204`의 예약 item이 `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13`–`:16`을 통해 CLR ThreadPool로 간다. Physical reply 관찰·기초 변환은 `:11128`–`:11166`의 비동기 관찰 자원에서 하고 최종 caller task는 독립 completion item에서 확정한다. 일반 await·명시 scheduler·SynchronizationContext는 그 언어 규칙을 따른다. target 역할 인계가 있다고 source completion pool까지 통합돼 있다는 뜻은 아니다. |
| Java, Kotlin 공통 runtime | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5385`–`:5438`의 node별 platform-thread pump가 binding wait, claim, 관리를 진행한다. `:6054`–`:6065`의 등록된 applicationReceiver는 `runtime/channels/ZLinkMeshApplicationDispatcher.java:171`로 넘기며, 같은 파일 `:602`의 namespace queue enqueue가 handler 진입을 배치한다. receiver가 없는 fallback은 RawMeshNode `:6069`의 applicationDispatch 게시다. 기본 `binding/ZLinkProcessExecutionLanes.java:22`–`:33`은 virtual-thread executor 위 serial drain이다. Handler 완료에 연결된 `ZLinkMeshApplicationDispatcher.java:648`–`:657`, `:772`의 replyAndClose가 reply를 보낸다. 전용 수신 pump와 application 실행은 구별된다. | `runtime/internal/service/ZLinkServiceOperationRegistry.java:329` 등록·시작, `:404`–`:419` terminal take와 예약 item 게시, `:513` 공통 take를 사용한다. `ZLinkServiceCompletionDispatcher.java:51`–`:74`는 각 item을 ForkJoinPool.commonPool에 독립 게시하고 batch를 callback 하나로 직렬화하지 않는다. `:146`에서 caller future를 완료한다. CompletionStage의 동기 dependent 또는 caller가 정한 executor가 이후 재개 위치를 정한다. |
| Node.js | `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:1021` receiveBatch, `:1078` receiveOne과 `runtime/backend/node/node-raw-mesh-backend.ts:1997` mailbox drain이 같은 JavaScript event loop에서 진행한다. Application의 정상 비동기 queue·turn에서 decode·handler를 실행하며, `raw-service-mesh-runtime.ts:940`–`:958`의 reply도 같은 event-loop 자원에서 시작한다. 물리적으로 다른 idle JavaScript worker는 없다. | `runtime/backend/node/node-raw-binding-port.ts:631`–`:645`의 binding reply Promise reaction이 physical 값을 처리한다. `foundation/operation-registry.ts:332`, `:349`, `:434`의 단일 take가 terminal을 정한다. `raw-service-mesh-runtime.ts:783`–`:809`와 `node-raw-mesh-backend.ts:1871`–`:1883`는 관찰 완료를 queueMicrotask로 배달한다. Promise continuation은 같은 자원의 나중 microtask다. 같은 OS thread라는 사실은 수신 stack의 즉시 callback과 다르다. |

관찰 가능한 공통 계약은 admission 경계, terminal 하나, deadline, owner queue take 순서, gate 권한, Yield·재진입 거부, scope·payload 수명, shutdown 결과다. 수신 역할을 맡는 OS thread의 ID나 모든 handler가 처음 수신한 OS thread에서 끝난다는 보장은 공통 계약이 아니다. C++이 위 인계안을 선택하고 .NET·Java·Node가 현재 자원 배치를 유지해도, 공통 계약 변경은 필요하지 않다. Node를 위해 새 worker를 추가하거나 다른 언어의 worker 수를 맞출 이유도 없다.

다만 C++만 same-gate 중첩 request를 inline 처리하거나, timeout을 decode 시점으로 바꾸거나, public `admitted`와 reply의 격리·실패를 합치거나, caller scheduler를 무시하면 관찰 가능한 차이가 생긴다. 이는 배치 최적화가 아니라 계약 변경이다. 그런 변경은 이 안에서 제외한다. 이번 작업에서는 네 언어의 실행 test를 돌리지 않았으므로 소스 비교가 모든 기존 parity 결함이 없음을 증명하지는 않는다.

## 결정 소유와 교체 범위

아래 숫자는 함수 호출 수·allocation 수·알림 수가 아니라 **동일 결정을 내리는 소유자 수**다. 물리 배정의 2→1은 benchmark endpoint의 host 하나와 process dispatcher 하나를 기준으로 한다. 여러 host의 배정까지 process 하나로 모으는 범위라면 현재 host 수 H에 대해 H+1→1로 세어야 한다. Application과 completion의 실제 worker 배정을 합치는 구현은 가능하지만, terminal·deadline·gate·binding을 한 상태로 합치면 안 된다. 두 기존 scheduler를 유지하고 이전 수신 자원이 기존 dispatcher queue의 실행에 참여하는 구현도 가능하므로, 물리 배정의 통합이 명세의 필수 요구라고 해석하지 않는다.

| 결정 | 현재 C++ 위치 | 다른 언어의 해당 결정 위치 | 유지·집중할 소유자와 전→후 |
|---|---|---|---|
| 수신·application의 자원 배정 | `src/runtime/dispatch/offload_executor.cpp:172`, `:330`–`:370`; host service `:810`, `:3418` | .NET `ZLinkProcessExecutionPool.cs:578`; Java `ZLinkJavaRawMeshNode.java:5385`, `ZLinkProcessExecutionLanes.java:70`; Node는 event loop와 기존 ready queue | Target만 실험하면 기존 executor **1→1**. 인계 성공 여부도 그 소유자가 결정한다. host에 별도 idle-worker 명단을 두지 않는다. |
| Source 완료 자원 배정 | `src/runtime/dispatch/provider_task_context.cpp:53`–`:88`, `:174`–`:191`의 process dispatcher; 수신·application은 위 offload executor | .NET `ZLinkCompletionDispatcher.cs:13`; Java `ZLinkServiceCompletionDispatcher.java:51`; Node `node-raw-mesh-backend.ts:1871` | 공통 pool을 택하는 전체안은 영향을 받는 offload 배치와 completion 배치를 **2→1**로 집중한다. 두 pool을 유지한 dispatcher 참여안은 수신 배정 **1→1**, dispatcher item 선택 **1→1**이다. 이 둘은 다른 결정이다. 기존 두 scheduler 위에 같은 작업의 배정을 다시 판정하는 제3 broker를 얹는 구조는 제외한다. 다른 infrastructure offload 소유까지 없앤다는 뜻은 아니다. |
| 물리 completion·HWM 재제출 | binding `Eventing/poller.cpp:404`, `Messaging/completion_owner.cpp:668`; Framework `raw_binding_adapter.hpp:147` | .NET `ZLinkRequestSubmissionOutcome.cs:69`; Java `ZLinkJavaRawServicePort.java:450`; Node `node-raw-binding-port.ts:698` | Binding owner **1→1**. poller 등록·native 상태·retry를 Framework에 복제하지 않는다. |
| 논리 첫 terminal | `foundation/operation_registry.cpp:539`, `:557`, `:604`, `:790` | .NET ManagedMeshNode `:11115`; Java OperationRegistry `:404`, `:513`; Node OperationRegistry `:332`, `:349`, `:434` | 기존 registry **1→1**. 동일 entry의 take를 유지한다. |
| 등록·deadline·close 수락 | `raw_mesh_node_owner.cpp:1519`–`:1563`; `operation_registry.cpp:382`, `:300` | .NET ManagedMeshNode `:5608`; Java OperationRegistry `:329`; Node RawServiceMeshRuntime `:761`, OperationRegistry `:246` | 기존 operation state lane **1→1**. 새 deadline timer·인덱스·pending 표 없음. |
| Handler queue·gate·claim·permit | `mesh_node_host_service.cpp:3805`; `service_mailbox.cpp:321`, `:453`; 기존 serial execution 경로 | .NET MeshDispatchPump `:289`; Java MeshApplicationDispatcher `:602`; Node 기존 mailbox·owner 실행 경로 | 기존 queue·gate 소유자 각각 **1→1**. 자원 배정을 실행 권한으로 사용하지 않는다. |
| Caller continuation의 실행 문맥 | `dispatch/task.hpp:416`, `:462`–`:479`; `provider_task_context.cpp:162` | 각 언어의 Task·CompletionStage·Promise와 기존 gate 재개 scheduler | 기존 wait registration의 claim·captured context **1→1**. source completion scope를 진입해 중복 post를 줄일 수 있지만 explicit scheduler 우회 없음. |
| Accepted 완료와 role의 drain 수명 | `operation_registry.cpp:194`–`:212`, `:790`–`:825`; `provider_task_context.cpp:67`; `offload_executor.cpp:218` | .NET PendingOperation `:14207`; Java Dispatcher `:79`; Node registry close `:387` | 완료 예약·dispatcher·role은 서로 다른 수명 소유자이며 각각 **1→1**. 서로 다른 사실 세 개를 하나의 완료 flag로 합치지 않는다. |

수정 분류는 조건부 인계 구현이면 **A: 기존 계약 적응**이다. 현재 선택적 인계가 없다는 이유만으로 B: 기존 동작 결함이라고 단정하지 않는다. Spec에 없는 caller pump 표면이나 thread affinity를 요구하면 **D: 명세 공백 검토**로 바뀌며 이번 구현안에 포함하지 않는다. producer stack callback, second poller, retry 추가는 **C: 우회**로 배제한다. 소유 계층은 Framework 실행 자원 배치이고, binding·Core의 완료·전송 결정은 바뀌지 않는다.

## 권고와 조건부 구현 순서

### 비교한 두 대안

첫 대안은 **target 배치만 기존 executor 안에서 교체**하는 것이다. `node_receive_role_t`의 무한 run이 회전 결과를 바깥 executor에 돌려주게 하고, 그 executor의 단일 worker 가용 상태에서 역할 배정과 다음 application 작업 선택을 끝낸다. completion dispatcher는 그대로 둔다. 변경의 독립 범위가 가장 작고, .NET의 현재 동작을 비교 근거로 쓸 수 있다. 직렬 요청의 전달 1 지연과 send target 비용을 단독으로 검증할 수 있다.

둘째 대안은 **source까지 공통 자원 배치로 합치는 것**이다. 기존 offload worker 배정과 process completion dispatcher worker 선택을 하나의 자원 소유자 안으로 옮기되, completion dispatcher는 예약 item·새 turn·독립 실행·drain의 논리 소유자로 남긴다. 두 번째 completion queue나 receive 후 보관 queue를 만들지 않는다. Source의 typed 수신자까지 직접 이어 실행할 수 있으나, 두 독립 pool의 수명·worker 정책·상한을 보존하는 비용이 크다.

Source의 더 작은 비교안으로 **기존 dispatcher queue의 실행에 이전 수신 자원이 참여**할 수도 있다. 역할 인계와 producer stack 반환 뒤, dispatcher가 자신의 기존 queue에서 독립 item 하나를 선택하고 원래 completion scope를 진입해 실행하게 한다. 별도 queue·pending 표 없이 기존 Asio queue와 worker 정책을 사용하며, initial post가 이미 다른 worker를 깨웠을 수 있어 게시·wake 비용 제거는 보장하지 못한다. 기존 worker가 먼저 item을 가져가면 해당 완료의 자원 전달도 남는다. 이 참여안은 두 배정 소유자를 합치지 않아도 같은 자원의 새 turn이 가능한지 확인하는 비교 실험이며, “통합 실행기가 명세상 필수”라는 주장은 하지 않는다.

첫 대안의 실험을 추천한다. Source는 이 작은 참여안의 성공률·순비용부터 비교할 수 있다. 둘째 대안은 첫 실험과 source 게시 비용의 실제 측정이 이득을 입증한 뒤에 검토한다. 단지 스레드 전달 두 개를 지우면 0.6이라는 이유로 두 pool을 재작성할 근거는 없다. 현재처럼 worker에 게시하는 배치는 배정 실패 시에도 필요한 일반 경로이며, 별도 terminal 메커니즘으로 복제하지 않는다.

### 파일과 변경 순서

이 절은 구현 승인 후의 계획이다. 이번 읽기 전용 작업에서는 수행하지 않았다.

1. `src/runtime/dispatch/offload_executor.hpp`, `.cpp`에서 **기존 worker 상태의 소유자 안에** 독점 수신 배정을 둔다. 초기 role 배정과 후속 인계가 같은 배정 상태를 쓰게 한다. 현재의 queued role 수·idle 수를 보고 외부에서 성공을 추측하는 경로를 교체한다. 인계를 위해 새 thread를 만들거나 최대 worker 수를 늘리지 않는다. start 확인을 기다리지 않는다.
2. `src/runtime/mesh/mesh_node_host_service.hpp`, `.cpp`에서 `node_receive_role_t::run/rotate`를 유한 회전과 회전 밖 배치 경계로 나눈다. 지역 변수였던 다음 readiness 등 역할 연속 상태가 다음 수신 자원으로 정확히 이어지게 한다. `rotate`의 claim·관리·permit 정리가 반환된 뒤 local application turn을 실행한다. 성공 시 수신 wake를 한 번 발행하고, 실패하면 현재 역할과 기존 owner drain 게시를 유지한다.
3. `src/runtime/mesh/service_mailbox.hpp`, `.cpp`의 기존 ready 집합·record take를 사용해 local drain을 고른다. Local “방금 받은 request” 슬롯을 추가하지 않는다. 기존 `submit_owner`의 자원 선택을 단일 scheduler에 맡기고 gate·permit·admission 판정을 복사하지 않는다. `dispatch_application`, `route_handler_invoker`, `route_request_completion_t`의 handler·reply·scope 경로는 재사용한다.
4. Source의 작은 참여 실험은 `src/runtime/dispatch/provider_task_context.cpp` 안에서 기존 dispatcher queue를 실행하는 내부 진입과 completion scope를 재사용하고, host의 회전 밖 배치 경계에서만 참여한다. item을 host의 임시 queue로 옮기지 않는다. 이 실험은 post 비용을 없애는 구현이 아니므로 따로 측정한다. 첫 대안과 이 참여 실험의 측정이 유효하면, 둘째 대안에서 `src/runtime/dispatch/provider_task_context.hpp`, `.cpp`의 **별도 Asio worker 선택**을 공통 자원 배치로 교체한다. Process dispatcher의 예약 수명·completion 문맥·close 의미는 남긴다. Offload와 Asio가 각각 같은 완료를 실행할 수 있는 병행 경로를 남기지 않는다. 다른 runtime 작업에 대한 기존 정책을 보존할 수 없는 경우 이 단계는 진행하지 않는다. 저장소 기준 `framework/languages/cpp/CMakeLists.txt:250`–`:265`는 provider_task_context를 provider-abstractions library로 빌드하고 `:398`은 Framework가 그 library에 의존하게 한다. 따라서 provider-abstractions에서 상위 Framework의 offload executor를 역참조하면 안 된다. 공통 배정 소유자를 기존 하위 library로 옮기고 CMake source·link 소유를 함께 바꿔 하나만 남기는 방향으로 검토한다. Standalone provider와 test의 link·수명 검증도 필요하다.
5. `src/runtime/foundation/operation_registry.cpp:269`의 기존 item을 새 배치 소유자에게 독립 게시한다. Terminal take와 item 저장소를 바꾸지 않는다. 다른 item을 전부 실행 가능하게 공개한 뒤, 이전 수신 자원이 해당 dispatcher의 새 turn 하나를 실행한다. local callback loop로 sibling 독립성을 없애지 않는다. 이때 `provider_task_context.cpp:188`과 같은 completion 소유 문맥을 진입해야 기존 `task.hpp`가 같은 turn을 재사용한다.
6. `mesh_node_runtime.cpp:4407`의 기존 수신자·codec projection과 `dispatch/request_task.hpp:480`을 그대로 이어 사용한다. 별도 raw→typed task·두 번째 관찰 연결을 추가하지 않는다. 마지막 caller queue 알림은 남는 비용으로 측정한다. Caller drain을 위해 benchmark나 sample에 private poller 접근을 넣지 않는다.

### 교체와 리팩토링 점검

| 제안 | 교체하는 기존 메커니즘 | 남기지 않을 병행 메커니즘 | 판정 위치 수 |
|---|---|---|---|
| Target 자원 배정 | host `submit_owner`에서 무조건 다른 worker drain을 게시하는 선택과 지속 role의 고정 점유 | host의 별도 idle 명단, 두 번째 ready record queue, record 전용 inline fast path | Target 자원 배정 **1→1**, 인계 지원 지점 **0→1**. 지원 지점 추가는 같은 소유자의 명세상 배정 결과이며 두 번째 권한자가 아니다. |
| Source dispatcher 참여 실험 | 기존 dispatcher queue를 전용 completion worker만 구동하던 실행 자원 범위 | 별도 local ready queue·item 사본·두 번째 terminal·별도 item 선택 정책 | 수신 배정 **1→1**, dispatcher item 선택 **1→1**. 동일 queue의 실행 참가자만 늘며 thread 수는 늘지 않는다. 기존 post·wake 비용은 남는다. |
| Source 공통 배치 | offload 자원 배정과 completion Asio의 독립 worker 선택 | 기존 두 scheduler 위의 broker, 동일 item의 Asio 게시와 local 실행을 따로 결정하는 flag | 영향 범위의 물리 배정 소유자 **2→1**. Terminal **1→1**, 완료 turn 허용 판단 **1→1**. |
| Role 연속 상태 이전 | run stack에 묶인 readiness·회전 연속 상태 | TLS의 두 번째 poller 소유권, 별도 generation으로 stale completion 필터링 | 수신 역할 소유자 **1→1**, poller 구동자 동시 최대 **1→1**. |
| Completion 실행 | dispatcher item의 다른 worker 선택만 변경 | 두 번째 pending 표·완료 flag·serial completion batch | 논리 terminal·typed 수신자·drain 예약 각각 **1→1**. |
| Reply 실행 | 기존 handler 완료 turn 재사용 | 새 reply queue·socket reactor 전달 | Reply 시작 위치 **1→1**, 추가 reply 전달 **0→0**. |

새 자원 배정 상태가 필요하면 기존 executor worker membership 상태를 그 자리에서 교체해야 한다. 새 플래그를 host에 덧붙여 executor 상태와 교차 확인하는 안은 승인 대상에서 제외한다. 역할 readiness를 저장하는 것은 새 사실을 만드는 일이 아니라 기존 stack 상태의 소유권을 옮기는 일이다. 어떤 state·queue·lock을 실제로 제거했는지는 구현 diff에서 다시 확인해야 한다. 이 읽기 전용 설계만으로 삭제 완료를 주장하지 않는다.

리팩토링 점검 결과: 성능 개선 후보 2건(target application 전달, source completion 전달), 설계에서 배제한 우회 변형 7건, 현재 코드의 불필요 코드로 확정한 항목 0건이다. 고친 항목은 0건이고 구현으로 넘긴 후보는 위 2건이다. Admission·terminal·gate를 지우는 변경은 제안하지 않았다. 수정 전/후 규칙 수는 target-only **배정 1→1**, 공통 pool 전체안 **배정 2→1**, dispatcher 참여안 **수신 배정 1→1·item 선택 1→1**, 나머지 의미별 판정자 각각 **1→1**이다. 실제 runtime 전체의 결함 전수 감사 결과라는 뜻은 아니다.

### 필요한 검증

기존 검사 위치는 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_receive_role_completion.cpp`, `test_cpp_framework_execution.cpp`, `test_cpp_framework_request_observer_lifetime.cpp`, `test_cpp_framework_idle_receive_cost.cpp`, `test_cpp_framework_messaging.cpp`, `test_cpp_framework_reply_submit_claim.cpp`다. 요청 site 수정 보고서의 `CsitesRequestCompletionKeepsStorageUntilEveryTerminalTurn`, `CsitesReplyDescriptorsUseRequestStorageUntilProjectionReturns`, `CsitesLocalTerminalAndInvocationSurviveAllRequestOutcomes`도 유지해야 한다. 기존 통과가 새 인계 구현을 검증한다는 뜻은 아니다.

- **배정 성공·실패 양쪽:** idle 자원을 실제 예약한 경우만 local turn이 실행되는지 확인한다. Idle이 없거나 close가 경합하면 기존 worker 경로로 한 번만 배달한다. waiter 수 또는 post 성공만으로 성공 처리하는 의도적 잘못된 배정을 검사가 거부해야 한다.
- **호출 stack·문맥:** 사용자 handler·completion 첫 instruction에서 producer drain·lane-current scope가 남지 않았는지 검사한다. 같은 OS thread에서도 application/completion ambient 문맥과 completion owner identity가 정상 경로와 같아야 한다. runtime blocking terminal은 InvalidOperation이고, handler의 허용된 async request는 정상 진행해야 한다.
- **진행 독립:** local handler와 local completion callback을 각각 붙잡아 둔 동안 후속 수신 자원의 completion·deadline·관리 작업이 먼저 진행하는지 확인한다. 한 완료 callback이 blocking하거나 예외를 던져도 unrelated completion이 완료돼야 한다. 수신 역할 자원이 application에 다시 배정되지 않아야 한다.
- **순서·권한:** Node·Channel은 FIFO take와 병렬 pipeline 진입을 검사한다. SpotWide·PerActor·Entry, 자기 Actor request 거부, 일반 await의 gate 유지, 허용된 Yield의 단일 재개 record·Actor claim 유지, lifecycle 우선순위·시간 예산을 검사한다. Thread 전달 감소를 위해 동시성을 줄이지 않는다.
- **Permit·backpressure:** ordinary receive 전 permit, handler 직전 반환, permit 0에서 completion·deadline 진행, admission pending, ready 결과·admitted·reply의 제출 간 격리를 검사한다. Send의 pending·ready 양쪽을 따로 검사한다.
- **Terminal·수명:** reply·timeout·owner cancellation·shutdown 경쟁, public 완료 뒤 인도된 physical 결과, header·typed decode 실패, callback throw, 인계 중 close를 검사한다. 결과 하나·해제 하나·callback 반환까지 저장소 유지가 필요하다. 닫힌 correlation을 Framework가 다시 읽지 않는다.
- **Shutdown:** seal 이후 새 작업 차단, 인계 예약·미게시 item·suspended handler까지 drain, closing callback 전에 runtime 소유 대기 정리, 최종 수신자 처리 뒤 scope·transport 해제를 확인한다. 성공·force-stop 모두 반환 뒤 callback이 남지 않게 한다.
- **자원 수·wake:** socket별 동시 wait/drain 최대 1, thread high-water 기존 이하, 회전당 인계 판단 최대 1, 인계 성공/실패/후속 receiver wake/application wake/명시 scheduler post를 따로 센다. 소스 수준의 post 감소만으로 OS 전환 감소를 판정하지 않는다.

네 언어 공통 contract 검사는 위의 관찰 가능한 결과를 같은 입력으로 비교한다. 물리 thread ID가 같음을 검사하지 않는다. Node에는 역할 인계 성공을 요구하지 않으며 .NET·Java의 기존 오류·cancellation 표면 차이는 명세가 정한 대로 투영한다. Binding·Core 교체가 없는 이 안에서도 package·binary 동일성을 기록해야 한다.

## 가장 작은 판별 실험

지금 있는 실행 파일만 재실행해서 구현하지 않은 인계의 효과를 증명할 수는 없다. 이번 작업에서는 새 측정 대신 기존 결과로 포화 세 셀의 caller 비용 제약을 확인했다. 다음 실험은 구현 승인 뒤의 최소 개입이며, 실험용 build도 이번 읽기 전용 범위 밖이다.

1. **Target-only 후보 하나**를 만든다. 같은 기존 source 실행 파일과 binding 대조군을 고정하고 target의 executor 배치만 바꾼다. Handler·wire·codec·payload·window·검증·Core와 binding은 고정한다. 새 thread·completion queue·retry는 없다. Test-only 제어를 쓴다면 바로 기존 배정 함수를 선택하게 하고 배정 권한을 두 벌로 만들지 않는다.
2. **request-serial 4KiB**를 첫 셀로 고른다. 필요한 순감소가 11.128µs라 가장 작은 지연 이득으로 0.6을 판별할 수 있다. 현재 전체 간격 약 130.570µs에서 **119.442µs 이하**가 목표다. Target-only 변경의 감소가 작으면 source 단계가 나머지를 모두 줄여야 한다. 그 잔여량과 실제 source edge의 지연을 비교한 뒤 둘째 대안을 결정한다.
3. **호출 지점 계측을 최소로 둔다.** Target의 회전 종료·배정 성공·handler 진입·reply 시작, source의 binding 완료 통지·새 completion turn 진입·caller 관찰을 같은 message 식별자로 잇는다. 각 endpoint 내부 구간과 end-to-end caller 구간을 분리하고 CPU·OS 전환·post·wake를 함께 본다. 모든 메시지에 문자열 log를 만들지 않고 기존 flow·비용 게이트 또는 낮은 비율의 고정 크기 계측을 쓴다. 역할 배정 실패율과 회전당 건수도 보존한다.
4. 같은 실행에서 header·submitted/completed/received·오류·abandoned·drain 오염을 검증하고, 결과 count 하나를 의도적으로 바꾼 사본을 validator가 거부하는지도 확인한다. 수신 callback을 잡아 둔 별도 correctness 검사는 성능 측정과 분리한다.
5. 초기 인과 판별은 전후 한 쌍으로 하고, 이득이 보일 때만 같은 조건의 3-run 중앙값으로 확인한다. Source까지 바꿀 때도 target-only cohort를 유지해 source 인계 비용을 분리한다. 최종 성능 주장은 여섯 셀과 두 구현의 동일 조건 3-run 비교로 한다. 마지막으로 **request-backpressure 1KiB**에서 caller CPU가 목표 **6.587µs/건**과 여전히 멀면, 전체 0.6을 위한 실행기 재작성은 중단하고 caller·transport 병목으로 돌아간다.

모든 향후 benchmark는 `flock -o /root/jobs/fx/cx/measure.lock ...` 안에서 `SKIP_BUILD=1`, source CPU 0–7, target CPU 8–15로 실행한다. 메모리는 최소 12GiB를 유지한다. 기존 진단 작업이 lock을 쓰는 동안에는 잡지 않는다. 후보 build가 필요하면 별도 승인된 구현 작업에서 끝내고, 측정 중 build하지 않는다.

통과 기준은 “post가 줄었다”가 아니다. **정확한 결과·기존 동시성과 진행·수명 보존**, **후속 수신 통지까지 포함한 순비용 감소**, **같은 조건의 처리량·직렬 지연 개선**이 함께 보여야 한다. Target-only 성공도 여섯 셀 0.6 달성 증거로 확대하지 않는다.

## 산출물 검증과 남은 불확실성

10줄 요약 형식, 여섯 셀의 비율·목표 간격·caller 상한 계산, 원본 36회 실행의 errors·abandoned·header failure·contamination·drain-bound flag, 핵심 소스 해시가 읽는 동안 바뀌지 않았음을 검사했다. 산술 검사는 정상 계산을 수락하고 비율 하나에 0.01을 더한 사본을 거부하는 양방향 검사로 확인했다. 이는 benchmark나 runtime 회귀 test가 아니다.

남은 불확실성은 #3957의 네 셀 전용 thread별 CPU·전환, 두 전달의 causal queue 지연, 인계 성공률·batch 분포, 후속 수신 wake의 순비용이다. 이 값을 측정하지 않은 채 특정 개선 비율을 보장하지 않는다. 코드·명세 변경과 runtime test 실행은 0건이며 새로 발생한 runtime test 실패도 없다.
