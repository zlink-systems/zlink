# .NET ChannelEgressRouting E2E feature map

기준 문서:
`framework/doc/framework/common/e2e/config-12-channel-egress-routing.ko.md`

`.NET` lane은 공통 role server, scenario client와 실제 process runner를 제공한다.
아래에서 `구현·실행 대기`는 source와 selector는 있지만 actual-process 증거를 아직 만들지 않았다는 뜻이다.
다른 config의 test는 Config 12 actual-process 완료 증거로 계산하지 않는다.

| 시나리오 | 상태 | 필요한 actual-process 증거 |
|---|---|---|
| CH-E2E-01 | actual 통과 | 실제 port 0 endpoint를 Location Store에 게시한 뒤 RouteMesh peer가 Ready로 수렴한다. 양방향 Channel request와 물리 연결 identity 중복 부재를 검증한다. Client는 정식 `ZLinkHttpClient`를 사용하고 runner는 공통 typed config writer를 사용한다. 최신 증거: `logs/20260729-044618-974448` |
| CH-E2E-02 | actual 통과 | RouteMesh handler가 `audit.record`와 `workflow.command`를 순서대로 호출하고 원래 reply에 두 downstream 결과를 보존한다. 증거: `logs/20260729-045524-1297423` |
| CH-E2E-03 | actual 통과 | Play Entry Spot Actor handler와 timer가 ClientServer `Async` 뒤 같은 turn에서 state 변경과 resume을 완료한다. 증거: `logs/20260729-003016-3958766` |
| CH-E2E-04A | actual 통과 | `logs/20260805-104857-1219987/`에서 실제 `workflow-client` process가 160개 request를 보내고 `workflow100`·`workflow300` evidence를 `100:300` weight에 맞춰 기록했다. `workflow300`을 weight 0으로 바꾼 동안 단일 target 선택을 확인한 뒤 두 target을 복원했다. |
| CH-E2E-04B | actual 통과 | `logs/20260805-105013-1227234/`에서 `workflow300`을 drain으로 전환한 뒤 진행 중 요청의 완료와 신규 요청의 `workflow100` 단일 선택을 확인했다. `workflow300.evidence.log`와 `workflow-client.stdout.log`에 drain 요청과 후속 request 결과가 남아 있다. |
| CH-E2E-04C | actual 통과 | `logs/20260805-105047-1229680/`에서 기존 `workflow-300` RID와 교체 후 `workflow300replacement`의 새 RID를 비교하고, 이전 RID가 제거된 뒤 새 Server request가 완료되는 것을 확인했다. `workflow.before-replacement.json`, `workflow.after-replacement.json`과 replacement evidence가 남아 있다. |
| CH-E2E-05 | actual 통과 | `logs/20260805-105914-1265624/`에서 Client role이 없는 `workflow-server-only` process의 request가 `NotFound`로 끝나고 evidence가 생성되지 않음을 확인했다. 별도 `workflow-client` request는 `workflow300` handler에 한 번 기록됐으며, E2E client는 `ZLinkHttpClient`만 사용한다. |
| CH-E2E-06 | actual 통과 | RouteMesh·ClientServer가 같은 ChannelName을 등록한 경우와 ClientServer Client 역할을 중복 등록한 경우가 각각 별도 process startup configuration error로 끝난다. 증거: `logs/20260729-045555-1335584` |
| CH-E2E-07A | actual 통과 | `logs/20260805-105202-1234879/`에서 등록하지 않은 ChannelName request가 1초 이내 `NotFound`로 끝나고, 같은 process의 등록된 `game.api` 경로는 영향을 받지 않는 것을 client assertion으로 확인했다. |
| CH-E2E-07B | actual 통과 | `logs/20260805-105215-1236427/`에서 Session role이 target RID나 endpoint 없이 `game.api` member를 호출하고 Api role evidence가 한 번 기록됐다. |
| CH-E2E-07C | actual 통과 | `logs/20260805-160550-2560586/`에서 `game.api` descriptor를 유지한 채 runner가 관리하는 TCP proxy를 중단하여 target으로 가는 연결만 차단했다. Public topology의 `game.api`는 `readyTargetCount=0`이 되었고 request는 `Unavailable`로 끝났다. API process의 evidence에는 operation ID가 기록되지 않았으며, `session.game.topology.after-network-block.json`과 `api-route-proxy.log`가 연결 차단과 상태 변화를 증명한다. |
| CH-E2E-08 | actual 통과 | WorkflowServer는 `game` Object Client로만 참여한다. ClientServer handler가 global SpotId와 ActorId로 Spot 다음 Actor를 요청하고 원래 reply에 순서를 보존한다. 증거: `logs/20260729-003016-3958766` |
| CH-E2E-09 | actual 통과 | `logs/20260805-105316-1241460/`에서 RouteMesh·ClientServer·fanout·STREAM을 함께 실행했다. Location Store의 모든 advertised endpoint가 `127.0.0.1`의 실제 port로 게시되고 `:0`이 아니며, ClientServer request·fanout delivery·STREAM connector connect가 모두 완료됐다. |
| CH-E2E-10 | actual 통과 | Result-free ClientServer send를 제출하고 두 Server가 선택 구간에서 handler를 각각 실행하며 전체 handler 수가 제출 수와 정확히 일치한다. 증거: `logs/20260729-045617-1359918` |
| CH-E2E-11 | actual 통과 | Session이 target RID나 endpoint 없이 `game.api` ChannelName만 사용해 remote Api request와 send를 처리한다. 증거: `logs/20260729-045637-1365103` |
| CH-E2E-12 | 부분 actual | 같은 process Client+Server와 remote Server weighted 후보를 검증하고 local weight를 `0`으로 바꾼 동안 remote만 선택한 뒤 복원했다. drain variant는 미구현. `logs/20260729-051215-2078936` |
| CH-REG-01 | actual 통과 | Session과 Play가 같은 RouteMesh의 서로 다른 ChannelName으로 양방향 request를 실행하고, 각 handler가 원래 ChannelName과 reply context를 보존한다. 증거: `logs/20260729-045702-1376921` |
| CH-REG-02 | actual 통과 | Session·Play Object Server의 Redis Location·Relocation Store, stable Snapshot actor/spot factory, Node·Spot·Actor direct, Session Entry→Play User Spot deferred join과 bound-session push를 검증한다. 증거: `logs/20260729-003347-3978117` |
| CH-REG-03 | actual 통과 | Logical Multicast의 remote subscribed Spot delivery와 classic Pub/Sub remote delivery를 각각 exactly-once로 검증한다. 증거: `logs/20260729-004432-4150890` |
| CH-REG-04 | 부분 구현·실행 대기 | 32개 병렬 request correlation·terminal uniqueness 구현. timeout·cancellation·disconnect·Spot shutdown은 미구현 |
| CH-REG-05 | 부분 actual | 같은 ClientServer endpoint에서 WorkflowServer를 종료·재시작하고 새 RID 선택, 이전 RID 제거와 request 완료를 검증했다. lifecycle generation public evidence와 이전 generation late reply 경쟁은 아직 미구현이다. 증거: `logs/20260729-004432-4150890` |
| CH-REG-06 | actual 통과 | 정상 RouteMesh와 ClientServer request가 application retry 없이 각각 1초 안에 완료된다. 증거: `logs/20260729-045757-1462397` |
| CH-REG-07 | actual 통과 | 공통 fixture의 일곱 sample·RouteMesh·ChannelName과 .NET sample source 일치. `logs/20260729-050208-1671604` |
| CH-REG-08 | actual 통과 | `logs/20260805-105355-1244199/`에서 Session·Play·Api의 GameMesh peer snapshot과 Location Store를 비교해 physical peer RID와 listener endpoint 중복이 없음을 확인했다. |
| CH-REG-09 | actual 통과 | sample source에 `PreferredNodeRid`·`PreferredRoutingId`가 없는지 검증. `logs/20260729-050242-1677966` |
| CH-REG-10 | 부분 actual | 같은 ChannelName Client+Server, weighted local·remote 선택과 local weight `0` 제외·복원을 검증했다. drain variant는 미구현. `logs/20260729-051223-2078916` |

2026-07-29 actual 과정에서 `ZLinkManagedMeshNode`가 port 0 bind 요청값을 그대로 게시해 모든
descriptor endpoint가 `tcp://127.0.0.1:0`이 되는 production gap을 발견했다. Runtime은 bind 직후
socket의 실제 endpoint를 저장하도록 수정했다. `CH-E2E-01`은 session peer 2개와 play peer 2개가
Ready인 상태에서 통과했다. Location evidence의 세 endpoint는 서로 다르며 port는 모두 0이 아니다.

같은 실행 과정에서 Node direct handler context가 registry lookup용 빈 channel key를 MeshName으로
노출하던 문제를 수정했다. Node direct context는 실제 등록 MeshName인 `game`을 받으며
`CH-REG-02` actual에서 확인했다.

Logical Multicast는 service wire schema의 command `23`을 사용해야 하지만 .NET managed runtime이
remote publish를 Channel send command `18`로 보내고 있었다. Schema가 정의한 ChannelName, topic과
source SpotId를 command `23`으로 encode·decode하고 remote node의 일치하는 Spot mailbox로 전달하도록
수정했다. `CH-REG-03` actual에서 remote Logical Multicast와 classic fanout이 각각 정확히 한 번
전달됐다.

ClientServer discovery descriptor를 제거하기 전에 Location owner lease를 해제하던 종료 순서도
수정했다. 정상 종료는 owner write를 막은 뒤 auto-connect discovery descriptor를 현재 owner
fence로 제거하고, 그 다음 owner lease를 해제한다. `CH-REG-05` actual의 교체 전 process는
`ForceStopping` 없이 종료됐다.

`all` selector는 누락된 네 selector를 출력하고 실패한다. aggregate runner에는 등록하지 않았다.
모든 필수 selector가 실제 process에서 통과한 뒤에만 `all`과 aggregate inventory를 활성화한다.
