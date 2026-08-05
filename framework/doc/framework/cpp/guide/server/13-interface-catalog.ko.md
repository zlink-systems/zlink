---
title: "13. 주요 타입 사용 색인 · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

# 13. 주요 타입 사용 색인

> **이 장의 계약 소유 문서** —
> [C++ exact interface 목차](../../../common/spec/server/languages/cpp/interfaces/README.ko.md)가
> 정확한 signature를 소유한다. 이 챕터는 application에서 자주 쓰는 public 타입을
> 기능별로 찾는 안내서다.

C++ framework의 타입 이름은 `_t` 접미사를 쓴다. **application이 직접 만드는 타입**과
**DI로 주입받는 타입**을 구분해 읽으면 빠르다 — 전자는 상속하거나 선언하고, 후자는
`dependency_types`에 적어 생성자로 받는다.

## 1. Channel messaging

호출하는 쪽은 DI로 client를 주입받는다.

```cpp
class place_order_handler_t
{
  public:
    using dependency_types = dependency_list_t<route_client_t>;

    task_t<order_placed_t> handle (const place_order_t &request)
    {
        co_return co_await _client
          .request_to_channel ("orders", request)
          .timeout (std::chrono::seconds (3))
          .submit<order_placed_t> ();
    }

  private:
    route_client_t &_client;
};
```

| 타입 | Application에서 하는 일 |
| --- | --- |
| `route_client_t` | ChannelName 또는 관리 대상 Node RID로 send · request |
| `publisher_t` · `spot_publisher_client_t` | classic fanout channel에 event publish |
| `route_send_call_t` | one-way 제출 |
| `channel_request_call_t` | timeout 지정과 typed reply 수신 |
| `message_context_t` · `route_message_context_t` | 이 dispatch의 metadata와 출처 |
| `handler_filter_context_t` | filter가 보는 dispatch 정보 |

**handler는 class로 만들고 계약을 멤버로 선언한다.** `request_type` · `reply_type` ·
`topic_name`이 그 계약이다. 반환형이 `task_t<TReply>`면 request, `task_t<void>`면 send다.

Node direct(`request_to_node`)는 특정 MeshNode 자체를 관리할 때만 쓴다. 업무 object는
ActorId · SpotId · ChannelName으로 부른다.

## 2. Topology 등록

시작 단계에서만 쓰는 builder들이다. `app.run ()` 이후에는 없다.

| 타입 | 무엇을 등록하나 |
| --- | --- |
| `zlink_framework_options_t` | 루트 — codec · handler group · location store · dispatch |
| `mesh_node_builder_t` | MeshNode 하나(`add_route_mesh`) |
| `mesh_channel_builder_t` | 그 node의 channel 역할(`channel_name`) |
| `mesh_channel_server_builder_t` · `mesh_channel_client_builder_t` | handler 등록 · 호출 전용 선언 |
| `fanout_channel_builder_t` | classic fanout channel(`add_fanout_channel`) |
| `client_server_channel_builder_t` | client · server 짝 channel(`add_client_server_channel`) |
| `stream_node_options_builder_t` | STREAM node(`add_stream_node`) |
| `mesh_peer_connections_t` · `endpoint_connections_t` | 수동 peer 연결 |
| `mesh_node_socket_config_t` | 소켓 상한([16. Options](16-options.ko.md) §3.1) |

`mesh_channel_builder_t`는 `client ()` 또는 `server ()`를 **정확히 한 번** 부른다.

## 3. Spot

application이 상속해 만드는 타입과 framework가 주는 context가 나뉜다.

| 타입 | 성격 |
| --- | --- |
| `spot_t<TActor>` | User Spot — 상속해서 만든다 |
| `entry_spot_t<TActor>` | Entry Spot — 상속해서 만든다 |
| `instance_spot_t` | Instance Spot — 상속해서 만든다 |
| `spot_context_t` · `entry_spot_context_t` · `instance_spot_context_t` | 생성자로 받는다 |
| `spot_common_context_t` | 위 셋의 공통 부분 |
| `spot_manager_t` | DI로 받아 Spot을 만들고 찾는다 |
| `spot_ref_t` | SpotId와 generation을 담은 참조 |
| `spot_create_call_t` · `spot_create_result_t` | 생성 호출과 결과 |
| `spot_create_response_t` | 생성 callback의 accept · reject |
| `spot_actor_join_result_t` | join admission의 accept · reject |
| `spot_closing_context_t` | 닫히는 중에 주어지는 deadline 정보 |
| `spot_handler_registry_t` · `instance_spot_handler_registry_t` | `configure ()`에서 handler를 등록 |
| `spot_relocation_adapter_t<TSpot>` | 상태를 담고 푸는 adapter |
| `spot_relocation_ready_call_t` · `spot_relocation_ready_completion_t` | 이전 가능 시점 신호와 결과 |
| `user_spot_factory_builder_t` · `instance_spot_factory_builder_t` | 등록 시 정책 지정 |

**Spot handler는 Spot의 member 함수다.** `configure ()`에서
`add_handler<&TSpot::method> ()` 형태로 등록한다. 예외가 하나 있다 — **timer만 별도
handler 타입**을 `add_timer<THandler> ()`로 등록하고, 그 타입의 `handle`이 대상 Spot과
tick 둘을 받는다([6. Spot](06-spot.ko.md) §6.1).

| timer 관련 타입 | 하는 일 |
| --- | --- |
| `timer_t` | 등록이 돌려주는 핸들. `cancel ()`에 쓴다 |
| `timer_options_t` | overrun 정책과 catch-up 상한 |
| `timer_tick_t` | tick마다 오는 지연 · 건너뛴 수 등 |
| `timer_failure_event_t` | tick handler가 실패했을 때 |

## 4. Actor

| 타입 | 성격 |
| --- | --- |
| `actor_t` | 상속해서 만든다 |
| `actor_context_t` | 생성자로 받는다. join · bound session 접근 |
| `actor_manager_t` | DI로 받아 Actor를 만들고 찾는다 |
| `actor_client_t` | ActorId로 send · request |
| `actor_ref_t` · `actor_id_t` | 참조와 식별자 |
| `actor_factory_t` · `actor_factory_builder_t` | 생성 방법과 등록 정책 |
| `actor_create_call_t` | 생성 호출 |
| `actor_create_created_t` · `actor_create_existing_t` · `actor_create_rejected_t` | 생성 결과 세 갈래 |
| `actor_create_response_t` | Entry Spot의 admission 응답 |
| `actor_join_call_t` | join 예약 |
| `actor_join_accepted_t` · `actor_join_rejected_t` · `actor_join_failed_t` | join 완료 세 갈래 |
| `actor_relocation_adapter_t<TActor>` | 상태를 담고 푸는 adapter |
| `session_actor_t` · `session_actor_manager_t` | session에 bind된 Actor |

생성 결과와 join 완료가 각각 **세 갈래 타입**이다. `std::get_if<...>`나 `std::visit`으로
가른다.

## 5. STREAM session

| 타입 | 성격 |
| --- | --- |
| `packet_stream_session_t` | 상속해서 만든다. `on_packet` · `on_connected` 등을 override |
| `stream_t` | 그 연결. reply · send · close |
| `session_message_context_t` | 이 packet의 dispatch 정보 |
| `stream_error_t` | 오류 통지 |
| `stream_send_call_t` · `stream_write_call_t` | 보내기 |
| `bound_session_t` · `bound_session_send_call_t` | Actor에 묶인 session으로 push |
| `bind_actor_call_t` | session과 Actor를 잇는다 |
| `stream_compression_options_builder_t` · `stream_compression_codec_t` | 압축 구성 |
| `stream_snapshot_t` | 상태 조회 |

**C++ session은 handler registry가 아니라 `on_packet` 하나에서 분기한다.** 다른 네
언어와 모양이 다른 자리다([9. STREAM](09-stream.ko.md)).

## 6. Location과 relocation

| 타입 | 성격 |
| --- | --- |
| `location_store_t` · `relocation_store_t` | 직접 구현하거나 제공 구현을 쓴다 |
| `redis_location_store_t` · `redis_location_options_t` | Redis 구현과 설정 |
| `redis_relocation_store_t` · `redis_relocation_options_t` | 〃 |
| `location_options_t` | 동작 값([16. Options](16-options.ko.md) §5) |
| `location_readiness_t` | 필요한 peer가 Ready인지 |
| `location_runtime_query_t` | 상태와 topology 조회 |
| `location_runtime_status_t` · `location_topology_entry_t` | 조회 결과 |
| `location_page_t` · `location_page_request_t` | 페이지 조회 |

store를 직접 구현할 일은 드물다. `store_*` · `blob_*` 계열은 그때만 본다.

## 7. Host와 관측

| 타입 | 성격 |
| --- | --- |
| `app_t` | 진입점. `create ()` · `run ()` |
| `framework_runtime_t` | host 상태와 relocate · shutdown |
| `route_mesh_runtime_t` | MeshNode 상태 snapshot과 observation |
| `route_mesh_runtime_options_t` | 실행 중 가중치 조정 |
| `client_server_runtime_t` · `fanout_runtime_t` | 해당 channel의 상태 |
| `message_flow_observer_t` | 메시지 흐름 기록 수신 |
| `framework_exception_t` | 실패. `kind ()` · `is_retriable ()` |
| `logger_t<TOwner>` | DI로 받는 로거 |

관측 표면의 사용법은 [11. Monitoring](11-monitoring.ko.md)이 다룬다.

## 8. 어디서 오는가

| 얻는 방법 | 해당 타입 |
| --- | --- |
| 상속한다 | `spot_t` · `entry_spot_t` · `instance_spot_t` · `actor_t` · `packet_stream_session_t` |
| 생성자로 받는다(context) | `spot_context_t` 계열 · `actor_context_t` · `message_context_t` |
| `dependency_types`로 주입받는다 | `route_client_t` · `publisher_t` · `actor_client_t` · `channel_client_t` · `spot_manager_t` · `actor_manager_t` · `logger_t<T>` |
| 시작 단계 builder가 돌려준다 | `mesh_node_builder_t` 계열 · `stream_node_options_builder_t` |
| 호출이 돌려준다 | `*_call_t` · `*_result_t` · `*_ref_t` |

DI 주입 규칙은 [18. DI 컨테이너](18-di-container.ko.md)가 다룬다.

## 9. 관련 문서

- 정확한 signature: [C++ exact interface 목차](../../../common/spec/server/languages/cpp/interfaces/README.ko.md)
- 실행 모델과 `task_t` · `result_t`: [21. 실행·구성 모델](21-execution-model.ko.md)
- 옵션과 기본값: [16. Options](16-options.ko.md)
- 관측 표면: [11. Monitoring](11-monitoring.ko.md)
