# Java SpotService .NET 포팅 inventory

기준 문서:

- `framework/doc/framework/common/e2e/config-2-spot-service.ko.md`
- `framework/languages/dotnet/e2e/SpotService/feature-map.ko.md`
- `framework/languages/dotnet/e2e/SpotService/`

## 요약

기존 Java SpotService E2E는 단일 Gradle application에서 `ZLINK_JAVA_E2E_ROLE`로 `registry`, `play`,
`publisher`, `client` 역할을 전환했다. 현재 구조는 기존 구현을 보존하면서 `Shared`, `Client`,
`Server/Gateway`, `Server/Play`, `Server/MultiNode`, `Server/Session`, `Server/Publisher` Gradle
subproject로 분리했다. embedded registry role은 제거했고, 실행 role은 공식 Redis location store
extension을 같은 endpoint와 실행별 key prefix로 공유한다.

`.NET` 기준의 `Gateway`, `MultiNode`, `Session` source role은 Java role project로 존재한다. Client는
HTTP driver이고, 기존 spot/route/stream scenario 실행 책임은 `Server/Gateway`로 옮겼다. Java
feature-map은 현재 구현된 public API 경로와 public contract/harness gap을 구분한다. gap은 테스트 전용
adapter, raw frame 우회, 새 public API 추가로 메우지 않는다.

각 `.NET` `Client/Scenarios/*.cs` 파일에는 같은 이름의 Java
`Client/src/main/java/systems/zlink/e2e/spotservice/client/Scenarios/*.java` 파일이 있다. 아래 표의
scenario 행은 같은 이름의 Java classification file과 실제 실행 body(`Server/Gateway` 또는 shared
scenario executor)를 함께 의미한다.

## Inventory

| .NET 기준 파일 | Java 대응 파일 | 분류 | 상태 | 비고 |
|----------------|----------------|------|------|------|
| `.gitignore` | `.gitignore` | config | done | multi-project build, `.gradle`, logs 산출물 제외 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | registry role 없이 실행별 Redis location store와 role별 installDist binary를 실행하고 Java feature-map의 완료 marker를 검증한다. |
| `feature-map.ko.md` | `feature-map.ko.md` | docs | done | Java 완료/gap scenario 구분 |
| `Shared/SpotService.Shared.csproj` | `Shared/build.gradle.kts` | build | done | shared Java library project |
| `Shared/Messages.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/Contracts.java` | shared | done | request, reply, evidence, stream payload record |
| `Client/SpotService.Client.csproj` | `Client/build.gradle.kts` | build | done | client application project |
| `Client/Program.cs` | `Client/src/main/java/systems/zlink/e2e/spotservice/client/Program.java` | client | done | HTTP driver entrypoint. `Server/Gateway`의 `/scenario/<mode>`를 호출한다. |
| `Client/Scenarios/SmA1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | `SM-A1` marker |
| `Client/Scenarios/SmA2Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | `SM-A2` marker |
| `Client/Scenarios/SmA3Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | `SM-A3` marker |
| `Client/Scenarios/SmA4Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | `SM-A4` marker |
| `Client/Scenarios/SmA5Scenario.cs` | `Client/Scenarios/SmA5Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioStage.java`, `StageProbeReqHandler.java`, `StageTimerStartReqHandler.java`, `StageTimerHandler.java`, `ClientScenario.java` | scenario | done | app-level `ScenarioStage` wrapper가 public spot request handler에서 호출되고, public RouteMesh request로 stage request와 timer start를 검증한다. |
| `Client/Scenarios/SmA6Scenario.cs` | `run_e2e.sh`, `Server/Play/src/main/java/systems/zlink/e2e/spotservice/play/Program.java` | scenario | done | explicit close lifecycle marker |
| `Client/Scenarios/SmA7Scenario.cs` | `run_e2e.sh`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/MismatchedSpot.java` | scenario | done | spot type mismatch marker |
| `Client/Scenarios/SmA8Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/UserSpot.java`, `ClientScenario.java` | scenario | done | worker offload marker |
| `Client/Scenarios/SmB1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioSession.java`, `ScenarioActor.java`, `ClientScenario.java` | scenario | done | local actor join marker |
| `Client/Scenarios/SmB2Scenario.cs` | `Client/Scenarios/SmB2Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | remote actor join, cross-node mailbox 실행, bound session push evidence |
| `Client/Scenarios/SmB3Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioActor.java`, `ClientScenario.java` | scenario | done | payload fidelity marker |
| `Client/Scenarios/SmB4Scenario.cs` | `Client/Scenarios/SmB4Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | remote actor request/reply and route-bridge reply evidence |
| `Client/Scenarios/SmB5Scenario.cs` | `Client/Scenarios/SmB5Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | missing actor packet request failure and `DispatchError` HandlerMissing evidence |
| `Client/Scenarios/SmB6Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `UserActorLeaveHandler.java`, `UserSpot.java` | scenario | done | actor leave/disconnect callback 차이 marker |
| `Client/Scenarios/SmB7Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioActor.java`, `ClientScenario.java` | scenario | done | actor lifecycle/order marker |
| `Client/Scenarios/SmB8Scenario.cs` | `Client/Scenarios/SmB8Scenario.java` | scenario | done | Java public `ZLinkEntrySpotContext.destroyActor(actor)` 흐름으로 entry actor 명시 destroy, 후속 packet 실패, destroy evidence 1회 발생을 검증한다. |
| `Client/Scenarios/SmB9Scenario.cs` | `Client/Scenarios/SmB9Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/EntryActorJoinAdmissionHandler.java`, `UserSpot.java`, `ClientScenario.java`, `run_e2e.sh` | scenario | done | entry spot actor join admission 허용/거부를 local/remote user spot 대상으로 검증하고, 거부 actor가 user spot에 join되지 않는지 확인한다. focused PASS: `logs/20260707-234158-4020740` |
| `Client/Scenarios/SmC1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | external consumer to spot messaging |
| `Client/Scenarios/SmC2Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/UserSpot.java`, `ClientScenario.java` | scenario | done | spot to channel and publish |
| `Client/Scenarios/SmC3Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/UserSpot.java`, `ClientScenario.java` | scenario | done | spot to spot and publish |
| `Client/Scenarios/SmC4Scenario.cs` | `Server/Publisher/src/main/java/systems/zlink/e2e/spotservice/publisher/Program.java` | scenario | done | publisher client marker |
| `Client/Scenarios/SmD1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioSession.java`, `ClientScenario.java` | scenario | done | local actor session bind/relay |
| `Client/Scenarios/SmD2Scenario.cs` | `Client/Scenarios/SmD2Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | remote stream session bind, cross-node actor relay, bound session push evidence |
| `Client/Scenarios/SmD3Scenario.cs` | `Client/Scenarios/SmD3Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | entry spot bind와 user spot bind의 actor id, spot rid, push target 비교 |
| `Client/Scenarios/SmD4Scenario.cs` | `Client/Scenarios/SmD4Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/MultiBindHandler.java` | scenario | done | 한 stream session의 다중 actor bind, metadata 기반 request/push 분기, metadata 누락 실패 검증 |
| `Client/Scenarios/SmD5Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `UserSpot.java` | scenario | done | session disconnect 뒤 bound actor disconnect callback marker |
| `Client/Scenarios/SmD6Scenario.cs` | `Client/Scenarios/SmD6Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | bound session actor push가 shadow session에 전달되지 않는지 검증 |
| `Client/Scenarios/SmD7Scenario.cs` | `Client/Scenarios/SmD7Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | auth 전 actor packet 실패와 auth 뒤 단일 bound actor request/push 검증 |
| `Client/Scenarios/SmD8Scenario.cs` | `Client/Scenarios/SmD8Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/SlowSessionHandler.java` | scenario | done | stream disconnect pending failure와 같은 actor id 재auth/rebind 검증 |
| `Client/Scenarios/SmD9Scenario.cs` | `Client/Scenarios/SmD9Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioSession.java` | scenario | done | `StreamInbound` evidence marker |
| `Client/Scenarios/SmD10Scenario.cs` | `Client/Scenarios/SmD10Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | public stream connector의 bounded received-message queue와 session별 push 격리 검증 |
| `Client/Scenarios/SmD11Scenario.cs` | `Client/Scenarios/SmD11Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | 같은 client driver에서 stream actor request와 route-channel request를 함께 검증 |
| `Client/Scenarios/SmD12Scenario.cs` | `Client/Scenarios/SmD12Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ActorAuthHandler.java`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkAutoConnectPlanner.java`, `feature-map.ko.md` | scenario | done | stream server 재접속 뒤 기존 actor reauth/rebind, `SnapshotReq`, `ActorPushReq`, 새 stream push target을 검증한다. focused PASS: `logs/20260707-214123-3491316` |
| `Client/Scenarios/SmD13Scenario.cs` | `Client/Scenarios/SmD13Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | heartbeat가 켜진 stream 유지 뒤 후속 actor request와 inbound evidence 검증 |
| `Client/Scenarios/SmD14Scenario.cs` | `Client/Scenarios/SmD14Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Server/Play/src/main/java/systems/zlink/e2e/spotservice/play/Program.java`, `run_e2e.sh` | scenario | done | public stream node TLS server 설정, strict certificate validation failure, skip-validation auth/request/push 검증 |
| `Client/Scenarios/SmD15Scenario.cs` | `Client/Scenarios/SmD15Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientDriverSpot.java`, `run_e2e.sh` | scenario | done | gateway role의 public `ZLinkActorClient.requestToActor`가 entry actor handler에 도달하고 bound stream session push notify가 client connector에 도착하는지 검증한다. focused PASS: `logs/20260707-234915-4047473` |
| `Client/Scenarios/SmE1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Server/Play/src/main/java/systems/zlink/e2e/spotservice/play/Program.java` | scenario | done | missing route dispatch observer evidence |
| `Client/Scenarios/SmE2Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/TimerScenarioSpot.java` | scenario | done | timer tick marker |
| `Client/Scenarios/SmE3Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/IdleCloseTimerHandler.java` | scenario | done | idle timer close marker |
| `Client/Scenarios/SmE4Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/TimerOverrunHandler.java` | scenario | done | timer overrun policy marker |
| `Client/Scenarios/SmF1Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | route mesh target spot |
| `Client/Scenarios/SmF2Scenario.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | route mesh channel name |
| `Client/Scenarios/SmF4Scenario.cs` | `Client/Scenarios/SmF4Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | missing target spot route request 실패 marker. malformed relay packet 주입은 public E2E 표면이 아니므로 직접 scenario로 만들지 않는다. |
| `Client/Scenarios/SmF5Scenario.cs` | `Client/Scenarios/SmF5Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | target spot request 후 public spot close를 실행하고 같은 RouteMesh channel 일반 request가 계속 성공하는지 검증 |
| `Client/Scenarios/SmF6Scenario.cs` | `Client/Scenarios/SmF6Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/MultiNodeSpot.java`, `Server/MultiNode/src/main/java/systems/zlink/e2e/spotservice/multinode/MultiNodeHttpServer.java`, `Server/MultiNode/src/main/java/systems/zlink/e2e/spotservice/multinode/Program.java`, `run_e2e.sh` | scenario | done | RouteMesh 없이 SpotMesh만 등록한 multi-node role에서 source spot의 public spot outbound request/send와 actor client join이 remote target spot에 도달하는지 검증한다. focused PASS: `logs/20260707-235923-4093006` |
| `Client/Scenarios/SmG1Scenario.cs` | `Client/Scenarios/SmG1Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `run_e2e.sh` | scenario | done | runner가 시작한 play-a만 SIGKILL하고 owner lease TTL 뒤 재시작해 bounded failure, play-b 생존, 재auth/rejoin/rebind 복구를 검증 |
| `Client/Scenarios/SmG2Scenario.cs` | `Client/Scenarios/SmG2Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/EvidenceHttpServer.java`, `run_e2e.sh` | scenario | done | 앱 주도 owner remap을 play-a/play-b HTTP 제어와 public RouteMesh request로 검증 |
| `Client/Scenarios/SmG3Scenario.cs` | `Client/Scenarios/SmG3Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRuntime.java` | scenario | done | join/leave/request 경합에서 actor별 joined/left evidence 1회와 늦게 도착한 entry spot lifecycle event 무시를 검증 |
| `Client/Scenarios/SmG4Scenario.cs` | `Client/Scenarios/SmG4Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | scenario | done | 여러 bound stream session의 동시 push가 자기 session으로만 돌아오는지 검증 |
| `Client/Scenarios/SmQ9Scenario.cs` | `Client/Scenarios/SmQ9Scenario.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java`, `Server/MultiNode/src/main/java/systems/zlink/e2e/spotservice/multinode/Program.java`, `run_e2e.sh` | scenario | done | multi-node role HTTP endpoint에서 local spot 생성 후 public RouteMesh client로 target spot request 검증 |
| `Client/Support/ClientOptions.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/Env.java` | support | done | 환경 변수 option helper |
| `Client/Support/ScenarioAssert.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ClientScenario.java` | support | done | Gateway scenario executor의 assertion helper |
| `Client/Support/SpotLifecycleOrderContext.cs` | `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/ScenarioState.java` | support | done | evidence order 검증 입력 |
| `Server/Registry/*` | 제거됨 | server-role | not-needed | embedded registry role은 Redis location store 전환으로 삭제 |
| `Server/Play/*` | `Server/Play/src/main/java/systems/zlink/e2e/spotservice/play/Program.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/*.java`, `Server/Play/build.gradle.kts` | server-role | done | play role 구현과 support/handler/spot 타입. Redis location store 등록 |
| `Server/Gateway/*` | `Server/Gateway/build.gradle.kts`, `Server/Gateway/src/main/java/systems/zlink/e2e/spotservice/gateway/Program.java`, `Shared/src/main/java/systems/zlink/e2e/spotservice/shared/GatewayScenarioHttpServer.java` | server-role | done | HTTP scenario endpoint와 framework gateway process. Redis location store 등록 |
| `Server/MultiNode/*` | `Server/MultiNode/build.gradle.kts`, `Server/MultiNode/src/main/java/systems/zlink/e2e/spotservice/multinode/Program.java`, `Server/MultiNode/src/main/java/systems/zlink/e2e/spotservice/multinode/MultiNodeHttpServer.java` | server-role | done | SM-Q9용 multi-node spot/route role. Redis location store, spot mesh, route mesh, HTTP evidence endpoint를 제공한다. |
| `Server/Session/*` | `Server/Session/build.gradle.kts`, `Server/Session/src/main/java/systems/zlink/e2e/spotservice/session/Program.java` | server-role | done | `.NET` source role project exists. SM-D12에서 다른 stream server reauth/rebind와 새 stream push target을 검증한다. |
| `Server/Publisher/*` | `Server/Publisher/src/main/java/systems/zlink/e2e/spotservice/publisher/Program.java`, `Server/Publisher/build.gradle.kts` | server-role | done | Java publisher role. Redis location store 등록 |

## 검증

- `../../gradlew --project-cache-dir /tmp/zlink-spotservice-gradle-cache --no-daemon --no-parallel --max-workers=1 :Client:installDist :Server:Play:installDist :Server:Gateway:installDist :Server:Publisher:installDist --console=plain`
- `timeout 420s ./run_e2e.sh SM-A1` 통과: `logs/20260704-034626-63193`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-F4` 통과: `logs/20260707-155507-2533878`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D9` 통과: `logs/20260707-161120-2576918`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D11` 통과: `logs/20260707-161530-2594865`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D13` 통과: `logs/20260707-162215-2616391`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D3` 통과: `logs/20260707-162500-2625106`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D4` 통과: `logs/20260707-163149-2645642`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D6` 통과: `logs/20260707-163553-2658389`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D7` 통과: `logs/20260707-163948-2676016`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D8` 통과: `logs/20260707-164242-2684493`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D10` 통과: `logs/20260707-164924-2702055`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D14` 통과: `logs/20260707-173359-2868329`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-F5` 통과: `logs/20260707-172810-2852279`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-G1` 통과: `logs/20260707-183605-3084865`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-G2` 통과: `logs/20260707-175627-2934857`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-G3` 통과: `logs/20260707-182527-3039973`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-Q9` 통과: `logs/20260707-175108-2916618`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-G4` 통과: `logs/20260707-165524-2728961`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-A5` 통과: `logs/20260707-190901-3217538`
- 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D12` 실패: `logs/20260707-192117-3267565`.
  두 번째 auth의 ownership conflict는 사라졌지만 후속 actor request 중 `play-a`가 종료되어 gateway가
  `Connection refused`를 받았다. `logs/20260707-192427-3281428` trace 재시도는 gateway bind 실패로
  scenario 본문에 들어가지 못했으므로 SM-D12 완료 증거로 사용하지 않는다.
- SM-D12를 .NET/Node와 같은 흐름으로 맞춘 뒤 자체 Redis endpoint로 `timeout 420s ./run_e2e.sh SM-D12`
  재시도: `logs/20260707-192937-3300875`. 첫 stream auth와 `ActorEchoReq`, 두 번째 stream auth,
  `SnapshotReq`, `ActorPushReq` 흐름으로 정리했고 compile은 통과했다. focused runner는 두 번째
  단계에서 `session relay route was not ready before timeout`으로 실패했으므로 완료 증거로 사용하지
  않는다.
- Java runtime의 managed rebind relay 대기와 session dispatch context 전달을 보강한 뒤 자체 Redis
  endpoint로 `timeout 300s ./run_e2e.sh SM-D12` 재시도: `logs/20260707-194031-3328975`. 실패
  증상은 여전히 두 번째 단계의 `session relay route was not ready before timeout`이며, runner가
  retry하면서 CPU를 계속 사용해 해당 run은 중단했다. 이 로그도 완료 증거로 사용하지 않는다.
- SM-D12를 `.NET`/Node와 같은 `ActorPingReq`/`SnapshotReq`/`ActorPushReq` 흐름으로 맞추고,
  Java native session relay가 mesh peer를 양방향으로 연결하도록 조정한 뒤 자체 Redis endpoint로
  `timeout 360s ./run_e2e.sh SM-D12` 통과: `logs/20260707-214123-3491316`.
- 실행별 Redis location store를 기본으로 준비하도록 runner를 보강한 뒤
  `nice -n 10 timeout 1200s ./run_e2e.sh all` 통과:
  `logs/20260707-223144-3695956`, `spot-service e2e result=passed`.
- `nice -n 10 timeout 420s ./run_e2e.sh SM-D15` 통과:
  `logs/20260707-234915-4047473`, `spot-service e2e mode=actor-push-chain result=passed`.
- `nice -n 10 timeout 420s ./run_e2e.sh SM-F6` 통과:
  `logs/20260707-235923-4093006`, `spot-service e2e mode=spot-only-mesh result=passed`.
- `timeout 900s ./run_e2e.sh` 통과: `logs/20260704-034655-64390`, `spot-service e2e result=passed`

## 남은 gap

남은 gap은 없다. 이후 새 public contract 후보가 나오면 `feature-map.ko.md`의 대기 구역에
이유와 필요한 spec/guide 검토 항목을 먼저 적는다.
- Java public contract 기반 E2E 미구현: 없음.
- E2E/harness 대기: 없음.
