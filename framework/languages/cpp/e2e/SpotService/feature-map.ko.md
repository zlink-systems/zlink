# C++ SpotService E2E feature map

이 파일은 Config 2 SpotService 시나리오 중 C++ framework 공개 API로 검증한 항목과,
현재 공개 표면이 없어 E2E 앱에서 내부 패킷을 직접 만들지 않기로 한 항목을 구분한다.
외부 C++ client는 HTTP client와 stream connector만 사용한다. route client와
spot route 요청은 server HTTP endpoint 뒤에서 public framework API로 수행한다.

## 구현됨

- `SM-A1` (전환 필요): Entry Spot join과 Spot create로 user Spot 생성과 reply Spot ID를 검증한다.
  현재 source의 `location_runtime_query_t`는 정식 C++ interface에 없으므로 완료 근거가 아니다.
  public location store의 `resolve_spot(...)` 결과와 MeshNode runtime evidence로 owner와 generation을
  검증하도록 바꿔야 한다.
- `SM-A2`: 같은 user spot에 연속 상태 변경 request를 보내 누적 상태와 순서를 검증하고,
  `.NET`식 lifecycle context group에서는 앞선 SM-A4/F1/F2 state evidence가 보존되는지 확인한다.
- `SM-A3`: route client가 `target_node_rid`와 특정 `spot_id_t`를 함께 지정해 원격 user spot으로
  직접 request를 보내고, 해당 owner node와 spot id의 reply가 오는지 검증한다.
- `SM-A4`: 같은 key가 같은 owner node와 같은 spot id로 매핑되는지 검증하고, `.NET`식 lifecycle
  context group에서는 같은 context spot id가 play-a owner에 유지되는지 확인한다.
- `SM-A5`: `.NET`의 app-level `ScenarioStage` wrapper에 대응해 C++ user spot이 public spot
  request handler와 `spot_context_t::add_timer<THandler>`를 사용하고, stage request, stage timer,
  spot close lifecycle evidence를 검증한다.
- `SM-A6`: actor 없는 user spot을 생성한 뒤 public `close_spot`으로 닫아 initialize/closing
  lifecycle evidence를 검증한다.
- `SM-A7`: 같은 spot id를 다른 spot 타입으로 다시 `get_or_create_spot`할 때
  `spot_type_mismatch`로 거부되고 기존 user spot 상태가 유지되는지 검증한다.
- `SM-A8`: `/spot/worker/start`가 public `run_worker`로 무거운 작업을 spot 직렬 루프 밖에
  offload하고, 같은 spot의 다른 request가 worker 완료 전에 처리되며 `/spot/worker/complete`가
  worker 결과 evidence를 관측하는지 검증한다.
- `SM-B1`: `play-a` local actor join과 user spot dispatch를 검증한다.
- `SM-B2`: `play-b` remote actor join과 cross-node dispatch를 검증한다.
- `SM-B3`: join request의 문자열, 숫자, 배열 payload가 reply에 그대로 반영되는지 검증한다.
- `SM-B4`: `play-b`에 있는 actor로 후속 request를 보내 cross-node actor routing과 reply를
  검증한다.
- `SM-B5`: handler 없는 actor packet request가 client-visible error로 끝나고
  `no_handler` dispatch error marker가 play 노드 로그에 남는지 검증한다.
- `SM-B6` (재검증 필요): 명시적 leave는 actor leave reply와 evidence로 검증한다. Physical stream
  disconnect는 application이 Actor를 선택하지 않고 Framework가 current binding 전체에 자동 통지해야
  하므로 최신 runtime runner 증거가 필요하다.
- `SM-B7`: HTTP evidence snapshot에서 `ActorCreated`, entry spot packet handler, user spot join,
  join callback, 후속 actor packet handler의 순서를 검증한다.
- `SM-B8`: stream auth로 actor를 bind한 뒤 entry Spot의 public `destroyActor`로 actor를 명시 파괴하고,
  destroy evidence와 post-destroy request 실패를 검증한다.
- `SM-B9`: stream-bound entry actor가 public `actor_context_t::join_spot`으로 user spot admission을
  수행하고, 허용된 actor만 user spot에 commit되며 거부된 actor는 `ActorJoinRejected` reply와
  reject evidence로 끝나는지 검증한다.
- `SM-C1`: HTTP client가 Play role endpoint를 호출하고, server-owned route client가 특정 target
  node와 spot id로 request/send를 보내면 해당 spot이 처리하는지 검증한다. handler-missing/timeout
  이후 정상 request가 오염되지 않는지도 함께 확인한다.
- `SM-C2`: spot handler가 ChannelName으로 request/send를 내보내고, Logical Multicast를 수행하는
  흐름을 검증한다.
- `SM-C3`: 한 user spot이 다른 user spot으로 public `request_to`/`send_to`를 수행하고,
  Logical Multicast, missing handler request 실패, slow target timeout을 함께 검증한다.
- `SM-C4`: local Spot을 등록하지 않은 MeshNode가 Logical Multicast를 제출하고, play 노드의 구독
  Spot들이 이벤트를 받는지 검증한다.
- `SM-C5`: `play-a` spot handler가 cross-node user spot으로 request/send를 수행한 뒤 같은 spot
  handler에서 제출한 Logical Multicast 이벤트가 `play-b`의 target Spot subscriber evidence에 남는지
  검증한다. 두 Spot location row를 공개 조회로 먼저 관측하고, cross-node 의미 request는 재시도 없이
  한 번만 보내 수렴 직후 결과를 그대로 검증한다.
- `SM-D1`: 실제 `session-a` stream gateway에 연결해 `play-a` actor로 local stream relay를 보내고,
  actor가 bound session으로 보낸 push를 client 수신과 play/session evidence로 검증한다.
- `SM-D2`: route mesh를 등록하지 않은 `session-a` gateway와 분리된 `play-b` actor 사이에서 remote
  stream relay를 보내고, actor push가 MeshNode Actor owner route의 bound-session 경로를 거쳐 반환되는지 검증한다.
- `SM-D3`: entry spot actor와 user spot actor를 각각 실제 stream session에 bind하고,
  public stream connector request/push로 entry/user spot 경로의 relay와 bound-session push를
  검증한다.
- `SM-D4`: 한 stream session에 두 actor를 bind하고 `actor-id` metadata로 각각 다른 actor에
  relay/push가 전달되며, metadata 없는 request가 실패하는지 검증한다.
- `SM-D4A` (runtime contract 구현): `test_cpp_framework_m6b_runtime`이 같은 Actor를
  Session A에서 B로 bind한 뒤 stale binding admission과 late close가 current binding에
  영향을 주지 않는지 검증한다.
- `SM-D4B` (runtime contract 구현): 같은 runner가 bind 이후 inbound admission 동안 authority
  resolver read count가 증가하지 않는지 검증한다.
- `SM-D5` (runtime contract 구현): `test_cpp_framework_actor_gateway`가 exact binding snapshot의
  all-settled callback과 callback failure 이후 cleanup, Actor record 유지를 검증한다.
- `SM-D5A` (runtime contract 구현): 같은 runner가 physical connection을 유지하면서 선택
  Actor만 logical disconnect하고 다른 Actor binding을 유지하는지 검증한다.
- `SM-D6`: actor push가 bound stream session으로만 전달되고, 연결만 하고 bind하지 않은 consumer는
  같은 push를 받지 않는지 검증한다.
- `SM-D7`: stream auth 전 packet dispatch가 실패하고, 잘못된 auth request가 public error로
  끝나며, auth 성공 후 request dispatch가 정상 동작하는지 검증한다.
- `SM-D8`: stream 연결 종료 시 pending request가 실패하고 자동 재전송되지 않으며, 새 stream
  session에서 재auth/rebind한 뒤 actor messaging이 정상 재개되는지 검증한다.
- `SM-D9`: stream inbound observer가 auth/join/state response의 kind/name/request-seq를
  관측하는지 검증한다.
- `SM-D10`: `max_received_messages`로 stream push 수신 queue를 제한하고, 느린 push callback에서도
  같은 session request와 다른 session push가 계속 정상 동작하는지 검증한다.
- `SM-D11`: 같은 client process에서 stream actor request와 일반 channel request를 동시에 보내도
  각각 stream dispatcher와 channel dispatcher에서 reply를 받는지 검증한다.
- `SM-D12`: `session-a`에서 join/state/push를 수행한 actor가 연결을 끊은 뒤 `session-b`로
  다시 auth/rebind해 play 노드의 기존 state snapshot과 후속 push를 이어받는지 검증한다.
- `SM-D13`: `.NET`과 같이 heartbeat-enabled stream이 유지되는지 확인하고, 같은 stream에서
  후속 actor request가 성공하는지 검증한다.
- `SM-D14`: public stream node TLS server 설정으로 `tls://` endpoint를 열고, stream connector가
  self-signed certificate를 strict mode에서 거부한 뒤 skip-validation mode에서 bind, relay, push를
  평문 stream과 같은 의미로 수행하는지 검증한다.
- `SM-D15`: gateway role의 HTTP endpoint가 public `actor_client_t::request_to_actor`로 actor
  handler를 호출하고, actor가 bound stream session으로 push한 notify를 client가 수신하는지 검증한다.
- `SM-E1`: handler 없는 spot route request가 client-visible error로 끝나고, 이후 정상 spot route
  request가 같은 route channel에서 계속 성공하는지 검증한다.
- `SM-E2`: user spot이 public `spot_context_t::add_timer<THandler>`로 timer를 등록하고,
  timer tick handler가 같은 spot evidence에 tick marker를 남기는지 검증한다.
- `SM-E3`: public spot create lifecycle에서 idle timer를 등록하고, timer handler가 public
  `spot_context_t::close()`로 같은 spot을 닫으며 이후 닫힌 spot request가 실패하는지 검증한다.
- `SM-E4`: public `timer_options_t`의 overrun policy별로 지연된 timer handler를 실행하고,
  `timer_tick_t`의 delivery/scheduled/skipped evidence로 skip, bounded catch-up, delayed next tick
  동작을 검증한다.
- `SM-F1`: server-owned Spot client가 resolver에서 받은 local `SpotHandle`로 request/send를 제출하고
  target Spot만 처리하는지 검증한다.
- `SM-F2`: remote owner의 `SpotHandle`로 request/send를 제출하며 caller가 target RID와 endpoint를
  조립하지 않아도 MeshNode가 owner route와 generation을 보존하는지 검증한다.
- `SM-F3`: `Client/Scenarios/sm_f3_scenario.hpp`가 같은 MeshNode에서 ChannelName request, RID direct
  request와 Spot direct request를 섞어 제출하고 각각 올바른 dispatcher로 분기되는지 검증한다.
- `SM-F4`: 존재하지 않는 target Spot, handler 없는 request와 timeout이 정식 failure로 끝나고 후속
  정상 request가 복구되는지 검증한다. hostile raw frame은 Core contract test가 소유한다.
- `SM-F5`: `Client/Scenarios/sm_f5_scenario.hpp`가 target Spot 종료 뒤 해당 경로만 실패하고 같은
  MeshNode의 ChannelName request와 peer readiness가 유지되는지 검증한다.
- `SM-F6`: MultiNode role을 같은 MeshName의 MeshNode로 등록하고, 별도 channel·Spot socket 없이 서버 간 구동
  순서와 무관하게 client readiness 뒤 remote spot request/send와 actor join이 target spot evidence에
  남는지 검증한다.
- `SM-G1`: `session-a`/`session-b`를 각각 `play-a`/`play-b`에 bind하고, actor와 stream session이
  붙은 `play-a`를 실제 SIGKILL한다. 이후 `play-b` actor/session은 계속 동작하는지 확인하고
  계속 실행 중인 `play-b`에 재auth/rebind해 상태를 복구한다.
- `SM-G2`: 앱이 같은 logical key의 owner spot RoutingId를 `play-a`에서 `play-b`로 remap한
  흐름을 `/spot/create`와 target node가 명시된 spot route request로 표현하고, remap 전후
  request evidence가 새 owner에만 남는지 검증한다.
- `SM-G3`: 다수 stream client가 같은 user spot에 동시에 join/request/leave를 보내고, actor별
  `ActorJoined`/`ActorLeft` evidence가 정확히 1회씩 남는지 검증한다.
- `SM-G4`: 다수 stream client가 각각 다른 actor에 bind된 상태에서 동시에 push를 트리거하고,
  각 client가 자기 actor의 `ActorPushNotify`만 수신하는지 검증한다.
## 남은 시나리오

- `SM-C6`: remote peer backpressure에서 blocking publish의 ROUTER send timeout과 non-blocking
  submit의 즉시 backpressure 결과를 각각 검증해야 한다. 대상별 submit이므로 먼저 수용된 대상과
  막힌 대상의 수치가 publish detail에 함께 기록되는지도 확인해야 한다. 현재 runner에는 이 증거가
  없다.

## 남은 구현 후보

- 위 target별 ROUTER backpressure 시나리오를 실행할 제어 가능한 harness의 구현 증거를
  Framework 10.0.0 적용 단계에서 확정해야 한다.
# CA-D78 Session Actor binding runtime evidence

- `SA-BIND-01`, `SA-BIND-02`: `test_cpp_framework_m6b_runtime`과
  `test_cpp_framework_actor_gateway`가 한 connection의 multi-Actor binding, Actor당 단일
  current Session, stale binding token 차단을 검증한다.
- `SA-ROUTE-01`, `SA-ROUTE-02`: bind 이후 inbound admission은 저장한 exact binding만
  검증하며 authority resolver를 다시 호출하지 않는다.
- `SA-DISC-01`, `SA-DISC-03`, `SA-DISC-04`, `SA-DISC-05`: physical STREAM disconnect는
  current binding snapshot 전체를 처리하고 개별 callback failure 뒤에도 나머지 callback과
  cleanup을 계속한다.
- `SA-MOVE-06`: internal Actor route update는 같은 `ObjectGeneration`에서만 허용한다.
- `SA-LOGICAL-01`: `test_cpp_framework_actor_gateway`가 live Session의 선택 Actor logical
  disconnect와 다른 binding 유지를 검증한다.
  공통 E2E scenario의 실제 process 간 검증은 새 scenario porting 뒤 별도 PASS log로 갱신한다.
