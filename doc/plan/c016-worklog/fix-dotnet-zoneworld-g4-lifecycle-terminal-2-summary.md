# .NET ZoneWorld G4 D-093 구현 결과

D-093의 승인된 B 범위에서 lifecycle-terminal과 admission deadline 소유권을 구현했다.
G4는 두 번 모두 `Unavailable`을 관찰했고 aggregate sample 실행에서도 같은 결과를 확인했다. 보완된 focused 테스트는 26/0이다. 전체 검증에는 아래 세 gate 실패가 남았다. Core·binding·다른 언어·보호 문서를 수정하지 않았으며 commit은 없다.

- 소유 계층: Framework mesh/Location이 connection intent 제거를 결정하고, 기존 pending durable operation이 terminal과 replay 중단을 소유한다. Core/binding은 physical disconnect와 typed completion을 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:324`의 owner 종료 → current operation `Unavailable`, `04-actor-model.ko.md:667`의 sender replay·remaining deadline·exhaustion, 승인 결정 D-093 규칙 1·2.
- 교차언어 대조: D-093 진단 당시 Java sender에는 logical lifecycle terminal이 없었고 Node ActorJoin은 timeout 뒤 peer 상태로 오류를 바꿨다. 공유 workspace의 별도 Java 작업은 현재 lifecycle 입력과 sample 오류 구분을 구현 중이다. Node의 durable replay/즉시 terminal parity는 후속 검증이 필요하다.
- 변경 분류: **B — 기존 결함, D-093 승인**.

수정 전/후 규칙 수: **admission deadline 소유자 3→1**, **lifecycle-terminal 규칙 0→1**.
Lifecycle generation·monitor 상태를 복제하지 않고 기존 intent 제거 전이에 현재 operation의 completion을 연결한다.

## 규칙별 변경

경로 prefix는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/`이다.

| 규칙 | 파일:행 | 구현 |
|---|---|---|
| 1 | `Service/ZLinkManagedMeshNode.cs:426`, `:438`, `:458`, `:483` | 기존 expectation/connection intent 제거 API에서 target의 expectation과 admitted peer가 없는 전이를 전달한다. Expectation 제거는 반복 호출에서도 조건을 평가하며 explicit `DisconnectPeer`도 제거 뒤 같은 조건을 평가한다. Physical `RemovePeer(disconnect:false)`는 이 전이를 전달하지 않는다. |
| 1 | `Service/ZLinkManagedMeshNode.cs:4292`, `:9220` | Durable submit 전에 전이를 구독하고 해당 target의 기존 pending operation을 `NotConnected` completion으로 끝낸다. Framework mapper는 이를 `Unavailable`로 전달한다. Pending token이 native wait와 replay를 중단하고, terminal 이후 구독을 해제한다. |
| 2 | `Actors/ZLinkDeferredActorJoin.cs:228`, `:268`, `:367` | Admission에 별도 deadline token을 전달하지 않는다. Predecessor 대기에는 등록 시점부터 계산한 remaining timeout과 외부 cancellation을 사용한다. Sender의 typed terminal kind를 completion에 유지한다. |
| 2 | `Host/ZLinkActorRemoteJoiner.cs:96`, `:165`, `:229`, `:463`, `:611`, `:1575` | 전체 transaction CTS를 제거하고 route/authority I/O 및 admission 이후 relocation에서만 기존 deadline helper를 사용한다. Admission은 남은 deadline을 sender에 전달하고 terminal을 기다린다. Native local join도 기본 timeout 대신 같은 remaining timeout을 전달한다. |
| 2 | `Host/ZLinkFrameworkActorFacade.cs:83`, `:157`, `:183` | Entry target의 Location 조회에 같은 absolute deadline을 적용한다. 명시된 deadline이 있는 local join만 기존 deadline helper를 사용하며, 명시되지 않은 local callback은 기존 caller cancellation을 유지한다. |

Peer 부재를 sender에서 조회하는 대안은 초기 route 부재와 transient disconnect를 lifecycle 종료로
오판한다. 기존 logical intent 제거 전이를 연결하면 lifecycle 판정은 기존 mesh/Location 안에 남는다.
Deadline을 admission 대기 전체에 유지하는 대안은 sender terminal과 취소가 경쟁하므로, I/O가
수행되는 범위에만 같은 absolute deadline을 적용한다. Deadline·retry budget은 늘리지 않았다.

Admission 전 route/authority I/O는 기존 `ExecuteWithDeadlineAsync`로 제한한다. Admission 이후에는
reservation을 기록한 뒤 authority 재확인·relocation·cutover를 같은 helper로 제한한다. 이미 받은
`Rejected`는 그대로 반환한다. Source cleanup은 기존 absolute deadline의 남은 시간과 shutdown
토큰을 사용한다. Predecessor 대기의 timeout은 admission token으로 전파하지 않는다.

## 회귀 테스트

`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/` 기준이다.

- `DurableSenderRuntimeTests.cs:14`: admitted request의 reply를 보류하고 target의 outbound connection을 끊는다. Source의 물리 peer 제거만으로는 완료되지 않는다. Intent 제거 뒤에는 1초 이내 `NotConnected`로 완료하고 재연결해도 추가 replay가 없다. Intent를 유지하면 재연결 뒤 retained terminal을 받고 target은 한 번만 실행한다. Expectation을 peer보다 먼저 제거하는 경우도 initiating disconnect와 non-initiating owner 재평가 양쪽에서 검사한다.
- `EntrySpotActorDispatchTests.cs:7108`: admission deadline 시점 이후 native terminal 전달을 재개한다. Deferred join이 먼저 취소되지 않고 sender가 전달한 `Unavailable`·`DeadlineExceeded`·`Rejected`를 보고한다. `:7151`은 local deferred admission의 captured deadline, `:7177`은 deadline이 명시되지 않은 local callback의 기존 caller cancellation을 검사한다.
- 기존 `DurableRequestTests`와 `DurableSenderPreservesExhaustionCauseAndOriginalOperation`의 exhaustion assertion은 유지한다.

## 검증 결과

Package SHA-256: `a46acff62126d5ed4cad8f4bfc0fe14ea246587d4499d34cb415bd0b1d513817`.
Nupkg의 Linux x64 native와 `core/build-dev/lib/libzlink.so` SHA-256은 모두
`1c7887c36dd2f1fe133fe3a39ccbfeb9ea42d4b051b302e3ecacf160e31b116d`다.

환경은 `/tmp/zlink-d093-env.sh`에 보존했다. `TMPDIR=/dev/shm/zlink-tmp-dotnet`,
`ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
`NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-a46acff62126d5ed`,
`UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`,
`DOTNET_CLI_TELEMETRY_OPTOUT=1`이다. 모든 유효한 .NET 검증은
`flock -w7200 /tmp/zlink-dotnet-gate.lock`, sample은 그 바깥의
`flock -w7200 /tmp/zlink-samples-gate.lock`도 획득한 뒤 실행한다.

| 검증 | 결과 | 증거 |
|---|---|---|
| Durable focused + 제거 순서·deferred terminal·local deadline | 25 passed / 0 failed / 0 skipped | `/tmp/zlink-d093/final-boundaries.log` |
| 명시된 deadline 없는 local callback 회귀 | 1 passed / 0 failed | `/tmp/zlink-d093/local-default.log` |
| ZoneWorld 1 | exit 1; G4 통과, replacement ActorCreate `104/RequestProtocolError` | `/tmp/zlink-d093/zoneworld-1/` |
| ZoneWorld 2 | exit 0; G4 통과, 전체 역할 정상 종료 | `/tmp/zlink-d093/zoneworld-2/` |
| `bash samples/run_samples.sh` 1회 | exit 1; 앞선 6개 sample 통과, ZoneWorld E5 재시작 후 mesh admission 미완료 | `/tmp/zlink-d093/samples/run.log:339` |
| unit half `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` 1회 | 1951 passed / 1 failed / 0 skipped | `/tmp/zlink-d093/unit-half.log` |

Unit half는 검토 보완 전 snapshot의 결과다. 이후 수정한 제거 순서와 local deadline 경계는
위 focused 26/0으로 검증했다. Unit half와 전체 sample runner를 추가로 반복하지 않았다.

G4 첫 실행에서 `g4-crash-51f5e2`는 `Unavailable`로 완료했다.
`/tmp/zlink-d093/zoneworld-1/g4-evidence/logs/zone-node-1.log:1184`에 typed exception이 있고,
`:1195` 이후 같은 flow `01a07071-ae14-7081-9eb6-508a9b02f340`로 오류·probe 응답이 이어진다.
해당 G4 topology의 종료 log에는 gateway·ops·zone-node-1·zone-node-3와 replacement
zone-node-2의 `Stopped/None`이 있다. 의도한 crash 프로세스는 이 종료 판정에서 제외한다.

G4 두 번째 실행은 `g4-crash-9eaf2c`이며
`/tmp/zlink-d093/zoneworld-2/g4-evidence/logs/zone-node-1.log:1299`에서 `Unavailable`을
관찰했다. G4 topology의 정상 종료 역할 모두 `Stopped/None`이다. 전체 두 번째 실행도
`zoneworld-2/evidence/ZoneWorld/logs/`의 gateway·ops·zone-node-1·zone-node-2·zone-node-3·
zone-node-replacement에서 `Stopped/None`을 확인했다.

Aggregate에서는 TicTacToe·Bingo·SupportChat·ShoppingMall·DeliveryDispatch·GameQuest가
통과했다. 이어진 ZoneWorld의 G4도 `g4-crash-adfda1`에 대해
`/tmp/zlink-d093/samples/g4-evidence/logs/zone-node-1.log:1183`에서 `Unavailable`을
관찰했다. 전체 runner는 이후 E5 재시작의 mesh admission 확인에서 실패했다.

## Java·Node parity 후속

- Java parity에 필요한 것은 D-093 규칙 1의 logical intent 제거 입력이다. 진단 당시에는 없었으며, 현재 별도 작업은 `ZLinkJavaDurableRequest.java:57`의 `targetLifecycleEnded`와 `ZLinkJavaRawMeshNode.java:6999`의 expectation/ready-peer 조회를 연결하고 있다. Initial route 부재, transient disconnect/reconnect, 제거 순서 두 가지를 같은 public 동작으로 검증해야 한다.
- Java ZoneWorld `PlayerActor.java:220`의 sample은 `DEADLINE_EXCEEDED`·`SHUTTING_DOWN`을 `Unavailable`로 합치지 않고 실제 kind를 검사해야 한다. 공유 workspace의 별도 작업에서 현재 이 오류 통합을 제거하고 있다. 이 작업은 Java 파일을 수정하지 않았다.
- Node `actor-local-native-join.ts:643`은 timeout 후 admitted peer·RID·generation으로 `RouteNotConnected`를 고른다. `service-stateful-runtime.ts:4068`의 durable 대상에 ActorJoin이 없으므로, 같은 in-flight 경계에서 physical disconnect 후 replay/reconnect와 logical intent 제거 시 즉시 `Unavailable`을 검증하고 runtime을 정렬해야 한다. Timeout까지 기다리는 peer 부재 판정만으로는 규칙 1과 동등하지 않다.

## BLOCKERS

- ZoneWorld 1은 replacement의 `bot-nw-y` 생성에서 `104/RequestProtocolError`로 실패했다. `zoneworld-1/replacement-failure/logs/zone-node-replacement.log:158`의 remote create 뒤 `:163`에서 startup이 실패한다. 오류 전달 위치는 `ZLinkBackendSpotNodeWrapper.cs:616`이며 정확한 protocol 거부 원인은 이번 D-093 범위에서 확정하지 않았다.
- Unit half는 `DirectReplyCompletionRegistryTests.cs:60`에서 재등록 assertion이 실패했다. 해당 registry는 `ZLinkDirectReplyCompletionRegistry.cs:35`, `:64`의 wall-clock retention을 사용하며, 이번 작업에서 수정하지 않았다. 원인은 별도 확인이 필요하다. 전체 unit half를 반복하지 않았다.
- Aggregate sample은 `ZW-E5-arm` 통과 뒤 zone-node-2를 재시작하고 mesh admission 완료를 기다리다가 실패했다. `/tmp/zlink-d093/samples/run.log:339`에서 `zn-c1fd4ef6-cf0f-4192-8d8a-31a372977cf0`와 `zn-0d49ee4a-e758-4262-ae40-f8521949004f` 양쪽 모두 완료 기록이 없다고 보고한다. 증거는 `/tmp/zlink-d093/samples/evidence/ZoneWorld/logs/`에 보존했다. Admission 미완료 원인은 확정하지 않았으며 deadline·재시도·monitor 상태를 추가하지 않았다.
- 중간 focused 실행에서 기존 ActorCreate `ready-after-submit`이 한 번 `InternalError(105)`를 반환했고, 새 lifecycle 테스트에서도 target 실행 전 첫 terminal이 한 번 관찰됐다. 기존 exhaustion 분리 진단은 12/0이다. 이 간헐 실패는 D-093 변경으로 우회하지 않았으며 보완된 focused 26/0에서도 재발하지 않았으나 이 간헐 결과의 root cause를 확정하지 않았다.
- 기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning은 수정 범위 밖이다.
