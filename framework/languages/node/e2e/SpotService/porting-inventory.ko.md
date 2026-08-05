# Node.js SpotService E2E 포팅 인벤토리

기준 문서: `framework/doc/framework/common/e2e/config-2-spot-service.ko.md`

기준 구현: `framework/languages/dotnet/e2e/SpotService`

현재 상태: Node.js `SpotService` config는 `.NET` runner처럼 `all`을 child group으로 나누어 실행한다.
`default-batch`는 SM-A1, SM-A2, SM-A3, SM-A4, SM-A5, SM-A6, SM-A7, SM-A8, SM-B1, SM-B2, SM-B3, SM-B4, SM-B5,
SM-B6, SM-B7, SM-B8, SM-B9, SM-C1, SM-C2, SM-C3, SM-C4, SM-C5, SM-D1, SM-D2, SM-D3, SM-D4, SM-D5, SM-D6,
SM-D7, SM-D8, SM-D9, SM-D10, SM-D11, SM-D12, SM-D13, SM-D14, SM-D15, SM-E1, SM-E2, SM-E3, SM-E4, SM-F1,
SM-F2, SM-F3, SM-F4, SM-F5를 operation group 단위로 실행하고,
outer `all`은 이어서 SM-F6, SM-G2, SM-G3, SM-G4, SM-G1을 별도 child scenario로 실행한다.
공통 Config 2에 없는 SM-Q9는 보조 operation으로만 선택 실행한다.
SM-F4는 존재하지 않는 location의 request 실패를 선택 scenario로 검증했다. malformed relay packet 주입은 public route-client 표면으로 만들 수 없으므로 public E2E 직접 대상에서 제외한다. 이 inventory는 `.NET` 기준 파일과 공통 scenario ID를 빠뜨리지 않고,
구현된 범위는 `done`으로 기록한다.

## Scenario

| Scenario | .NET 기준 파일 | Node.js 대상 파일 | 상태 | 비고 |
|----------|----------------|-------------------|------|------|
| SM-A1 | `Client/Scenarios/SmA1Scenario.cs` | `Client/Scenarios/sm-a1-scenario.ts` | done | entry spot 생성과 request. PASS: `logs/20260630-074201-3148526` |
| SM-A2 | `Client/Scenarios/SmA2Scenario.cs` | `Client/Scenarios/sm-a2-scenario.ts`, `Server/Play/Handlers/state-req-handler.ts` | done | self-contained selectable scenario가 user spot 생성 뒤 public `ZLinkSpotOutbound.requestToSpot(...)`로 routed `StateReq` state/evidence를 검증. PASS: `logs/20260630-082158-3260793` |
| SM-A3 | `Client/Scenarios/SmA3Scenario.cs` | `Client/Scenarios/sm-a3-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | unique spot 생성 뒤 public routed `StateReq`가 play-a spot에만 도달하는지 검증. PASS: `logs/20260630-081839-3241783` |
| SM-A4 | `Client/Scenarios/SmA4Scenario.cs` | `Client/Scenarios/sm-a4-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | owner spot 생성 뒤 같은 key의 owner spot rid에 routed `StateReq`를 보내 owner routing이 play-a에 머무는지 검증. PASS: `logs/20260630-081825-3239853` |
| SM-A5 | `Client/Scenarios/SmA5Scenario.cs` | `Client/Scenarios/sm-a5-scenario.ts`, `Server/Play/Handlers/stage-handlers.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | routed readiness request 뒤 app-level stage request/timer handler가 같은 spot serial 흐름에서 동작하는지 검증. PASS: `logs/20260630-081839-3241830` |
| SM-A6 | `Client/Scenarios/SmA6Scenario.cs` | `Client/Scenarios/sm-a6-scenario.ts` | done | spot initialize/explicit close lifecycle. PASS: `logs/20260630-074201-3148526` |
| SM-A7 | `Client/Scenarios/SmA7Scenario.cs` | `Client/Scenarios/sm-a7-scenario.ts` | done | public SpotTypeMismatch negative path. PASS: `logs/20260630-074201-3148526` |
| SM-A8 | `Client/Scenarios/SmA8Scenario.cs` | `Client/Scenarios/sm-a8-scenario.ts` | done | worker offload and interleaved spot turn via public `executeOnSpot(...)`. PASS: `logs/20260630-074201-3148526` |
| SM-B1 | `Client/Scenarios/SmB1Scenario.cs` | `Client/Scenarios/sm-b1-scenario.ts`, `Server/Session/`, `Server/Play/Handlers/control-handlers.ts`, `Server/Play/Spots/scenario-actors.ts` | done | Session stream auth, Play control route actor ensure, local actor bind, actor pingMsg relay, `entry-created` -> `entry-joined` lifecycle evidence. PASS: `logs/20260630-070738-3054201` |
| SM-B2 | `Client/Scenarios/SmB2Scenario.cs` | `Client/Scenarios/sm-b2-scenario.ts`, `Server/Session/`, `Server/Play/Handlers/control-handlers.ts`, `Server/Play/Spots/scenario-actors.ts` | done | `play-b` remote actor ensure, actor bind, actor pingMsg relay, remote node `entry-created` -> `entry-joined` lifecycle evidence. PASS: `logs/20260630-070751-3054866` |
| SM-B3 | `Client/Scenarios/SmB3Scenario.cs` | `Client/Scenarios/sm-b3-scenario.ts`, `Server/Play/Spots/scenario-actors.ts` | done | complex actor request scalar/array/dictionary payload fidelity. 선택 PASS: `logs/20260629-201946-1303393`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-B4 | `Client/Scenarios/SmB4Scenario.cs` | `Client/Scenarios/sm-b4-scenario.ts`, `Server/Session/`, `Server/Play/Spots/scenario-actors.ts` | done | session-a stream에서 bind한 `play-b` actor로 cross-node request/reply 검증. 선택 PASS: `logs/20260629-202300-1319449`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-B5 | `Client/Scenarios/SmB5Scenario.cs` | `Client/Scenarios/sm-b5-scenario.ts` | done | missing actor handler request가 stream error reply로 실패하고 `handlerMissing` evidence를 남김. PASS: `logs/20260630-072224-3098758` |
| SM-B6 | `Client/Scenarios/SmB6Scenario.cs` | `Client/Scenarios/sm-b6-scenario.ts`, `Server/Session/Spots/scenario-actors.ts`, `Server/Play/Infrastructure/actor-spot-store.ts` | done | explicit leave와 stream close disconnect callback을 구분해 검증. 선택 PASS: `logs/20260630-073618-3133487`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-B7 | `Client/Scenarios/SmB7Scenario.cs` | `Client/Scenarios/sm-b7-scenario.ts`, `Server/Play/Spots/scenario-actors.ts` | done | 같은 actor의 연속 `ActorPingReq` reply와 evidence에서 `entry-created` -> `entry-joined` -> packet 순서, `seen=1`, `seen=2` 직렬 처리 검증. PASS: `logs/20260630-070802-3055574` |
| SM-B8 | `Client/Scenarios/SmB8Scenario.cs` | `Client/Scenarios/sm-b8-scenario.ts`, `Server/Play/Spots/scenario-actors.ts` | done | public `entrySpot.context.destroyActor(...)` 후 `actor-destroyed` evidence와 post-destroy stream error reply를 검증. PASS: `logs/20260630-072210-3097534` |
| SM-B9 | `.NET feature-map SM-B9` | `Client/Scenarios/sm-b9-scenario.ts`, `Server/Play/Spots/scenario-actors.ts` | done | entry spot join admission에서 accepted/rejected actor 결과와 rejected actor stream error reply를 검증. PASS: `logs/20260707-195152-3345108` |
| SM-C1 | `Client/Scenarios/SmC1Scenario.cs` | `Client/Scenarios/sm-c1-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts`, `Server/Play/Handlers/state-req-handler.ts` | done | public request/send route-to-spot, slow request timeout, timeout 이후 정상 request를 검증. PASS: `logs/20260630-081924-3247169` |
| SM-C2 | `Client/Scenarios/SmC2Scenario.cs` | `Client/Scenarios/sm-c2-scenario.ts`, `Server/Play/Handlers/channel-handlers.ts`, `Server/Play/Handlers/spot-outbound-handlers.ts` | done | spot -> channel request/send, spot publish, missing channel request/send negative를 public API와 message-flow observer evidence로 검증. 선택 PASS: `logs/20260629-205741-1493698`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-C3 | `Client/Scenarios/SmC3Scenario.cs` | `Client/Scenarios/sm-c3-scenario.ts`, `Server/Play/Handlers/spot-to-spot-handlers.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | source/target spot 생성 뒤 spot-to-spot request/send/publish/timeout/negative 경로를 public outbound API로 검증. PASS: `logs/20260630-082100-3253716` |
| SM-C4 | `Client/Scenarios/SmC4Scenario.cs` | `Client/Scenarios/sm-c4-scenario.ts`, `Server/Gateway/`, `Server/Play/Endpoints/play-endpoints.ts` | done | Gateway role이 public `publishSpot(...)`로 `spot-publish` evidence를 남기고 Play-A subscribed spot event와 미구독 alternate spot 미수신을 검증. PASS: `logs/20260630-083734-3303700` |
| SM-C5 | `.NET feature-map SM-C5` | `Client/Scenarios/sm-c5-scenario.ts`, `Server/Play/Spots/scenario-spots.ts` | done | SpotMesh publish가 cross-node subscribed spot에 도달하고 미구독 spot에는 남지 않는지 검증. PASS: `logs/20260707-195152-3345108` |
| SM-D1 | `Client/Scenarios/SmD1Scenario.cs` | `Client/Scenarios/sm-d1-scenario.ts`, `Server/Session/Endpoints/session-endpoints.ts`, `Server/Play/Handlers/control-handlers.ts`, `Server/Play/Spots/scenario-actors.ts` | done | session-a stream에서 `play-a` local actor를 bind하고 `ActorPushReq` relay reply와 public bound session push `ActorPushNotify`를 검증. 선택 PASS: `logs/20260629-211928-1555127`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D2 | `Client/Scenarios/SmD2Scenario.cs` | `Client/Scenarios/sm-d2-scenario.ts`, `Server/Session/Endpoints/session-endpoints.ts`, `Server/Play/Handlers/control-handlers.ts`, `Server/Play/Spots/scenario-actors.ts` | done | session-a stream에서 `play-b` remote actor를 bind하고 `ActorPushReq` relay reply와 public bound session push `ActorPushNotify`를 검증. 선택 PASS: `logs/20260629-212246-1563843`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D3 | `Client/Scenarios/SmD3Scenario.cs` | `Client/Scenarios/sm-d3-scenario.ts`, `Server/Play/Spots/scenario-actors.ts`, `Server/Play/Spots/scenario-spots.ts` | done | entry actor bind/push와 user spot actor bind, `UserActorPingReq`, `UserActorPushReq`, user spot evidence를 검증. 선택 PASS: `logs/20260629-212739-1577626`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D4 | `Client/Scenarios/SmD4Scenario.cs` | `Client/Scenarios/sm-d4-scenario.ts`, `Server/Session/Handlers/scenario-session.ts`, `Shared/messages.ts` | done | `MultiBindReq`로 한 stream에 두 actor를 bind하고 metadata `actor-id` 선택 relay, actor별 push, id 없는 request 실패를 검증. 선택 PASS: `logs/20260629-213206-1588322`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D5 | `Client/Scenarios/SmD5Scenario.cs` | `Client/Scenarios/sm-d5-scenario.ts`, `Server/Session/Handlers/scenario-session.ts`, `Server/Session/Spots/scenario-actors.ts` | in-progress | application의 selected Actor notify loop를 제거했다. Framework automatic all-bound notification을 최신 focused runner로 재검증해야 한다. |
| SM-D6 | `Client/Scenarios/SmD6Scenario.cs` | `Client/Scenarios/sm-d6-scenario.ts`, `Server/Play/Spots/scenario-actors.ts` | done | target actor에 bind된 `session-a` stream consumer만 `ActorPushNotify`를 받고, `session-b`의 별도 consumer는 target actor push를 받지 않는지 검증. 선택 PASS: `logs/20260629-213945-1613927`; `all` PASS: `logs/20260702-064908-43303` |
| SM-D7 | `Client/Scenarios/SmD7Scenario.cs` | `Client/Scenarios/sm-d7-scenario.ts`, `Server/Session/Handlers/scenario-session.ts` | done | stream auth reply와 bound actor packet dispatch/reply를 검증. 선택 PASS: `logs/20260629-214310-1624231`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D8 | `Client/Scenarios/SmD8Scenario.cs` | `Client/Scenarios/sm-d8-scenario.ts`, `Server/Play/Spots/scenario-actors.ts`, `Shared/messages.ts` | done | slow actor pending request가 stream close 후 실패하고, 새 stream에서 재auth/rebind 후 messaging이 재개되는지 검증. 선택 PASS: `logs/20260629-214843-1639970`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D9 | `Client/Scenarios/SmD9Scenario.cs` | `Client/Scenarios/sm-d9-scenario.ts` | done | public stream connector `observeInbound(...)`가 response frame의 kind, request sequence, payload length를 관측하는지 검증. 선택 PASS: `logs/20260629-215409-1654253`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D10 | `Client/Scenarios/SmD10Scenario.cs` | `Client/Scenarios/sm-d10-scenario.ts` | 10.0.0 전환 대상 | 공통 connector 계약의 기존 메시지 유지·새 send 폐기를 payload identity로 단언해야 한다. 현재 drop 횟수와 총수만 확인해 어떤 메시지가 폐기됐는지 구분하지 못한다. |
| SM-D11 | `Client/Scenarios/SmD11Scenario.cs` | `Client/Scenarios/sm-d11-scenario.ts` | done | 같은 flow에서 stream actor request와 Session HTTP channel control request를 함께 보내 두 reply 경로가 분리되는지 검증. 선택 PASS: `logs/20260629-220146-1678135`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D12 | `Client/Scenarios/SmD12Scenario.cs` | `Client/Scenarios/sm-d12-scenario.ts`, `run_e2e.sh` | done | `session-a` stream에서 actor state를 만든 뒤 `session-b` stream으로 재auth/rebind하고 `SnapshotReq`/`ActorPushReq`로 state 보존과 새 stream push를 검증. 선택 PASS: `logs/20260629-220618-1691419`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D13 | `Client/Scenarios/SmD13Scenario.cs` | `Client/Scenarios/sm-d13-scenario.ts` | done | `.NET` 기준과 같이 public heartbeat option이 켜진 stream이 여러 heartbeat interval 동안 연결 상태를 유지하는지 검증. 선택 PASS: `logs/20260629-221012-1704641`; `all` PASS: `logs/20260630-074201-3148526` |
| SM-D14 | `Client/Scenarios/SmD14Scenario.cs` | `Client/Scenarios/sm-d14-scenario.ts`, `Server/Session/session-host-factory.ts`, `run_e2e.sh` | 10.0.0 전환 대상 | public `setTlsServer(...)`로 `wss://` endpoint는 열지만 browser의 인증서 검증 생략 환경 변수에 의존한다. 공개 신뢰 설정으로 정상 인증서 성공과 잘못된 인증서 거부를 검증해야 한다. |
| SM-D15 | `.NET feature-map SM-D15` | `Client/Scenarios/sm-d15-scenario.ts`, `Server/Session/`, `Server/Play/Spots/scenario-actors.ts` | done | cross-role actor request reply와 bound session push chain을 같은 actor/session evidence로 검증. 선택 PASS: `logs/20260707-195152-3345108`; `all` PASS: `logs/20260708-062031-351969` |
| SM-E1 | `Client/Scenarios/SmE1Scenario.cs` | `Client/Scenarios/sm-e1-scenario.ts`, `Server/Play/Handlers/dispatch-error-observer.ts` | done | missing handler request/command을 public spot outbound와 message-flow observer로 연결하고 SpotRoute `handlerMissing` evidence를 검증. PASS: `logs/20260630-082045-3252646` |
| SM-E2 | `Client/Scenarios/SmE2Scenario.cs` | `Client/Scenarios/sm-e2-scenario.ts`, `Server/Play/Handlers/timer-handlers.ts` | done | lifecycle timer tick repeats and close succeeds. PASS: `logs/20260630-074201-3148526` |
| SM-E3 | `Client/Scenarios/SmE3Scenario.cs` | `Client/Scenarios/sm-e3-scenario.ts`, `Server/Play/Handlers/timer-handlers.ts` | done | idle timer close, `spot-closing` evidence, close 이후 routed request 실패를 public timer/close/outbound 경로로 검증. PASS: `logs/20260630-081940-3248210` |
| SM-E4 | `Client/Scenarios/SmE4Scenario.cs` | `Client/Scenarios/sm-e4-scenario.ts`, `Server/Play/Handlers/timer-handlers.ts` | done | timer overrun policies. PASS: `logs/20260630-074201-3148526` |
| SM-F1 | `Client/Scenarios/SmF1Scenario.cs` | `Client/Scenarios/sm-f1-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | target spot 생성 뒤 route client 경로의 request/command를 검증. PASS: `logs/20260630-082100-3253769` |
| SM-F2 | `Client/Scenarios/SmF2Scenario.cs` | `Client/Scenarios/sm-f2-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | target spot request/command selectable scenario를 검증. PASS: `logs/20260630-082100-3253756` |
| SM-F3 | `.NET client scenario file 없음`, `feature-map.ko.md`의 `SM-F3` 행 | `Client/Scenarios/sm-f3-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts`, `Server/Play/play-host-factory.ts` | 10.0.0 전환 대상 | ChannelName·RID direct·Spot direct를 같은 MeshNode에서 섞어 제출하고 각 handler evidence가 정확히 한 번인지 10.0.0 표면으로 검증해야 한다. |
| SM-F4 | `Client/Scenarios/SmF4Scenario.cs` | `Client/Scenarios/sm-f4-scenario.ts` | done | 존재하지 않는 location의 request 실패를 검증했다. malformed relay packet 주입은 public route-client 표면이 아니므로 runtime 내부 검증이나 별도 bridge-level 테스트 대상으로 분리한다. `all` PASS: `logs/20260713-063253-3989258` |
| SM-F5 | `.NET client scenario file 없음`, `feature-map.ko.md`의 `SM-F5` 행 | `Client/Scenarios/sm-f5-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts`, `Server/Play/play-host-factory.ts` | 10.0.0 전환 대상 | Spot close 뒤 해당 Spot direct 경로만 실패하고 MeshNode peer와 ChannelName handler가 계속 ready인지 10.0.0 표면으로 검증해야 한다. |
| SM-F6 | `.NET feature-map SM-F6` | `Client/Scenarios/sm-f6-scenario.ts`, `Server/MultiNode/`, `run_e2e.sh` | done | RouteMesh 없이 SpotMesh만 구성한 MultiNode role에서 target spot request/reply와 owner evidence를 검증. 선택 PASS: `logs/20260707-195217-3346296`; `all` PASS: `logs/20260708-062057-353085` |
| SM-G1 | `Client/Scenarios/SmG1Scenario.cs` | `Client/Scenarios/sm-g1-scenario.ts`, `Server/Play/Endpoints/play-endpoints.ts` | done | `play-a`/`play-b` actor bind 후 `play-a` `/crash` endpoint로 프로세스를 종료하고, `play-a` actor request 실패, `play-b` actor 생존, `session-b`에서 `play-b`로 재auth/rebind 복구를 검증. 선택 PASS: `logs/20260629-223922-1778101`; `all` PASS: `logs/20260708-062230-357711` |
| SM-G2 | `Client/Scenarios/SmG2Scenario.cs` | `Client/Scenarios/sm-g2-scenario.ts`, `Server/MultiNode/Endpoints/multi-node-endpoints.ts`, `run_e2e.sh` | done | node A에 기존 Spot·actor를 준비한 뒤 node B를 추가한다. B peer actor capability와 Entry Spot handle readiness 후 기존 owner A 유지, B 로컬 Spot 생성과 B Entry Spot actor 생성을 검증. 선택 PASS: `log/20260716-110658-3617643` |
| SM-G3 | `Client/Scenarios/SmG3Scenario.cs` | `Client/Scenarios/sm-g3-scenario.ts`, `Server/Session/Handlers/scenario-session.ts`, `Server/Play/Spots/scenario-spots.ts` | done | 같은 user spot에 두 stream client를 연결해 `UserSpotAuthReq`, concurrent `UserActorPingReq`, `LeaveReq`를 실행하고 actor별 join/leave evidence가 1회씩 남는지 검증. 선택 PASS: `logs/20260629-224535-1792721`; `all` PASS: `logs/20260708-062135-354732` |
| SM-G4 | `Client/Scenarios/SmG4Scenario.cs` | `Client/Scenarios/sm-g4-scenario.ts` | done | 여섯 stream client를 각각 다른 actor에 bind한 뒤 `.NET` 기준처럼 순차적으로 `ActorPushReq`를 보내 reply와 `ActorPushNotify`가 각 actor/session으로만 돌아오는지 검증. 선택 PASS: `logs/20260629-225216-1811400`; `all` PASS: `logs/20260708-062201-355741` |

## Supplemental .NET Operation

| Operation | .NET 기준 파일 | Node.js 대상 파일 | 상태 | 비고 |
|-----------|----------------|-------------------|------|------|
| sm-q9 | `Client/Scenarios/SmQ9Scenario.cs` | `Client/Scenarios/sm-q9-scenario.ts`, `Server/MultiNode/` | done | MultiNode role, local spot 생성, public `ZLinkSpotOutbound.requestToSpot(...)` 기반 선택 operation을 검증. 선택 PASS: `logs/20260630-082118-3256244`; `all` PASS: `logs/20260708-062258-359849` |

## File Mapping

| .NET 기준 영역 | Node.js 대상 영역 | 분류 | 상태 | 비고 |
|----------------|-------------------|------|------|------|
| `.gitignore` | `.gitignore`, `logs/.gitignore` | ignore | done | dist, node_modules, 실행 로그 제외 |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | scenario 범위, 구현 상태, runner proof 기록 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | Play-A/Play-B/Session-A/Session-B/Gateway/MultiNode build/start/readiness/cleanup/client 실행 구현. `all`은 공통 Config 2에 정의된 `default-batch`, `SM-F6`, `SM-G2`, `SM-G3`, `SM-G4`, `SM-G1` child scenario를 실행한다. SM-Q9는 보조 operation으로만 선택 실행한다. `default-batch`는 `.NET` operation group 순서대로 client process를 나누어 실행하고 SM-F4를 포함한다. PASS: `logs/20260708-062031-351962`; `default-batch` PASS: `logs/20260708-062031-351969`. Node fetch가 차단하는 unsafe port와 같은 run 안의 중복 port를 피하도록 port picker를 필터링함 |
| `Shared/Messages.cs`, `Shared/SpotService.Shared.csproj` | `Shared/messages.ts` | shared | done | 구현 scenario가 쓰는 create, state, stage, actor, stream, timer, route, MultiNode message 계약을 포함한다. |
| `Client/Program.cs`, `Client/SpotService.Client.csproj` | `Client/main.ts`, `Client/package.json`, `Client/tsconfig.json` | client-entry/project | done | 구현 scenario 선택과 실행 앱을 제공한다. |
| `Client/Support/*` | `Client/Support/` | support | done | option parsing, assertion, HTTP helper, stream connector helper, process helper를 구현했다. |
| `Client/Scenarios/*.cs` | `Client/Scenarios/` | scenario | done | 공통 scenario와 `.NET` 기준 scenario를 작성했다. |
| `Server/Registry/*` | 없음 | server-role | not-needed | SpotService runner는 registry role 없이 Redis location store와 explicit endpoint wiring을 사용한다. |
| `Server/Play/*` | `Server/Play/` | play-role | done | scenario에 필요한 spot, actor, channel, timer, message-flow observer, crash endpoint를 구현했다. |
| `Server/Session/*` | `Server/Session/` | session-role | done | stream session gateway, auth dispatch, local/remote actor bind, actor relay, multi-bind와 metadata actor selection relay, stream close disconnect callback, TLS server 구성을 구현했다. |
| `Server/Gateway/*` | `Server/Gateway/` | gateway-role | done | publish-only Gateway role, HTTP `/spot/publish`, public `ZLinkSpotPublisherClient.publishSpot(...)` 호출 구현. Play-A가 Gateway pub endpoint에 `connectPeerPub(...)`로 붙는 `.NET` 기준 topology에서 subscribed spot event delivery를 검증. PASS: `logs/20260630-083734-3303700` |
| `Server/MultiNode/*` | `Server/MultiNode/` | multinode-role | done | `.NET` MultiNode role 형태로 route mesh, spot mesh, local create endpoint, route-to-spot endpoint를 추가했고 sm-q9 선택 operation으로 검증했다. PASS: `logs/20260630-082118-3256244` |

## Public Contract 확인 결과

- Node framework의 spot/actor/session public API가 `.NET`의 SM-A~SM-G scenario를 같은 수준으로 노출하는지
  확인했다.
- SM-A2/SM-A3/SM-A4/SM-A5/SM-C1/SM-C3/SM-E1/SM-E3/SM-F1/SM-F2/SM-G2와 sm-q9는 public custom resolver와
  `ZLinkSpotOutbound.requestToSpot(...)`/`sendToSpot(...)` 경로가 same-node route bridge owner를 local
  dispatch로 처리하도록 보강한 뒤 선택 scenario로 통과했다.
- SM-B1/SM-B2는 Session stream auth, local/remote actor bind, actor packet relay와 Play actor creation
  경로의 `entry-created` lifecycle evidence를 검증해 완료로 올렸다.
- SM-B6은 explicit leave와 stream close disconnect callback evidence를 구분해 검증했고 default `all`
  gate에 포함했다.
- SM-D5는 stream close 후 Session `onDisconnected`와 선택 actor `entry-disconnected` evidence를 검증했고
  default `all` gate에 포함했다.
- SM-D14는 public stream node builder `setTlsServer(...)`로 server certificate/key를 stream socket bind
  전에 적용한다. 현재 성공 경로는 `ZLINK_BROWSER_IGNORE_HTTPS_ERRORS`에 의존하므로 공개 connector
  신뢰 설정과 잘못된 인증서 거부를 갖추기 전까지 10.0.0 전환 대상으로 유지한다.
- sm-q9는 MultiNode role과 location store 기반 spot resolver 설정을 public `ZLinkSpotOutbound.requestToSpot(...)`
  경로로 검증했다. PASS: `logs/20260630-082118-3256244`.
- SM-B7은 같은 actor의 연속 packet dispatch 순서가 `entry-created` -> `entry-joined` -> packet,
  `seen=1`, `seen=2`로 보존됨을 검증해 완료로 올렸다.
- SM-B8은 Entry Spot actor handler에서 public `destroyActor(...)`를 호출한 뒤 post-destroy request가
  stream error reply로 실패하고 `actor-destroyed` evidence가 남는지 검증했다.
- SM-C1은 public `sendToSpot(...)`와 `requestToSpot(...)`로 command, request, slow timeout, timeout 이후
  정상 request를 검증했다.
- SM-C3은 spot-to-spot handlers와 selectable scenario를 public outbound API로 검증했다.
- SM-C4는 local spot factory가 없는 Gateway role에서 public spot publisher client로 publish한다.
  Play-A가 Gateway pub endpoint에 붙는 public `connectPeerPub(...)` topology에서 Gateway publish marker와
  Play subscribed spot event marker가 같은 run에 남는지 검증했다.
- internal helper, raw frame, 테스트 전용 adapter로 scenario를 우회하지 않는다.

## 후속 계약 판정

| 묶음 | Scenario | 판정 | 다음 작업 |
|------|----------|------|-----------|
| routed spot request | `SM-A2`, `SM-A3`, `SM-A4`, `SM-A5`, `SM-C1`, `SM-C3`, `SM-E1`, `SM-E3`, `SM-G2`, `sm-q9` | 구현 | Spot context outbound와 resolver 기반 route는 기존 public contract로 검증했다. 선택 PASS 로그는 각 scenario 행에 남겼다. |
| route client target spot | `SM-F1`, `SM-F2`, `SM-F3`, `SM-F4`, `SM-F5` | 혼합 | `SM-F1`·`SM-F2`·`SM-F4`는 기존 증거가 있다. `SM-F3`과 `SM-F5`는 10.0.0 MeshNode·ChannelName·Spot direct 표면으로 다시 검증해야 한다. |
| stream TLS server | `SM-D14` | 구현 | stream server TLS certificate/key 설정을 Node public API로 받아 runner가 self-signed TLS endpoint를 열고 reject/accept 경로를 검증했다. PASS: `logs/20260630-085904-3356699`; `all` PASS: `logs/20260630-101424-3467655`. |
