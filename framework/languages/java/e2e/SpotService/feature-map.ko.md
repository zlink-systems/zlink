# Java SpotService E2E feature map

이 문서는 Config 2 SpotService 공통 시나리오 중 Java framework E2E가 현재 검증하는 항목과,
public API 또는 harness 제어가 더 필요한 항목을 구분한다. 실행 코드는 public Spring starter,
`ZLinkSpotManager`, `ZLinkSpotOutbound`, MeshNode·ChannelName builder,
MeshNode builder, stream connector, `ZLinkSpotPublisherClient`만 사용한다.
Client는 HTTP driver이고, framework spot/route/stream 참여는 `Server/Gateway`, `Server/Play`,
`Server/Publisher` 같은 server role에서 수행한다.

마지막 검증:

- 명령: `nice -n 10 timeout 1200s ./run_e2e.sh all`
- 결과: passed
- 로그: `framework/languages/java/e2e/SpotService/logs/20260707-223144-3695956/`
- focused 명령: 자체 Redis endpoint로 `timeout 360s ./run_e2e.sh SM-D12`
- focused 결과: passed
- focused 로그: `framework/languages/java/e2e/SpotService/logs/20260707-214123-3491316/`
- focused 명령: `nice -n 10 timeout 420s ./run_e2e.sh SM-C5`
- focused 결과: passed
- focused 로그: `framework/languages/java/e2e/SpotService/logs/20260707-233141-3961827/`
- focused 명령: `nice -n 10 timeout 420s ./run_e2e.sh SM-B9`
- focused 결과: passed
- focused 로그: `framework/languages/java/e2e/SpotService/logs/20260707-234158-4020740/`
- focused 명령: `nice -n 10 timeout 420s ./run_e2e.sh SM-D15`
- focused 결과: passed
- focused 로그: `framework/languages/java/e2e/SpotService/logs/20260707-234915-4047473/`
- focused 명령: `nice -n 10 timeout 420s ./run_e2e.sh SM-F6`
- focused 결과: passed
- focused 로그: `framework/languages/java/e2e/SpotService/logs/20260707-235923-4093006/`

공통 E2E 문서와 다른 언어의 구현에 존재하는 public 기능이 Java에 없으면 단순 미구현으로 완료
처리하지 않는다. 다만 다른 언어 구현만으로 Java public contract를 새로 추가하지 않는다. spec 또는
공통 framework spec/guide에 근거가 있는 항목은 parity gap으로 관리하고, 근거가 부족한 항목은
draft/spec 검토 대상으로 분리한다. 공통 E2E 문서는 누락을 찾는 기준이지만, 새 public API를 추가할
근거로만 쓰지 않는다.

## 구현됨

- `SM-A1`: public `ZLinkSpotManager.getOrCreate`로 user spot을 생성하고 evidence로 확인한다.
- `SM-A2`: public `ZLinkSpotOutbound.requestToSpot`으로 user spot state mutation을 검증한다.
- `SM-A3`: `room-a`와 `room-b`가 각각 `play-a`와 `play-b`에서만 처리되는지 확인한다.
- `SM-A4`: 같은 key가 같은 `RoutingId`와 같은 owner 노드로 반복 라우팅되는지 확인한다.
- `SM-A5`: app-level `ScenarioStage` wrapper가 public spot request handler에서 호출되고, wrapper가
  spot state mutation과 public `context.addTimer` 등록을 수행하는지 확인한다. Client는 play role의
  HTTP control endpoint를 통해 public RouteMesh request로 stage request와 timer start를 보내고,
  `StageRequest`와 `StageTimer` evidence로 실행을 검증한다.
- `SM-A6`: user spot 생성 시 initialize evidence가 남고, public `ZLinkSpotManager.close`로 명시
  close했을 때 closing evidence가 남는지 확인한다.
- `SM-A7`: 이미 만든 spot rid를 다른 spot 타입으로 `getOrCreate`할 때 public configuration error로
  거부되고, 기존 spot이 같은 타입으로 계속 조회되는지 확인한다.
- `SM-A8`: public `context.runIoWorker(...).yield()`로 긴 I/O 작업을 spot 직렬 루프 밖에서 실행하는
  동안 같은 spot의 후속 request가 막히지 않고, yield 완료 뒤 spot state/evidence를 안전하게
  갱신하는지 확인한다.
- `SM-B1`: stream session에서 만든 local actor가 entry spot request를 처리하고, public
  `ZLinkActorContext.joinSpot`으로 user spot에 join한 뒤 user spot actor request를 처리하는지 확인한다.
- `SM-B2`: stream session이 `play-b`에서 actor를 만들고, 그 actor가 `play-a`의 user spot에 join해
  node 경계를 넘는 actor join과 mailbox 처리가 되는지 확인한다.
- `SM-B3`: actor create/join/request payload의 profile, level, tag 값이 handler까지 유지되고 reply에
  같은 값이 반영되는지 확인한다.
- `SM-B4`: remote actor가 user spot에 join한 뒤 actor request/reply가 MeshNode routed path를 거쳐 원래
  session client로 돌아오는지 확인한다.
- `SM-B5`: 존재하지 않는 actor handler packet request가 분류된 실패로 끝나고, dispatch error
  evidence에 handler missing marker가 남는지 확인한다.
- `SM-B6`: public `ZLinkSpotContext.leaveActor(actor)`로 명시 leave를 실행했을 때 user spot leave
  callback만 남고, stream disconnect 때는 actor disconnect callback만 남는지 확인한다.
- `SM-B7`: actor create, entry request, join, user request callback/handler가 evidence marker를 남겨
  lifecycle과 packet handler 실행 순서를 확인할 수 있는지 검증한다.
- `SM-B8`: public `ZLinkEntrySpotContext.destroyActor(actor)` 경로로 entry actor를 명시적으로
  destroy하고, 이후 같은 actor packet이 실패하며 destroy evidence가 1회만 남는지 확인한다.
- `SM-B9`: entry spot actor request handler가 public `actor.context().joinSpot(...)` 결과를 admission
  응답으로 분류하고, user spot `onActorJoin`이 허용과 거부를 나누는지 확인한다. local `play-a`와
  remote `play-b` 대상 모두에서 허용 actor는 `ActorUserJoined` evidence가 남고, 거부 actor는
  `ActorJoinRejected` 응답을 받으며 user spot join evidence가 남지 않는다.
- `SM-C1`: 외부 consumer의 public `ZLinkSpotOutbound`로 request, send, timeout, 미등록 packet
  negative path를 검증한다.
- `SM-C2`: spot handler 안에서 public `ZLinkSpotOutbound.requestToChannel` /
  `sendToChannel`로 ChannelName에 request/send를 내보내고, 같은 handler에서 Logical Multicast를
  수행해 구독 spot의 evidence를 확인한다.
- `SM-C3`: public `ZLinkSpotOutbound.requestToSpot` / `sendToSpot`으로 user spot 사이
  request/send가 노드 경계를 넘어 처리되는지 확인하고, Logical Multicast evidence를 함께 확인한다.
- `SM-C4`: local Spot factory가 없는 MeshNode 역할이 public publisher client로 Logical Multicast를
  제출하고, 구독 Spot들이 event evidence를 남기는지 확인한다.
- `SM-C5`: `play-a`의 `room-a` Spot handler가 public `outbound().publish(...)`로 Logical Multicast를
  제출하고, `play-b`의 `room-b` 구독 Spot에 event evidence가 남는지 확인한다.
- `SM-D1`: public stream connector와 framework `ZLinkSessionActors.bind` / `ZLinkSessionActor.relay` /
  actor `boundSession().send`로 local stream session auth, actor relay, actor push를 검증한다.
- `SM-D2`: `play-b`에서 만든 actor가 `play-a` user spot에 join한 뒤 user spot actor request와
  bound session push가 원래 remote stream connector로 돌아오는지 확인한다.
- `SM-D3`: 같은 stream session actor가 entry spot에 bound된 상태와 user spot에 joined된 상태에서
  각각 request/reply와 push를 처리하며 actor id와 spot rid가 유지되는지 비교한다.
- `SM-D4`: 한 stream session에 actor 두 개를 bind한 뒤 `actor-id` metadata로 request와 push target이
  분리되고, metadata가 없는 actor request는 실패하는지 확인한다.
- `SM-D4A` (runtime contract 구현): `ZLinkSessionActorBindingContractTest`가 같은 Actor의
  Session A→B replacement 뒤 stale binding의 late disconnect를 차단한다. 새 incarnation은
  explicit bind를 거쳐야 하며 이전 generation binding은 새 binding을 해제하지 못한다.
- `SM-D4B` (runtime contract 구현): 같은 test가 bind 횟수를 고정한 뒤 relay에서 hidden bind를
  수행하지 않는지 확인한다. stale stored route는 한 번만 제출되고 typed
  `REQUEST_TARGET_NOT_FOUND`로 끝난다.
- `SM-D5`: physical stream disconnect 때 Framework가 고정한 모든 bound Actor에 disconnect를
  자동 통지하는지 확인한다. application은 Actor 목록을 순회하지 않으며, 명시적
  `notifyDisconnected`는 별도 logical notification에만 사용한다.
- `SM-D5A` (runtime contract 구현): 같은 test가 physical connection을 닫지 않은 상태에서
  선택 Actor만 logical disconnect하고 다른 Actor binding을 유지하는지 확인한다.

`SM-D4A`·`SM-D4B`·`SM-D5`·`SM-D5A`의 process 간 transport orchestration은 SpotService
E2E runner에 아직 추가되지 않았다. 현재 증거는 JVM runtime의 focused contract test다.
- `SM-D6`: bound session과 shadow session을 각각 만들고, actor push가 request를 보낸 bound
  session에만 도착하며 shadow session에는 전달되지 않는지 확인한다.
- `SM-D7`: stream session auth 전 actor packet dispatch가 실패하고, auth 뒤 단일 bound actor
  request와 push가 metadata 없이 정상 처리되는지 확인한다.
- `SM-D8`: 끊긴 stream의 pending session request가 실패하고, 같은 actor id로 재접속해 auth한 뒤
  actor request가 다시 성공하는지 확인한다.
- `SM-D9`: `ScenarioSession.onDispatch`가 stream inbound packet name을 `StreamInbound` evidence로 남기고,
  client가 actor-session 경로에서 그 evidence를 확인한다.
- `SM-D10`: **blocked — §12.1.** public stream connector의 bounded received-message queue는 기존
  message를 유지하고 새 message를 버리며 `RECEIVED_MESSAGE_DROPPED`를 보고해야 한다. 현재 Java
  connector는 오래된 push를 제거하므로 정식 admission 의미를 검증하지 못한다. 수정 뒤 같은 흐름의
  후속 request와 다른 session push가 계속 정상 동작하는지도 함께 확인한다.
- `SM-D11`: 같은 client driver process에서 public stream connector actor request와 route-channel
  request를 함께 실행하고, stream inbound evidence와 route request evidence를 함께 확인한다.
- `SM-D12`: `session-a` stream에서 actor 상태를 만든 뒤 `session-b` stream으로 같은 actor id를
  재auth/rebind하고, `SnapshotReq`와 `ActorPushReq`로 actor state 보존과 push target이 새 stream으로
  옮겨졌는지 확인한다. Java native session relay는 mesh peer를 양방향으로 연결하고, relay submit은
  native binding retry 경로에서 route가 준비될 때까지 대기한다.
- `SM-D13`: heartbeat가 켜진 public stream connector가 heartbeat interval을 넘겨 유지된 뒤
  후속 actor request를 처리하고 stream inbound evidence를 남기는지 확인한다.
- `SM-D14`: public `ZLinkStreamNodeBuilder.setTlsServer(...)`로 self-signed TLS stream endpoint를
  열고, strict certificate validation 실패와 skip-validation 성공 경로의 actor auth/request/push를
  확인한다.
- `SM-D15`: gateway role에 주입된 public `ZLinkActorClient.requestToActor`가 entry actor handler에
  도달하고, handler가 actor의 bound stream session으로 보낸 push notify를 client connector가 실제로
  받는지 확인한다.
- `SM-E1`: handler 없는 spot route request/send가 error/drop 경로를 타고 dispatch observer evidence를
  남기는지 확인한다.
- `SM-E2`: user spot이 public `context.addTimer`로 등록한 timer를 주기적으로 실행하고 tick evidence를
  남기는지 확인한다.
- `SM-E3`: public `context.addTimer`로 만든 idle timer가 idle spot을 public `context.close`로 닫고,
  계속 열려 있어야 하는 spot은 닫지 않는지 evidence로 확인한다.
- `SM-E4`: public `ZLinkTimerOptions`와 `ZLinkTimerOverrunPolicy`로 skip/catch-up/delay overrun
  policy를 설정하고, `ZLinkTimerTick`의 delivery/skipped evidence가 남는지 확인한다.
- `SM-F1`: 외부 consumer가 RouteMesh 경로로 target spot에 도달하는지 확인한다.
- `SM-F2`: RouteMesh 채널명이 target spot egress의 실제 channel 기준으로 동작하는지 확인한다.
- `SM-F3`: 같은 RouteMesh에서 일반 route-channel request/reply와 target spot request/send가 한
  channel 위에서 함께 구성되고, 일반 packet은 channel handler가 처리하는지 확인한다.
- `SM-F4`: 존재하지 않는 target spot route request가 framework error로 실패하고, 같은 channel의
  정상 spot routing이 계속 동작하는지 확인한다. malformed relay packet 주입은 public route client
  표면으로 만들 수 없으므로 이 public E2E에서 직접 다루지 않는다.
- `SM-F5`: 같은 MeshNode로 ChannelName request와 target Spot direct request를 보낸 뒤
  public Spot close 경로로 target Spot을 닫고, 같은 ChannelName의 일반 request가 계속 성공하는지
  확인한다. Java play role은 ChannelName handler와 Spot owner가 한 MeshNode에 함께 있으므로 프로세스
  kill 대신 `ZLinkSpotManager.close`를 거치는 public 관리 표면으로 spot routing 사용/중단을
  검증한다.
- `SM-F6`: gateway와 multi-node 역할 두 개를 같은 MeshName의 MeshNode로 시작하며 별도 channel·Spot socket을 만들지 않는다.
  runner는 세 역할의 `route_mesh=disabled` marker를 client 실행 전에 확인한다. source spot의
  public `SpotHandleResolver` / `ZLinkSpotOutbound` 경로가 remote target spot request/send에 도달하는지
  확인한다. 같은 구성에서 public `ZLinkActorManager` / `ZLinkActorClient`로 만든 actor가 remote
  target spot에 join하고 target node evidence가 남는지도 확인한다.
- `SM-G1`: runner가 자신이 시작한 `play-a` 프로세스만 SIGKILL한 뒤 owner lease TTL 만료를 기다려
  같은 endpoint로 재시작한다. crash 중 기존 actor request가 bounded failure로 끝나고 `play-b`
  spot request는 계속 성공하며, 재시작 뒤 같은 actor id로 재auth, rejoin, rebind한 actor request가
  정상 처리되는지 확인한다.
- `SM-G2` (부분 구현): 현재 구현은 처음부터 실행 중인 play-a와 play-b에 서로 다른 Spot을 직접
  만들 뿐이다. MeshNode scale-out 뒤 기존 owner 유지, peer/capability와 Entry Spot handle readiness,
  선택한 play-b Entry Spot의 application `JoinReq`로 새 actor 생성, play-b 로컬 manager의 새 Spot
  생성을 순서대로 검증하지 않는다.
- `SM-G3`: public stream connector actor들이 같은 user spot에 join한 뒤 동시 actor request와
  leave를 수행해 `ActorUserJoined` / `ActorUserLeft` evidence가 actor별 1회만 남고, 늦게 도착한
  entry spot lifecycle event가 user spot dispatch를 되돌리지 않는지 확인한다.
- `SM-G4`: 여러 public stream connector session을 각각 actor에 bind한 뒤 동시에 actor push를
  트리거하고, 각 reply와 push가 자기 actor/session으로만 돌아오는지 확인한다.
`.NET Client/Scenarios/*.cs`에 대응하는 Java Client scenario 파일은 존재한다. Java client는
`Server/Gateway`의 HTTP endpoint를 호출하고, runner가 필요한 서버 role과 evidence 검증을 함께
수행한다.

## public contract parity 또는 spec 검토 대기

`SM-G2` 진단에서 public location query가 `play-b`의 `actor:scenario` capability와 Entry Spot row를
반환하고 해당 rid의 `SpotHandle` resolve도 성공했지만, 그 handle로 보낸 application `JoinReq`는
Java runtime의 `NOT_FOUND`로 끝났다(`logs/20260716-092129-3281815/`). Entry Spot을 별도 HTTP actor
생성 endpoint로 우회하지 않고 일반 Spot handle request와 같은 공개 경로에서 dispatch할 수 있도록
runtime을 보완해야 한다. 다른 언어 구현만 근거인 기능은 Java public API로 바로 추가하지 않고,
별도 draft/spec 검토 대상으로 분리한다.

## Java public contract 기반 E2E 미구현

`SM-G2`는 runner가 play-a의 기존 owner를 먼저 만든 뒤 play-b를 추가로 시작하고, Client가 public
readiness를 확인한 다음 신규 actor와 Spot을 play-b에 배치하도록 orchestration을 분리해야 한다.
node 추가만으로 기존 owner가 이동하거나 자동 재분배된다고 단언하면 안 된다. 이 orchestration의
red gate는 `logs/20260716-091218-3232864/`에서 기존 구현의 선기동 의존을 확인했다.

## E2E/harness 대기

- `SM-C6`: remote ROUTER backpressure를 만들고,
  blocking publish의 전체 대상 원자적 admission과 timeout, non-blocking submit의 즉시 backpressure
  결과를 검증해야 한다. 일부 대상 전달을 성공으로 처리하지 않아야 하며 현재 runner에는 이 증거가 없다.

## 공통 scenario parity gap — 2026-07-29

다음 공통 scenario에는 Java actual fixture와 runner selector가 없다.

- `SM-A9`, `SM-A10`, `SM-A11`, `SM-A12`, `SM-A13`
- `SM-B0`, `SM-B10`, `SM-B11`, `SM-G5`
