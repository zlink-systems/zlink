# Kotlin SpotService E2E feature map

이 문서는 Config 2 SpotService 공통 시나리오 중 Kotlin E2E가 현재 검증하는 항목과,
public API 또는 harness 제어가 더 필요한 항목을 구분한다. server role은 public Spring starter,
`ZLinkSpotManager`, `ZLinkRouteClient`, `ZLinkSpotOutbound`, client/server channel builder, route mesh
channel builder, MeshNode builder, stream connector, `ZLinkSpotPublisherClient`를 사용한다. 대부분의
Client scenario는 HTTP endpoint와 public stream connector로 server role을 구동한다. RouteMesh의
client-side public route/spot 전송을 검증해야 하는 scenario는 별도 client driver spot을 띄워
`ZLinkRouteClient`와 `ZLinkSpotOutbound`를 직접 사용한다.

공통 E2E 문서와 다른 언어의 구현에 존재하는 public 기능이 Kotlin에 없으면 단순 미구현으로 완료
처리하지 않는다. 다만 다른 언어 구현만으로 Kotlin public contract를 새로 추가하지 않는다. spec 또는
공통 framework spec/guide에 근거가 있는 항목은 parity gap으로 관리하고, 근거가 부족한 항목은
draft/spec 검토 대상으로 분리한다. 공통 E2E 문서는 누락을 찾는 기준이지만, 새 public API를 추가할
근거로만 쓰지 않는다.

## 현재 live 검증 상태

- `logs/20260704-044437-55250` focused runner에서는 `SM-B1` 매핑의 actor-session 묶음이 통과했다.
  이 묶음은 `SM-B1`, `SM-B3`, `SM-B5`, `SM-B6`, `SM-B7`, `SM-B8`, `SM-D1`, `SM-D3`,
  `SM-D4`, `SM-D5`, `SM-D6`, `SM-D7`, `SM-D8`, `SM-D9`, `SM-D10`, `SM-D11`, `SM-D13`과
  `spot-service kotlin e2e focused modes=actor-session result=passed`를 확인한다.
- `logs/20260704-045245-65740` focused runner에서는 `SM-C4`가 `gateway-publish` mode로 통과했다.
  이 검증은 Java SpotService와 같은 수준으로 Gateway role의 public `ZLinkSpotPublisherClient.publish(...)`
  호출과 gateway publish evidence를 확인한다.
- `logs/20260704-045322-67016` full runner에서는 registry role 없이 Play/Gateway/MultiNode/Session role이
  공식 Redis location store extension을 같은 endpoint와 실행별 key prefix로 공유했고, 구현된 모든 mode와
  최종 `spot-service kotlin e2e result=passed` marker가 통과했다.
- actor-session topology는 Play가 소유하는 일반 `room-a`/`room-b`와 충돌하지 않도록 전용
  `actor-room-a`/`actor-room-b` spot을 사용한다. 같은 actor-session client와 Session role은 같은 Redis
  location store prefix를 공유한다.

## 구현됨

- `SM-A1`: public `ZLinkSpotManager.getOrCreate`로 user spot을 생성하고 evidence로 확인한다.
- `SM-A2`: Client가 Play HTTP endpoint를 호출하고, Play가 public `ZLinkRouteClient.requestToSpot`으로
  user spot state mutation을 검증한다.
- `SM-A3`: `room-a` 요청이 해당 spot의 owner인 `play-a`에서 처리되는지 확인한다.
- `SM-A4`: 같은 `RoutingId`인 `room-a`로 반복 보낸 요청이 같은 owner 노드에 유지되는지 확인한다.
- `SM-A6`: Play-B HTTP admin endpoint가 public `ZLinkSpotManager.getOrCreate`로 만든 user spot을
  public `ZLinkSpotManager.close`로 명시 close했을 때 closing evidence가 남는지 확인한다.
- `SM-A7`: 이미 만든 spot rid를 다른 spot 타입으로 `getOrCreate`할 때 public configuration error로
  거부되고, 기존 spot이 같은 타입으로 계속 조회되는지 확인한다.
- `SM-A8`: public `context.runIoWorker(...).yield()`로 긴 I/O 작업을 spot 직렬 루프 밖에서 실행하는
  동안 같은 spot의 후속 request가 막히지 않고, yield 완료 뒤 spot state/evidence를 안전하게
  갱신하는지 확인한다.
- `SM-B1`: stream session에서 만든 local actor가 entry spot request를 처리하고, public
  `ZLinkActorContext.joinSpot`으로 user spot에 join한 뒤 user spot actor request를 처리하는지 확인한다.
- `SM-B2`: play-b stream session에서 만든 actor가 play-a의 `room-a` user spot에 join하고, cross-node
  mailbox evidence를 남기는지 확인한다. `logs/20260707-180549-2974527/client.stdout.log`에서
  `scenario SM-B2 passed`와 `remote-actor-session` 통과 marker를 확인했다.
- `SM-B3`: actor create/join/request payload의 profile, level, tag 값이 handler까지 유지되고 reply에
  같은 값이 반영되는지 확인한다.
- `SM-B4`: play-b에 bound된 actor가 play-a의 `room-a` actor handler로 request를 보내고 reply를 받는지
  확인한다. `logs/20260707-180549-2974527/play-a-evidence.json`에는 `ActorUserRequest`,
  `play-b-evidence.json`에는 `ActorCreated` evidence가 남는다.
- `SM-B5`: handler 없는 actor packet name인 `MissingActorReq`를 public stream connector request로 보냈을
  때 request가 실패하고, message-flow observer가 `SPOT_ACTOR` surface의 `HANDLER_MISSING` /
  `REPLY_ERROR` evidence를 남기는지 확인한다. `logs/20260630-035320-2565912` full runner에서
  `SM-B5` marker와 `SPOT_ACTOR/HANDLER_MISSING/REPLY_ERROR/MissingActorReq` flow evidence를 확인했다.
- `SM-B6`: user spot에 join한 actor가 public `ZLinkSpotContext.leaveActor(actor)`로 명시 leave할 때
  user spot leave evidence만 남고, stream disconnect 때는 entry spot disconnect evidence만 남는지
  확인한다. `logs/focused-actor-session-20260630-023713-2377275`에서 `SM-B6` marker와
  actor-session 통과 marker를 확인했다.
- `SM-B7`: actor create, entry request, join, user request callback/handler가 evidence marker를 남겨
  lifecycle과 packet handler 실행 순서를 확인할 수 있는지 검증한다. Session role의 `/evidence/wait`
  endpoint로 필요한 evidence marker가 모두 기록될 때까지 기다린다.
- `SM-B8`: stream session으로 bind한 actor를 public `ZLinkEntrySpotContext.destroyActor(actor)`로
  명시 파괴하고, 같은 actor로 다시 보내는 request가 실패하는지 확인한다.
  `logs/focused-actor-session-20260630-023307-2370308`에서 `SM-B8` marker와 actor-session 통과
  marker를 확인했다.
- `SM-C1`: Client가 Play HTTP endpoint를 호출하고, Play가 public `ZLinkRouteClient`로 request, send,
  timeout, 미등록 packet negative path를 검증한다.
- `SM-C2`: spot handler 안에서 public `ZLinkSpotOutbound.requestToChannel` /
  `sendToChannel`로 ChannelName에 request/send를 내보내고, 같은 handler에서 Logical Multicast를
  수행해 구독 spot의 evidence를 확인한다.
- `SM-C3`: Client가 Play HTTP endpoint를 호출하고, source spot handler가 public
  `ZLinkSpotOutbound.sendToSpot`으로 target user Spot에 command를 보내는지 확인한다. Logical Multicast
  evidence도 함께 확인한다.
- `SM-C4`: Gateway MeshNode가 public publisher client로 Logical Multicast를 제출하고,
  gateway publish 응답과 evidence를 확인한다. Java 기준 구현도 publisher role의 publish 호출을 검증하며
  Play role의 target spot 수신까지 C4 완료 조건으로 삼지 않는다.
- `SM-D1`: public stream connector와 framework `ZLinkSessionActors.bind` / `ZLinkSessionActor.relay` /
  actor `boundSession().send`로 local stream session auth, actor relay, actor push를 검증한다. Session
  evidence wait endpoint도 actor-session mode 안에서 함께 검증한다.
- `SM-D2`: play-b stream session에 bound된 actor가 play-a user spot request 처리 중 같은 bound session으로
  push를 돌려받는지 확인한다. `logs/20260707-180549-2974527/client.stdout.log`에서
  `scenario SM-D2 passed`를 확인했다.
- `SM-D3`: 같은 stream actor가 entry spot request와 public `ZLinkActorContext.joinSpot`으로 join한
  user spot request에서 모두 relay/push를 수행하는지 비교한다. `actor-session` mode는
  `logs/focused-actor-session-20260630-020720-2316864`에서 `SM-D3` marker와 `ActorEntryRequest`,
  `ActorUserJoined`, `ActorUserRequest` evidence를 확인했다.
- `SM-D4`: 한 stream session에 두 actor를 bind하고 `actor-id` metadata로 각 actor request와 push가
  분기되는지 확인한다. `actor-id` 없이 보내는 request는 다중 bind 상태에서 실패해야 한다.
  `logs/focused-actor-session-20260630-021110-2323861`에서 `SM-D4` marker, 두 actor의
  `ActorSessionBound`, `ActorEntryRequest` evidence를 확인했다.
- `SM-D4A` (runtime contract 구현): 공용 JVM runtime의
  `ZLinkSessionActorBindingContractTest`가 Session replacement와 stale binding token을 검증한다.
  `KotlinFrameworkExtensionsContractTest`는 exact generation의 logical disconnect stage를
  coroutine 경계에서 기다린다.
- `SM-D4B` (runtime contract 구현): 공용 JVM runtime test가 bind 이후 hidden bind 없이 저장한
  route로 한 번만 relay하고 stale 결과를 typed failure로 전달하는지 검증한다.
- `SM-D5`: physical stream disconnect 때 Framework가 고정한 모든 bound Actor에 disconnect를
  자동 통지하고 entry spot의 callback evidence가 남는지 확인한다. session handler는 Actor 목록을
  순회하지 않으며, public `ZLinkSessionActor.notifyDisconnected`는 별도 logical notification에만
  사용한다. `logs/focused-actor-session-20260630-021110-2323861`에서 `SM-D5` marker,
  `ActorDisconnectNotified`, `ActorEntryDisconnected` evidence를 확인했다.
- `SM-D5A` (runtime contract 구현): Kotlin projection test가 선택한 exact binding의
  `notifyDisconnected` 완료만 기다리고 다른 generation binding에 영향을 주지 않는지 확인한다.

`SM-D4A`·`SM-D4B`·`SM-D5`·`SM-D5A`의 process 간 transport orchestration은 Kotlin
SpotService E2E runner에 아직 추가되지 않았다. 현재 증거는 공용 JVM runtime과 Kotlin
projection의 focused contract test다.
- `SM-D6`: `session-a`와 `session-b`에 각각 연결한 stream session 중 request를 보낸 actor의 bound
  session에만 public `ZLinkSessionActor.boundSession().send` push가 전달되고, 다른 gateway의 session에는
  `ActorPushNotify`가 전달되지 않는지 확인한다. `logs/focused-actor-session-20260630-031506-2451994`에서
  `SM-D6` marker와 actor-session 통과 marker를 확인했다.
- `SM-D7`: auth 전에 actor packet을 보내면 public stream connector request가 실패하고, 새 stream
  session에서 `ActorAuthReq`로 actor를 bind한 뒤에는 같은 actor request와 push가 정상 dispatch되는지
  확인한다. `logs/focused-actor-session-20260630-022502-2354570`에서 `SM-D7` marker와
  actor-session 통과 marker를 확인했다.
- `SM-D8`: stream reconnect 중 끊긴 stream의 pending request가 실패하고, disconnect callback evidence가
  남은 뒤 같은 actor가 새 stream에서 재auth/rebind되어 request를 처리하는지 확인한다.
  `logs/focused-actor-session-20260630-025247-2408499`에서 `SM-D8` marker와 actor-session 통과
  marker를 확인했다.
- `SM-D9`: public stream connector의 `observeInbound` observer가 stream reply packet name을 관측하는지
  확인한다. `logs/focused-actor-session-20260630-021110-2323861`에서 `SM-D9` marker와 Session
  `StreamInbound` evidence를 함께 확인했다.
- `SM-D10`: **blocked — Java §12.1 상속.** `maxReceivedMessages = 1`과 manual dispatch를 사용하는
  public stream connector는 기존 actor push를 유지하고 새 push를 버리며
  `RECEIVED_MESSAGE_DROPPED`를 보고해야 한다. 공유 Java connector가 오래된 push를 제거하므로 현재
  marker는 정식 admission 의미의 완료 증거가 아니다. 수정 뒤 backpressure의 session별 격리도 함께
  검증한다.
- `SM-D11`: 같은 client process에서 public stream connector로 actor request를 처리한 뒤 Play HTTP
  endpoint를 통해 public `ZLinkRouteClient` route-channel request를 보내 stream 경로와 channel 경로가 함께 동작하는지
  확인한다. `logs/focused-actor-session-20260630-030200-2426602`에서 `SM-D11` marker와 actor-session
  통과 marker를 확인했다.
- `SM-D13`: heartbeat가 켜진 public stream connector가 일정 시간 연결을 유지하고, 같은 stream session에서
  actor request를 계속 처리하는지 확인한다. `logs/focused-actor-session-20260630-025654-2416593`에서
  `SM-D13` marker와 actor-session 통과 marker를 확인했다.
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
  Play/MultiNode Kotlin route HTTP path는 `SpotHandle` 인자로 `ZLinkRouteClient.requestToSpot` /
  `sendToSpot`을 호출하도록 정리했다.
- `SM-F3`: 같은 RouteMesh에서 일반 route-channel request/reply와 target spot request/send가 한
  channel 위에서 함께 구성되고, 일반 packet은 channel handler가 처리하는지 확인한다.
- `SM-F4`: 존재하지 않는 target spot route request가 timeout이 아닌 framework error로 실패하고,
  같은 route-mesh mode의 정상 spot routing이 계속 동작하는지 확인한다. malformed relay packet 주입은
  공통 E2E와 `.NET` 기준 모두 public E2E 표면이 아니므로 직접 scenario로 만들지 않는다.
- `SM-F5`: channel request → spot route request/send → channel request 순서의 전용 client scenario와
  `route-lifecycle` mode가 public `ZLinkRouteClient`와 `ZLinkSpotOutbound`를 직접 사용해 통과한다.
  Java 공용 framework의 `ZLinkSpotOutbound`는 `SpotHandle` 인자 전송과 request를 제공하고, client direct
  outbound도 `SpotHandle` 기반 호출을 사용한다. `logs/20260707-174936-2911529/client.stdout.log`에서
  `scenario SM-F5 passed`와 `spot-service kotlin e2e mode=route-lifecycle result=passed`를 확인했다.
- `SM-F6`: 같은 MeshName의 MeshNode 두 개가 별도 channel·Spot socket 없이 target Spot에 도달하는지
  확인한다. source spot은 public `ZLinkSpotOutbound`의 `SpotHandle` 기반 request/send로 target spot을
  호출하고, entry Spot은 public actor join 경로로 remote target Spot에 actor를 join한다.
  `logs/20260707-221028-3618449/client-spot-only-mesh.stdout.log`에서 `scenario SM-F6 passed`와
  `spot-service kotlin e2e mode=spot-only-mesh result=passed`를 확인했다.
- `SM-G2` (부분 구현): 현재 구현은 처음부터 실행 중인 play-a와 play-b에 Spot을 순서대로 만들고
  owner remap만 확인한다. MeshNode scale-out 뒤 기존 owner 유지, 새 peer와 Entry Spot readiness,
  새 node의 application join을 통한 신규 actor·Spot 배치를 검증하지 못한다. 이 조건을 갖춘 runner와
  public readiness 증거를 추가한 뒤 완료로 바꾼다.
- `SM-G3`: public stream connector 2개가 같은 user spot에 join한 뒤, 동시에 actor request와 leave
  request를 실행하고 각 actor의 join/leave lifecycle evidence가 한 번씩 남는지 확인한다.
  Play role에 빠져 있던 `LeaveActorReq` actor request handler를 같은 public handler 등록 경로로
  추가했다. `logs/20260707-183939-3102578/client.stdout.log`에서 `scenario SM-G3 passed`와
  `spot-service kotlin e2e mode=join-leave-race result=passed`를 확인했고, `play-a-evidence.json`에는
  각 actor별 `ActorUserJoined`, `ActorUserRequest`, `ActorUserLeft` evidence가 남는다.
- `SM-G4`: public stream connector로 8개 stream session을 만들고 각 session에 actor를 bind한 뒤,
  동시에 actor request를 보내 각 actor의 bound session으로만 push가 돌아오는지 확인한다.
  `logs/20260707-181906-3021418/client.stdout.log`에서 `scenario SM-G4 passed`와
  `spot-service kotlin e2e mode=bound-push-load result=passed`를 확인했고,
  `play-a-evidence.json`에는 8개의 `ActorCreated`, 8개의 `ActorEchoReq`, 각 actor별 `push-*`
  evidence가 남는다.
- `SM-D14`: public `ZLinkStreamNodeBuilder.setTlsServer(...)`로 Play TLS stream endpoint를 열고,
  self-signed certificate에 대한 strict validation 실패와 skip-validation 성공 경로의 actor
  auth/request/push를 확인한다. `logs/20260707-184546-3126591/client.stdout.log`에서
  `scenario SM-D14 passed`와 `spot-service kotlin e2e mode=stream-tls result=passed`를 확인했고,
  `play-a-evidence.json`에는 `ActorAuthReq`, `ActorCreated`, `ActorSessionBound`, `ActorEchoReq`,
  `ActorEntryRequest` evidence가 남는다.
- `SM-A5`: Kotlin framework public API를 새로 만들지 않고 E2E 내부 `ScenarioStage` wrapper가
  public spot request와 timer 표면을 감싸도록 구성했다. `logs/20260707-185955-3178546/client.stdout.log`에서
  `scenario SM-A5 passed`와 `spot-service kotlin e2e mode=stage-wrapper result=passed`를 확인했고,
  play-a evidence에는 `SpotInitialized`, `StageRequest`, `StageTimer`가 남는다.
- `SM-G1`: runner가 시작한 play-a process만 종료하고 재시작하는 focused harness로 play node crash,
  장애 중 request 실패, play-b 생존 확인, play-a 재시작 뒤 stream 재auth/rejoin/rebind를 검증한다.
  `logs/20260707-185134-3145895/client.stdout.log`에서 `scenario SM-G1 passed`와
  `spot-service kotlin e2e mode=play-crash-recovery result=passed`를 확인했다.
- `SM-D12`: `session-a`에서 `play-a`의 remote actor를 bind하고 요청 상태를 변경한 뒤 연결을 종료한다.
  `session-b`에서 같은 `ActorRef`를 다시 bind해 다음 handler sequence가 정확히 1 증가하고, 새 bound
  session으로 전달된 push가 같은 sequence를 포함하는지 확인한다.
  `logs/20260713-153718-1546351/client-session-transfer.stdout.log`에서 `scenario SM-D12 passed`와
  `spot-service kotlin e2e mode=session-transfer result=passed`를 확인했다.

## public contract parity 또는 spec 검토 대기

- `SM-B9`: entry Spot admission의 local·remote 허용과 거부를 각각 실행하고, 거부 actor가 생성되지
  않으며 caller가 timeout이 아닌 분류된 실패를 받는지 검증해야 한다. 기존 join 성공 시나리오는
  거부 경계를 증명하지 않는다.
- `SM-C5`: `play-a` Spot이 제출한 Logical Multicast를 `play-b`의 구독 Spot이 실제 수신했다는
  evidence로 검증해야 한다. 현재 `SM-C4`의 발행 측 성공은 수신 완료 근거가 아니다.
- `SM-C6`: remote ROUTER backpressure를 만들고,
  blocking publish의 전체 대상 원자적 admission과 timeout, non-blocking submit의 즉시 backpressure
  결과를 검증해야 한다. 일부 대상 전달을 성공으로 처리하지 않아야 한다.
- `SM-D15`: 별도 role의 channel request에서 actor send, bound session push와 client stream 수신까지
  이어지는 사슬을 hop별 flow trace와 함께 검증해야 한다. 기존 session이 시작한 actor relay는 이
  cross-role 시작 조건을 증명하지 않는다.

## .NET 추가 검증 파일

아래 항목은 `.NET` tree에는 client scenario file로 존재하지만, 공통 Config 2 문서와 `.NET
feature-map.ko.md`에는 scenario ID로 등록되어 있지 않다. Kotlin에서는 공통 scenario 완료와 분리해
추적한다.

- `SM-Q9`: `.NET` `Client/Scenarios/SmQ9Scenario.cs`는 MultiNode role의 두 노드에서 local spot을
  만들고 route-to-spot request가 각 노드의 spot으로 유지되는지 확인한다. Kotlin에는 MultiNode 전용
  role/project와 같은 local create/state/evidence path가 생겼고, client `multi-node` mode가 같은 흐름을
  실행한다. `logs/20260630-013008-2203737` full runner에서 `scenario SM-Q9 passed`,
  `spot-service kotlin e2e mode=multi-node result=passed`, 두 노드의 `multi-state-request` evidence를
  확인했다.
