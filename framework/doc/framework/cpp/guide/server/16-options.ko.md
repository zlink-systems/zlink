---
title: "16. Options — 설정 목록과 기본값 · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: E2E 테스트](15-e2e-testing.ko.md) | [다음: ZLink를 어디에 쓰나](17-alternative.ko.md)
<!-- framework-adapter-nav:end -->

# 16. Options — 설정 목록과 기본값

> **이 장의 계약 소유 문서** — [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)이
> 다룬다. 이 챕터는 그 표면을 목록으로 정리해 무엇을 정할 수 있고 정하지 않으면
> 어떻게 되는지를 보여준다. 설정 파일에서 값을 읽어 오는 방법은
> [19. Configuration](19-configuration.ko.md)이 다룬다.

이 챕터는 **무엇을 정할 수 있고 정하지 않으면 어떻게 되는지**를 모은다. 각 옵션이
무엇을 바꾸는지는 해당 기능 챕터가 설명하고, 여기서는 자리와 기본값을 본다.

## 1. 설정 적용 위치

같은 설정이라도 어디에 지정하느냐에 따라 적용 범위가 달라진다.

| 자리 | 적용 범위 | 변경 시점 |
| --- | --- | --- |
| 루트 `options` | process 전체의 기본값 | `app.run ()` 전에만 |
| builder | 그 node · channel · STREAM node 하나 | `app.run ()` 전에만 |
| runtime option | 이미 실행 중인 값 일부 | 실행 중(§7) |

```cpp
auto app = app_t::create ();
app.add_zlink_framework ([] (zlink_framework_options_t &options) {
    // ① 루트 — 이 process의 모든 payload에 적용된다.
    options.codecs ().use (protobuf_codec_t::default_instance ());
    options.set_default_request_timeout (std::chrono::seconds (30));

    // ② builder — 이 node 하나에만 적용된다.
    auto mesh = options.add_route_mesh ("play");
    mesh.listen (node.mesh_endpoint)
      .set_routing_id (zlink::routing_id_t::from (std::string ("play")))
      .set_spot_limit (2000);
    mesh.channel_name ("room").server ();
});
return app.run (argc, argv);
```

`app.run ()` 이후에 builder를 다시 호출하는 표면은 없다. 잘못된 조합은 첫 호출까지
미루지 않고 **시작 단계에서 예외로 막힌다.**

## 2. 루트 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `codecs ().use (...)` | payload 직렬화 형식 | 내장 JSON |
| `set_default_request_timeout (...)` | request reply 대기 상한 | 30초 |
| `set_message_follow_duration (...)` | 이동 중인 대상으로 온 message를 따라 보내는 기간 | 30초 |
| `handlers ()` | handler group 등록 | — |
| `metadata ()` | metadata 전달 정책 | — |
| `configure_dispatch ()` | 진단 수준과 message flow(§4) | `errors_only` |
| `configure_inbound_dispatch ()` | host 전체 수신 상한(§3.2) | 자동 계산 |
| `configure_locations ()` | location store 동작(§5) | §5 표 |
| `add_location_store (...)` | 위치 결정 store | 없으면 단일 node 구성 |
| `services ()` | DI 등록([18. DI 컨테이너](18-di-container.ko.md)) | — |

`set_default_request_timeout`은 **0 이하를 거부한다.** 값이 잘못되면 시작 단계에서
`framework_exception_t`가 난다.

## 3. MeshNode 옵션

`add_route_mesh (name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `listen (endpoint)` | 다른 node가 접속할 자기 주소 | 지정해야 한다 |
| `set_bind_host` · `set_advertise_host` | bind 주소와 광고 주소를 나눠 쓸 때 | bind 주소 그대로 |
| `set_routing_id (...)` | 이 node의 식별자 | 자동 생성 |
| `set_object_role (...)` | Object role — spot · actor 배치 여부 | 배치하지 않음 |
| `set_placement_weight (int)` | 새 object 배치 선택 가중치 | 100 |
| `set_actor_limit` · `set_spot_limit` | 이 node가 담을 상한 | 무제한 |
| `set_activation_concurrency (...)` | 동시에 진행할 cold activation 수 | runtime 기본값 |
| `set_default_request_timeout (...)` | 이 node 호출의 reply 상한 | 루트 값 |
| `peer_connections ()` | 수동 peer 연결 | location store 자동 발견 |
| `configure_router_socket ()` | 아래 §3.1 | 아래 표 |

### 3.1 소켓 상한

`configure_router_socket ()`이 돌려주는 `mesh_node_socket_config_t`의 값이다.

| 필드 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `max_message_size` | 받아들일 message 하나의 최대 크기 | 16 MiB |
| `send_high_water_mark` | 상대별로 보내려고 보관할 byte | 4,096,000 |
| `receive_high_water_mark` | 상대별로 받아서 보관할 byte | 4,096,000 |
| `mailbox_message_budget` | owner별 mailbox가 담을 message 수 | 1024 |
| `mailbox_byte_budget` | owner별 mailbox가 담을 payload byte | 64 MiB |
| `receive_timeout` · `send_timeout` | 지정하면 그 방향의 대기 상한 | 없음 |

두 high-water mark의 동작 원리와 값을 고르는 기준은
[4. Backpressure](04-backpressure.ko.md)가 다룬다.
`0`은 기본값이 아니라 **무제한**이다. 자동 계산에 맡기려면 값을 지정하지 않는다.

`mailbox_*` 두 값은 **시작 전에만** 설정한다. `0`은 무제한이 아니라 Framework profile이
정한 유한 기본값을 고른다.

### 3.2 host 전체 수신 상한

`configure_inbound_dispatch ()`가 돌려주는 표면이다. 연결마다 두는 상한과 성격이 다르다 —
아직 handler 실행을 시작하지 못한 message의 **payload 합계**에 적용한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `set_application_hwm_bytes (...)` | host 전체 상한 byte | 지정 안 하면 profile로 자동 계산 |
| `set_application_hwm_profile (...)` | 자동 계산에 쓸 성향 | `balanced` |
| `set_process_memory_limit_bytes (...)` | 자동 계산의 기준 memory | container·cgroup 상한 |

`set_process_memory_limit_bytes`에 `0`을 넣으면 시작 단계에서 거부된다. 무제한을
뜻하려면 값을 지정하지 않는다.

> 이 단위와 상한은 계약으로 확정되었을 뿐 **아직 runtime이 사용하지 않는다.**
> [4. Backpressure §6](04-backpressure.ko.md#6-framework-runtime-적용-범위)을 본다.

## 4. 진단

`configure_dispatch ()`가 돌려주는 표면이다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `message_flow (...)` | 기록 수준 | `errors_only` |
| `trace_sample_rate (double)` | 표본 비율 | 1.0 |
| `include_message_sizes (bool)` | payload byte를 함께 남길지 | 남기지 않음 |
| `trace_log_file (path)` | 앱 로그와 분리해 쓸 파일 | 분리하지 않음 |
| `set_message_flow_observer (...)` | 기록을 프로그램으로 받기 | 없음 |

수준별로 무엇이 남는지와 observer 사용법은 [11. Monitoring](11-monitoring.ko.md)이 다룬다.

## 5. Location 옵션

`configure_locations ()`가 돌려주는 `location_options_t`의 값이다.

| 필드 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `owner_lease_renew_interval` | owner lease 갱신 주기 | 5초 |
| `owner_lease_ttl` | lease 유효 기간 | 15초 |
| `owner_lease_renew_timeout` | 갱신 호출의 상한 | 3초 |
| `owner_lease_fencing_margin` | 이전 owner를 배제하는 여유 | 5초 |
| `polling_interval` | store 조회 주기 | 1초 |
| `store_failure_grace` | store 장애를 견디는 기간 | 30초 |
| `route_cache_max_age` | 경로 캐시 유효 기간 | 15초 |
| `message_follow_duration` | 이동 중 대상으로 온 message를 따라 보내는 기간 | 30초 |
| `max_active_outbound_relocations` | 동시에 내보낼 relocation 수 | 64 |
| `max_active_inbound_relocations` | 동시에 받아들일 relocation 수 | 64 |
| `max_concurrent_relocation_captures` | 동시에 capture할 수 | 8 |
| `max_concurrent_relocation_restores` | 동시에 restore할 수 | 8 |
| `max_relocation_payload_in_flight_bytes` | 이동 중 payload 총량 상한 | 256 MiB |
| `spot_router_channels` | Spot mesh 이름과 route channel 이름이 다를 때의 대응 | 같은 이름 사용 |

**`owner_lease_ttl`은 `owner_lease_renew_interval`보다 넉넉히 크게 둔다.** 갱신 한 번이
실패해도 lease가 살아 있어야 잠깐의 store 지연으로 owner가 바뀌지 않는다. 기본값은
5초 : 15초로 세 배다.

## 6. STREAM 옵션

`add_stream_node (name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `bind (endpoint)` | client가 접속할 주소 | 지정해야 한다 |
| `enable_actor_dispatch ()` | session이 Actor로 relay할 수 있게 한다 | 하지 않음 |
| `register_session<TSession> ()` | 연결마다 만들 session 타입 | 지정해야 한다 |
| `set_tls_server (cert, key, require_client_cert)` | TLS 구성 | 평문 |

> **`enable_actor_dispatch ()`는 STREAM node마다 한 번만 부른다.** 같은 node에 두 번
> 부르면 `request_protocol_error`로 던진다.

STREAM socket은 같은 profile에서도 MeshNode보다 작은 상한을 쓴다.
[9. STREAM](09-stream.ko.md)을 본다.

## 7. 실행 중 바꿀 수 있는 것

시작 뒤에 바꿀 수 있는 값은 **가중치 둘뿐**이다. 나머지는 시작 전에 정한다.

| 값 | 표면 | 무엇에 쓰나 |
| --- | --- | --- |
| 배치 가중치 | `route_mesh_runtime_options_t::placement_weight (int)` | 새 object 배치 대상에서 빼거나 되돌린다 |
| channel 가중치 | `...channel (name).weight (int)` | 새 select-one 대상에서 빼거나 되돌린다 |

둘 다 `0`으로 두면 **새 배정만 멈춘다.** 이미 있는 object와 연결은 그대로 살아 있다.
무중단 배포에서 이 node로 새 트래픽이 가지 않게 한 뒤 relocation을 시작하는 순서로
쓴다([12. 운영](12-operations.ko.md) §4).

## 8. 반드시 정해야 하는 것

기본값이 없어 지정하지 않으면 시작이 실패하는 것들이다.

| 값 | 어디에 |
| --- | --- |
| MeshNode의 `listen` 주소 | `add_route_mesh (...).listen (...)` |
| STREAM node의 `bind` 주소와 session 타입 | `add_stream_node (...)` |
| fanout publisher의 endpoint | `add_fanout_channel (...).enable_publisher (...)` |
| Spot·Actor를 배치할 node의 Object role | `set_object_role (object_role_t::server)` |
| 여러 node를 쓸 때의 location store | `add_location_store (...)` |

## 9. 자주 발생하는 문제

- **`0`으로 두었더니 memory가 계속 는다** → high-water mark의 `0`은 기본값이 아니라
  무제한이다. 자동 계산에 맡기려면 값을 지정하지 않는다.
- **timeout을 0으로 넣었더니 시작이 실패한다** → 정상이다.
  `set_default_request_timeout`은 0 이하를 거부한다.
- **`process_memory_limit_bytes`에 0을 넣었더니 거부된다** → 무제한을 뜻하려면 값을
  지정하지 않는다. `0`은 잘못된 값이다.
- **lease가 자꾸 뺏긴다** → `owner_lease_ttl`이 `owner_lease_renew_interval`에 비해
  너무 짧다. 갱신 실패 한 번을 견딜 여유를 둔다.
- **가중치를 0으로 했는데 기존 연결이 끊긴다고 생각했다** → 가중치는 **새 배정만**
  막는다. 기존 object와 연결은 유지된다.
- **실행 중에 소켓 상한을 바꾸려 했다** → 소켓 설정은 시작 전에만 정한다. 실행 중
  바꿀 수 있는 값은 §7의 둘뿐이다.

## 10. 관련 문서

- 정식 계약: [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)
- 설정 파일에서 값 읽기: [19. Configuration](19-configuration.ko.md)
- 상한이 무엇을 바꾸는지: [4. Backpressure](04-backpressure.ko.md)
- 가중치로 트래픽을 빼는 절차: [12. 운영](12-operations.ko.md)
