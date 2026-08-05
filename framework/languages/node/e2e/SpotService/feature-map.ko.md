# Node.js SpotService E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-2-spot-service.ko.md`

현재 상태: Node.js `SpotService` config는 `.NET` runner처럼 `all`을 child group으로 나누어 실행한다.
`default-batch`는 SM-A1, SM-A2, SM-A3, SM-A4, SM-A5, SM-A6, SM-A7, SM-A8, SM-B1, SM-B2, SM-B3, SM-B4, SM-B5,
SM-B6, SM-B7, SM-B8, SM-B9, SM-C1, SM-C2, SM-C3, SM-C4, SM-C5, SM-D1, SM-D2, SM-D3, SM-D4, SM-D5, SM-D6,
SM-D7, SM-D8, SM-D9, SM-D10, SM-D11, SM-D12, SM-D13, SM-D14, SM-D15, SM-E1, SM-E2, SM-E3, SM-E4, SM-F1,
SM-F2, SM-F3, SM-F4, SM-F5를 operation group 단위로 실행하고,
outer `all`은 이어서 SM-F6, SM-G2, SM-G3, SM-G4, SM-G1을 별도 child scenario로 실행한다.
공통 Config 2에 없는 SM-Q9는 보조 operation으로만 선택 실행한다.
SM-F4는 존재하지 않는 SpotId의 request·send terminal, close 뒤 같은 SpotId 재생성, stale `SpotRef`
close 거절과 새 incarnation 유지, ChannelName·RID direct 지속을 public surface로 검증한다. 이 문서는 `.NET`
`framework/languages/dotnet/e2e/SpotService/feature-map.ko.md`와 공통 문서의 scenario ID를 기준으로
포팅 범위를 고정한다. 내부 helper나 raw-frame 우회로 gap을 완료 표시하지 않는다.
서버 역할은 `E2E_START_ORDER=reverse`와 고정 seed `shuffle:20260715`로도 시작하며, 선택된 순서로
모든 역할을 시작한 뒤 readiness를 확인한다. 두 변형의 `SM-A1` runner가 통과했다.
`.NET`의 `SmQ9Scenario.cs`는 공통 문서에 없는 보조 operation이므로 scenario 표가 아니라
`porting-inventory.ko.md`의 보조 항목에서 추적한다. Node.js에는 MultiNode role과 선택 operation을
추가했고 public route-to-spot request가 각 local owner spot으로 도달하는지 검증했다. `all` PASS: `logs/20260702-064908-43296`

| Scenario | 상태 | 근거 |
|----------|------|------|
| SM-A1 | 구현 | play-a의 public spot manager endpoint `/spot/create`가 requested spot rid를 생성하고 `create-spot` evidence marker를 남기는지 검증했다. 로그: `logs/20260630-074201-3148526` |
| SM-A2 | 구현 | user spot 생성 뒤 public `/spot/state/request` endpoint가 `ZLinkSpotOutbound.requestToSpot(...)`와 resolver 기반 route로 `StateReq`를 전달하고 state/evidence를 검증한다. 선택 PASS: `logs/20260630-082158-3260793` |
| SM-A3 | 구현 | play-a에 unique user spot을 만들고 public routed `StateReq`가 play-a spot에만 도달하는지 검증한다. 선택 PASS: `logs/20260630-081839-3241783` |
| SM-A4 | 구현 | owner spot을 만든 뒤 같은 key가 가리키는 spot rid로 routed `StateReq` noop request를 보내 owner routing이 play-a에 머무는지 검증한다. 선택 PASS: `logs/20260630-081825-3239853` |
| SM-A5 | 구현 | app-level `ScenarioStage` wrapper, `StageProbeReq`, `StageTimerStartMsg`, stage timer handler가 routed readiness request 뒤 같은 spot serial 흐름에서 동작하는지 검증한다. 선택 PASS: `logs/20260630-081839-3241830` |
| SM-A6 | 구현 | `/spot/create`와 `/spot/close`가 public spot manager create/close 경로를 사용하고 `spot-initialize`/`spot-closing` lifecycle marker를 검증한다. 로그: `logs/20260630-074201-3148526` |
| SM-A7 | 구현 | 같은 spot rid를 `ScenarioUserSpot`으로 만든 뒤 `ScenarioAlternateSpot`으로 다시 `getOrCreate`해 public `SpotTypeMismatch` error와 evidence marker를 검증한다. 로그: `logs/20260630-074201-3148526` |
| SM-A8 | 구현 | Play HTTP endpoint가 target Spot으로 worker request를 보내고, Spot handler가 public `spot.context.runIoWorker(...).yield()`로 I/O 작업을 시작한다. completion 전에 같은 Spot의 별도 state request가 처리되는 evidence 순서를 검증한다. 로그: `logs/20260715-092022-2650621` |
| SM-B1 | 구현 | Session role, stream connector `AuthReq`, Play control RouteMesh `EnsureActorReq`, local actor bind, `ActorPingReq` relay, `entry-created` -> `entry-joined` lifecycle evidence를 검증한다. 선택 PASS: `logs/20260630-070738-3054201` |
| SM-B2 | 구현 | Session role에서 `play-b` control route로 actor ensure를 보내고, bound actor request가 `play-b` spot actor까지 cross-node relay되며 `entry-created` -> `entry-joined` lifecycle evidence가 remote node에 남는지 검증한다. 선택 PASS: `logs/20260630-070751-3054866` |
| SM-B3 | 구현 | Session stream auth 뒤 bound actor에 `ComplexActorReq`를 relay하고 scalar, array, dictionary payload가 reply와 `actor-complex` evidence에 그대로 남는지 검증한다. 선택 PASS: `logs/20260629-201946-1303393`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-B4 | 구현 | Session stream auth가 `play-b` actor를 bind한 뒤 `ActorPingReq`가 cross-node로 `play-b` actor에서 처리되고 reply가 돌아오는지 검증한다. 선택 PASS: `logs/20260629-202300-1319449`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-B5 | 구현 | missing actor handler request가 stream error reply로 실패하고 `dispatch-error\|surface=spotActor\|kind=actorRequest\|reason=handlerMissing\|action=replyError` evidence를 남기는지 검증한다. 선택 PASS: `logs/20260630-072224-3098758` |
| SM-B6 | 재검증 필요 | explicit leave는 `spot-actor-left` evidence만 남긴다. Physical stream close는 application이 Actor를 선택해 통지하지 않고 Framework가 current binding 전체에 자동 통지해야 한다. handler loop를 제거했으므로 최신 runtime runner 증거가 필요하다. |
| SM-B7 | 구현 | 같은 actor에 `ActorPingReq` 두 개를 연속 전송해 `entry-created` -> `entry-joined` -> packet dispatch 순서와 `seen=1`, `seen=2` 직렬 처리 evidence를 검증한다. 선택 PASS: `logs/20260630-070802-3055574` |
| SM-B8 | 구현 | Entry Spot actor handler에서 public `entrySpot.context.destroyActor(...)`를 호출하고, post-destroy `SnapshotReq`가 stream error reply로 실패하며 `actor-destroyed` evidence가 남는지 검증한다. 선택 PASS: `logs/20260630-072210-3097534` |
| SM-B9 | 구현 | Entry Spot `onActorJoin(...)`이 actor id별 admission 결과를 반환하고 거부 actor는 request가 stream error reply로 실패하며 accepted actor는 정상 reply되는지 검증한다. 선택 PASS: `logs/20260707-195152-3345108` |
| SM-C1 | 구현 | public `ZLinkSpotOutbound.requestToSpot(...)`/`sendToSpot(...)`로 channel -> spot request, command, slow request timeout, timeout 이후 정상 request를 검증한다. 선택 PASS: `logs/20260630-081924-3247169` |
| SM-C2 | 구현 | Spot handler가 public `requestToChannel(...)`, `sendToChannel(...)`, `publish(...)`를 사용해 channel echo, channel notify, spot event publish를 모두 evidence로 남긴다. missing channel request/send negative도 message-flow observer evidence로 검증한다. 선택 PASS: `logs/20260629-205741-1493698`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-C3 | 구현 | spot -> spot request/send/publish/timeout/negative handlers를 public `ZLinkSpotOutbound.requestToSpot(...)`, `sendToSpot(...)`, `publish(...)`로 검증한다. 선택 PASS: `logs/20260630-082100-3253716` |
| SM-C4 | 구현 | local Spot factory가 없는 Gateway MeshNode가 같은 RouteMesh의 public node publisher로 ChannelName과 topic을 제출한다. Play-A의 구독 Spot은 marker를 한 번 받고 미구독 Spot은 받지 않는다. 별도 publish transport endpoint 없이 Gateway와 Play-A의 기존 Router 연결만 사용한다. 선택 PASS: `log/20260729-051542-2246502/` |
| SM-C5 | 전환 필요 | Play-A의 Logical Multicast가 Play-B의 구독 Spot에 delivery evidence를 남기고 미구독 Spot에는 남기지 않는지 검증한다. 기존 PASS 로그는 이전 topology의 증거이며 10.0.0 구현 뒤 다시 실행한다. |
| SM-C6 | 전환 필요 | remote ROUTER backpressure를 만들고 blocking publish의 timeout, non-blocking submit의 즉시 backpressure 결과와 앞에서 수락된 target의 전달 유지를 검증해야 한다. 현재 runner에는 이 증거가 없다. |
| SM-D1 | 구현 | Session HTTP endpoint가 control RouteMesh로 `play-a` readiness를 확인하고, stream auth로 bind한 local actor에 `ActorPushReq`를 relay한다. actor handler는 public `actor.context.boundSession.send(...)`로 같은 stream client에 `ActorPushNotify`를 보내고 reply와 push payload를 검증한다. 선택 PASS: `logs/20260629-211928-1555127`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D2 | 구현 | Session HTTP endpoint가 control RouteMesh로 `play-b` readiness를 확인하고, stream auth로 bind한 remote actor에 `ActorPushReq`를 relay한다. `play-b` actor handler가 public bound session push로 `ActorPushNotify`를 보내고 reply node가 `play-b`인지 검증한다. 선택 PASS: `logs/20260629-212246-1563843`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D3 | 구현 | entry actor stream bind 뒤 `ActorPushReq` reply와 bound session push를 검증한다. user spot bind는 `UserSpotAuthReq`로 spot/actor join marker를 남기고 `UserActorPingReq`/`UserActorPushReq` relay reply, user spot rid, push payload, `actor-pingMsg` evidence를 검증한다. 선택 PASS: `logs/20260629-212739-1577626`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D4 | 구현 | `MultiBindReq`가 한 stream session에 두 actor를 bind하고, subsequent request가 stream metadata `actor-id`로 대상 actor를 선택한다. 각 actor request/reply, actor push, id 없는 request 실패를 검증한다. 선택 PASS: `logs/20260629-213206-1588322`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D4A | 구현 | `session A to B rebind fences stale relay and late disconnect without touching other actors`가 same-generation rebind, stale typed error, late disconnect 무효화와 다른 Actor binding 유지를 검증한다. |
| SM-D4B | 구현 | `stored actor route relays once without actor ref lookup or hidden retry`가 bind 뒤 resolver call 0, valid stored route 1회와 stale route single attempt를 검증한다. |
| SM-D5 | 구현 | `physical disconnect dedupes a racing logical notification and retains actor state inputs`와 stream-session automatic disconnect test가 fixed snapshot, exact callback 1회, all-settled failure cleanup과 Actor state 입력 유지를 검증한다. |
| SM-D5A | 구현 | `logical actor disconnect waits for one callback and keeps the physical connection and other binding`이 선택 callback terminal 대기, 다른 Actor 무영향과 connection 유지를 검증한다. |
| SM-D6 | 구현 | bound consumer와 별도 consumer를 각각 `session-a`, `session-b` stream session에 연결하고, `ActorPushReq`로 발생한 `ActorPushNotify`가 target actor에 bind된 consumer에게만 전달되는지 검증한다. 별도 consumer는 다른 actor에 bind되어 있으며 target actor push count가 0인지 확인한다. 선택 PASS: `logs/20260629-213945-1613927`; `all` PASS: `logs/20260702-064908-43303` |
| SM-D7 | 구현 | stream connector가 `AuthReq`로 actor bind를 완료하고, 같은 stream의 `ActorPingReq`가 bound actor로 dispatch되어 reply payload가 유지되는지 검증한다. 선택 PASS: `logs/20260629-214310-1624231`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D8 | 구현 | slow actor request가 pending인 상태에서 stream connector를 close하면 pending request가 실패하고 자동 재전송되지 않는지 확인한다. 이후 새 stream connector가 같은 actor id로 다시 auth/rebind하고 `ActorPingReq`가 정상 reply되는지 검증한다. 선택 PASS: `logs/20260629-214843-1639970`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D9 | 구현 | stream connector에 public `observeInbound(...)`를 `connect()` 전에 등록하고, stream auth 뒤 두 번의 `ActorPingReq` reply를 받는 동안 inbound response frame의 kind, request sequence, payload length가 관측되는지 검증한다. 선택 PASS: `logs/20260629-215409-1654253`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D10 | 전환 필요 | 공통 connector 계약대로 `maxReceivedMessages=1`에서 이미 수락한 메시지를 유지하고 이후 새 send를 버리는지 payload identity로 확인해야 한다. 현재 검사는 `ReceivedMessageDropped` 횟수와 총 수신 수만 확인하므로 어느 메시지가 수락·폐기됐는지 구분하지 못한다. 기존 request route 생존과 다른 actor stream 격리 증거는 유지한다. |
| SM-D11 | 구현 | 같은 client flow에서 stream `ActorPingReq`와 Session HTTP channel control-pingMsg을 차례로 호출해 stream reply와 channel reply가 서로 간섭 없이 각 경로로 돌아오는지 검증한다. 선택 PASS: `logs/20260629-220146-1678135`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D12 | 구현 | runner가 `session-a`와 별도 `session-b` stream host를 함께 시작하고, client가 `session-a`에서 actor state를 만든 뒤 close하고 `session-b`로 재auth/rebind한다. 이후 `SnapshotReq`와 `ActorPushReq`로 play-a actor state가 보존되고 push가 새 stream으로 돌아오는지 검증한다. 선택 PASS: `logs/20260629-220618-1691419`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D13 | 구현 | stream connector heartbeat를 public option으로 켜고 200ms interval, 2s timeout으로 여러 heartbeat 주기 동안 stream auth 상태가 유지되는지 검증한다. 현재 `.NET` 기준 scenario와 동일하게 정상 heartbeat 유지 경로만 완료로 본다. 선택 PASS: `logs/20260629-221012-1704641`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D14 | 전환 필요 | Node stream node builder의 public `setTlsServer(...)`로 `wss://` endpoint를 구성하고 auth/request/push를 검증해야 한다. 현재 TypeScript connector에는 자체 서명 인증서를 신뢰하도록 지정하는 public option이 없으므로, 검증 생략 option을 가정하거나 strict 실패와 성공을 모두 완료로 표시하지 않는다. |
| SM-D15 | 구현 | Session -> actor -> bound session push chain이 cross-role로 이어져 request reply와 push payload가 같은 actor/session evidence로 확인되는지 검증한다. 선택 PASS: `logs/20260707-195152-3345108`; `all` PASS: `logs/20260708-062031-351969` |
| SM-E1 | 구현 | public `setMessageFlowObserver(...)`, `ZLinkSpotOutbound.requestToSpot(...)`, `sendToSpot(...)`로 missing handler request/command 경로를 만들고 SpotRoute `handlerMissing` evidence를 검증한다. 선택 PASS: `logs/20260630-082045-3252646` |
| SM-E2 | 구현 | public `spot.context.addTimer(...)`로 lifecycle timer를 등록하고 `timer-basic` marker가 두 번 이상 발생한 뒤 spot close가 성공하는지 검증한다. 로그: `logs/20260630-074201-3148526` |
| SM-E3 | 구현 | public `spot.context.addTimer(...)`와 `spot.context.close(...)`로 idle timer close, `spot-closing` evidence, close 이후 routed request 실패를 검증한다. 선택 PASS: `logs/20260630-081940-3248210` |
| SM-E4 | 구현 | public `spot.context.addTimer(...)`의 `ZLinkTimerOverrunPolicy` 세 가지를 등록하고 `delivery`/`scheduled`/`skipped` evidence로 skip, bounded catch-up, delay-next-tick 의미를 검증한다. 로그: `logs/20260630-074201-3148526` |
| SM-F1 | 구현 | target spot을 만든 뒤 route client 경로의 `/spot/state/request`와 `/spot/state/command`를 검증한다. 선택 PASS: `logs/20260630-082100-3253769` |
| SM-F2 | 구현 | target spot request/command selectable scenario가 public route-to-spot path로 state reply와 command evidence를 검증한다. 선택 PASS: `logs/20260630-082100-3253756` |
| SM-F3 | 구현 | Spot 생성 결과의 dynamic owner RID를 사용한다. Mesh-level `addRequestHandler(...)`, Channel-level `channel(...).server().addRequestHandler(...)`와 global SpotId request를 같은 process에서 제출하고 세 handler의 exactly-once evidence를 검증했다. 선택 PASS: `log/20260729-043537-560993` |
| SM-F4 | 구현 | Missing SpotId request는 `requestTargetNotFound`, send는 `spotRouteNotFound`로 끝나며 activation evidence가 없다. close·recreate 뒤 이전 `SpotRef` exact close는 `spotGenerationStale`이고, 새 `SpotRef` exact close와 ChannelName·RID direct request는 성공한다. 선택 PASS: `log/20260729-044819-1007998` |
| SM-F5 | 구현 | Framework가 선택한 owner와 반대 node에서 ChannelName request와 global SpotId request를 처리한다. owner에서 Spot을 닫은 뒤 같은 caller의 Spot request만 typed failure로 끝나고 ChannelName request는 계속 처리된다. 선택 PASS: `log/20260729-041927-3968` |
| SM-F6 | 구현 | 같은 MeshName의 MeshNode 두 개만 구성한다. Runtime placement weight로 source와 target process를 분리한 뒤 global SpotId request·send와 deferred Actor join이 target owner에서 처리되는지 검증한다. Actor는 Snapshot relocation policy와 Relocation Store를 사용한다. 호출자는 owner RID·generation·route frame을 만들지 않으며 별도 Spot socket도 추가하지 않는다. 선택 PASS: `log/20260729-050702-1863756` |
| SM-G1 | 구현 | stream auth로 `play-a`/`play-b` actor를 각각 bind하고, `play-a` `/crash` endpoint로 프로세스를 종료한 뒤 `play-a` actor request 실패, `play-b` survivor request 유지, `session-b`에서 `play-b`로 재auth/rebind 복구를 검증했다. 선택 PASS: `logs/20260629-223922-1778101`; `all` PASS: `logs/20260708-062230-357711` |
| SM-G2 | 구현 | node A에 기존 Spot·actor를 만든 뒤 node B를 실제 추가하고, B peer의 actor capability와 Entry Spot handle readiness를 기다린다. 기존 owner A 유지와 B의 로컬 manager·Entry Spot handler를 통한 신규 Spot·actor 배치를 함께 검증한다. 선택 PASS: `log/20260716-110658-3617643` |
| SM-G3 | 구현 | 같은 user spot에 두 stream client를 연결해 `UserSpotAuthReq`, concurrent `UserActorPingReq`, `LeaveReq`를 실행하고 actor별 `spot-actor-joined`/`spot-actor-left` evidence가 1회씩 남는지 검증했다. 선택 PASS: `logs/20260629-224535-1792721`; `all` PASS: `logs/20260708-062135-354732` |
| SM-G4 | 구현 | 여섯 stream client를 각각 다른 actor에 bind한 뒤 `.NET` 기준처럼 순차적으로 `ActorPushReq`를 보내 reply와 `ActorPushNotify`가 각 actor/session으로만 돌아오는지 검증했다. 선택 PASS: `logs/20260629-225216-1811400`; `all` PASS: `logs/20260708-062201-355741` |

## 후속 계약 판정

| 묶음 | Scenario | 판정 | 다음 작업 |
|------|----------|------|-----------|
| routed spot request | `SM-A2`, `SM-A3`, `SM-A4`, `SM-A5`, `SM-C1`, `SM-C3`, `SM-E1`, `SM-E3`, `SM-G2`, `sm-q9` | 구현 | Node spot spec의 Spot context outbound와 resolver 기반 route를 기존 public surface로 검증했다. 선택 PASS 로그는 각 scenario 행에 남겼다. |
| route client target spot | `SM-F1`, `SM-F2`, `SM-F3`, `SM-F4`, `SM-F5` | 구현 | 다섯 scenario를 current public surface로 검증했다. `SM-F3`은 Mesh-level RID direct, ChannelName과 global SpotId handler namespace를 별도 evidence로 구분한다. |
| stream TLS server | `SM-D14` | 전환 필요 | Node stream node builder의 public `setTlsServer(...)`와 `wss://` endpoint를 사용한다. connector가 지원하지 않는 인증서 검증 생략 option을 전제로 하지 않고, 공개 TLS 신뢰 설정과 실제 auth/request/push 증거가 마련된 뒤 완료로 바꾼다. |

검증:

- `timeout 1200s framework/languages/node/e2e/SpotService/run_e2e.sh`
  - PASS: `logs/20260708-062031-351962` (`default-batch`, `SM-F6`, `SM-G2`, `SM-G3`, `SM-G4`, `SM-G1`, `SM-Q9`; 당시 SM-Q9 포함)
  - `default-batch` child PASS: `logs/20260708-062031-351969` (`SM-A1`, `SM-A2`, `SM-A3`, `SM-A4`, `SM-A5`, `SM-A6`, `SM-A7`, `SM-A8`, `SM-B1`, `SM-B2`, `SM-B3`, `SM-B4`, `SM-B5`, `SM-B6`, `SM-B7`, `SM-B8`, `SM-B9`, `SM-C1`, `SM-C2`, `SM-C3`, `SM-C4`, `SM-C5`, `SM-D1`, `SM-D2`, `SM-D3`, `SM-D4`, `SM-D5`, `SM-D6`, `SM-D7`, `SM-D8`, `SM-D9`, `SM-D10`, `SM-D11`, `SM-D12`, `SM-D13`, `SM-D14`, `SM-D15`, `SM-E1`, `SM-E2`, `SM-E3`, `SM-E4`, `SM-F1`, `SM-F2`, `SM-F4`)
- 선택 scenario: `timeout 180s framework/languages/node/e2e/SpotService/run_e2e.sh SM-F5`
  - PASS: `log/20260729-041927-3968` (동적 owner `play-b`, non-owner `play-a`의 remote SpotId request, local Channel candidate, close 뒤 Channel 지속)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B1`
  - PASS: `logs/20260630-070738-3054201` (`entry-created`, `entry-joined`, actor pingMsg evidence 확인)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-A3`
  - PASS: `logs/20260630-081839-3241783` (unique user spot routed `StateReq`)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-A4`
  - PASS: `logs/20260630-081825-3239853` (owner spot routed `StateReq`)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-A5`
  - PASS: `logs/20260630-081839-3241830` (stage request/timer)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-F1`
  - PASS: `logs/20260630-082100-3253769` (target spot request/command)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-F2`
  - PASS: `logs/20260630-082100-3253756` (target spot request/command)
- 선택 scenario: `framework/languages/node/e2e/SpotService/run_e2e.sh SM-F4`
  - PASS: `log/20260729-044819-1007998` (missing request·send typed failure, recreate generation fence, stale exact close, ChannelName·RID direct 지속)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-G1`
  - PASS: `logs/20260629-223922-1778101` (play-a crash isolation, play-b survivor, play-b rebind recovery)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-G2`
  - PASS: `logs/20260630-082118-3256210` (owner remap routed `StateReq`)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-G3`
  - PASS: `logs/20260629-224535-1792721` (concurrent user spot join/request/leave evidence)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-G4`
  - PASS: `logs/20260629-225216-1811400` (many bound session push isolation)
- 보조 operation: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh sm-q9`
  - PASS: `logs/20260630-082118-3256244` (MultiNode local owner route-to-spot)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B2`
  - PASS: `logs/20260630-070751-3054866` (remote `play-b` `entry-created`, `entry-joined`, actor pingMsg evidence 확인)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B3`
  - PASS: `logs/20260629-201946-1303393` (complex actor request payload fidelity)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B4`
  - PASS: `logs/20260629-202300-1319449` (remote actor request)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B5`
  - PASS: `logs/20260630-072224-3098758` (`handlerMissing` evidence와 stream error reply 확인)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B6`
  - PASS: `logs/20260630-073618-3133487` (explicit leave와 stream close disconnect callback evidence 구분)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B7`
  - PASS: `logs/20260630-070802-3055574` (`entry-created` -> `entry-joined` -> actor packet 순서 evidence 확인)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-B8`
  - PASS: `logs/20260630-072210-3097534` (`actor-destroyed` evidence와 post-destroy stream error reply 확인)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-C1`
  - PASS: `logs/20260630-081924-3247169` (request/send/slow timeout/post-timeout request)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-C3`
  - PASS: `logs/20260630-082100-3253716` (spot-to-spot request/send/publish/timeout/negative)
- 선택 scenario: `framework/languages/node/e2e/SpotService/run_e2e.sh SM-C4`
  - PASS: `log/20260729-051542-2246502/` (local Spot factory가 없는 Gateway publish, Play-A 구독 Spot 1회 수신과 미구독 Spot 미수신 확인)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D1`
  - PASS: `logs/20260629-211928-1555127` (local stream auth, actor request relay, bound session push)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D5`
  - PASS: `logs/20260630-073619-3133519` (stream close 후 `session-disconnected`/`entry-disconnected` evidence 확인)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D9`
  - PASS: `logs/20260629-215409-1654253` (stream inbound observer response frame 관측)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D10`
  - PASS: `logs/20260629-215844-1670329` (bounded received-message drop, request route 생존, 다른 stream push 격리)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D11`
  - PASS: `logs/20260629-220146-1678135` (stream actor request와 channel control request 혼합)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D12`
  - PASS: `logs/20260629-220618-1691419` (`session-a`에서 `session-b`로 재auth/rebind 후 actor state 보존)
- 선택 scenario: `timeout 420s framework/languages/node/e2e/SpotService/run_e2e.sh SM-D13`
  - PASS: `logs/20260629-221012-1704641` (heartbeat-enabled stream 유지)
- 선택 scenario: `timeout 360s framework/languages/node/e2e/SpotService/run_e2e.sh SM-E3`
  - PASS: `logs/20260630-081940-3248210` (idle close와 closed target route failure)
# CA-D78 Session Actor binding runtime evidence

- `SA-BIND-01`, `SA-BIND-02`: runtime contract test에서 한 Session의 multi-Actor binding과
  binding token 교체 뒤 stale operation 차단을 검증한다.
- `SA-ROUTE-01`, `SA-ROUTE-02`, `SA-MOVE-06`: relay는 registry에 저장한 exact binding
  route를 사용한다. Internal route refresh는 같은 `ObjectGeneration`만 허용하고 새
  incarnation은 explicit bind로만 교체한다.
- `SA-DISC-01`, `SA-DISC-03`, `SA-DISC-04`, `SA-DISC-05`: physical STREAM disconnect는
  current binding snapshot 전체를 deadline 안에서 all-settled로 통지하고 callback 실패와
  관계없이 binding cleanup을 완료한다. Focused contract test는
  `physical stream disconnect automatically notifies every captured actor and always cleans up`이다.
- CA-D78 focused runner 5개는
  `node --test --test-name-pattern="session A to B|stored actor route|logical actor disconnect|physical disconnect dedupes|internal route refresh preserves" test/contract/stream-runtime.test.js`
  실행에서 5/5 PASS했다. Node workspace build도 PASS했다.
- 공통 E2E scenario의 실제 process 간 검증은 새 scenario porting 뒤 별도 PASS log로 갱신한다.
