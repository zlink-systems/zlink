# C++ SpotActorTransfer E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md`

이 문서는 C++ Config 10 E2E의 현재 구현 상태를 공통 시나리오별로 기록한다. runner는 actor node
두 개와 session gateway 두 개, consumer를 서로 다른 process로 시작한다. actor는 처음 소유하는
actor node가 만들고 join을 시작한다. 이동은 별도 원격 생성 API가 아니라 기존 actor handoff 경로로
수행한다. `run_e2e.sh all`은 현재 A~F track만 실행하며, 현행 공통 문서의 모든 시나리오를
실행하지 않는다.
각 client 흐름은 `Client/Scenarios/st_*_scenario.hpp`에 ID별로 분리되어 있으며, 공통 client
context는 HTTP·connector 연결과 반복되는 evidence 조회만 제공한다.

| 시나리오 | 상태 | 현재 검증 |
|----------|------|----------------------|
| ST-A1 | `implemented` | Framework가 같은 node에 배치한 Actor와 Spot으로 same-node join을 실행한다. `admission -> location_committed -> joined -> leave -> join_completion_accepted` 순서와 Relocation Store read/write·Message Follow 0건을 검사한다. |
| ST-A2 | `implemented` | local admission 거절과 joined side effect 부재를 검사한다. |
| ST-A3 | `implemented` | joined gate가 유지되는 동안 actor packet이 완료되지 않는지 검사한다. |
| ST-B1 | `implemented` | stateful remote transfer와 target state를 검사한다. Target authority commit이 `joined`보다 먼저 완료되는지 확인하고, source·target 역할 evidence의 `commit_request`, `location_committed`, `commit_ack`, `source_cleanup`이 같은 transfer id와 message-flow correlation을 공유하는지 확인한다. |
| ST-B2 | `implemented` | commit ack 뒤 success reply를 확인하고 source cleanup evidence가 아직 없을 때 source를 중단한 뒤 target generation과 packet 처리가 유지되는지 검사한다. |
| ST-B3 | `implemented` | adapter 미등록 시 빈 state transfer와 source·target callback 순서를 검사하고, commit 경계 marker가 같은 transfer id와 message-flow correlation을 공유하는지 확인한다. |
| ST-B4 | `implemented` | 명시적인 빈 state transfer 뒤 target state를 검사한다. |
| ST-C1 | `implemented` | admission 뒤 commit 전 source를 중단하고, target의 구조화된 `pending_admission_expired` evidence와 target membership·dispatch 부재를 검사한다. |
| ST-C2 | `implemented` | target commit 뒤 source 중단 후 location과 bound push를 검사한다. |
| ST-C3 | `implemented` | callback failure 네 종류를 실행하고, joined callback 실패 뒤 실제 actor packet 요청이 실패하며 target handler evidence도 생기지 않는지 검사한다. |
| ST-D1 | `implemented` | local·remote location은 joined 완료 전 기존 ref를 유지하고 완료 뒤 committed ref로 바뀌며, local 지연 중 packet이 target handler에 먼저 도달하지 않고 commit 뒤 target에서 처리되는지 검사한다. |
| ST-D2 | `implemented` | commit 뒤 source cleanup queue가 stale owner release를 실행하기 전후에 target packet과 generation snapshot이 유지되는지 검사한다. |
| ST-E1 | `implemented` | transfer 전후 같은 connector의 bound push 수신을 검사한다. |
| ST-E1A | `runtime partial; E2E blocked` | Public `actor_t`·`actor_factory_t<TActor>`·`actor_join_completion_t`와 source-generated operation ID를 사용하는 User Spot remote callback 경로는 구현했다. Target은 location commit 뒤 callback을 실행하고 성공 전에는 backlog를 열지 않으며, 실패한 callback은 같은 operation ID로 재시도하고 성공한 operation은 중복 실행하지 않는다. Relocation Store root에 reply와 cursor를 기록하고 target replacement에서 복구하는 경로는 아직 없다. |
| ST-E2 | `implemented` | transfer-out adapter 실패로 commit 전 transfer를 거절하고, source의 기존 bound session이 follow-up notify를 받으며 target에는 `bound_push`·`joined` evidence가 없는지 검사한다. |
| ST-F1 | `implemented` | source `handoff_backlog`와 target `backlog_enqueued`를 같은 transfer correlation으로 검사한다. target은 prepare 뒤 받은 전체 backlog를 queue에 넣은 다음 location을 공개하며 P1→P2→P3 처리 순서도 확인한다. |
| ST-F2 | `implemented` | moving 중 B1/B2를 보내고 target의 `backlog_enqueued`가 `location_committed`보다 앞서는지 검사한다. location 공개 직후 D1을 보내 join caller가 완료를 읽기 전에 B1→B2→D1 순서가 유지되는지도 확인한다. |
| ST-F3 | `implemented` | moving 중 S1/S2를 보내고 target의 구조화된 location commit evidence 직후 기존 bound session으로 S3/S4를 보내 join caller가 완료를 읽기 전에 전체 순서를 검사한다. |
| ST-F4 | `partial` | explicit old Actor ref로 one-way G1/G2를 보낸다. G1의 `message_follow_relay`와 target 처리, route 제거 뒤 G2의 `message_follow_expired`와 target 미처리는 검사한다. 그러나 public global Actor ID로 제출한 message의 이전 route delivery를 transport fixture에서 지연하는 절차와 operation ID·reply route 보존 검증은 없다. |
| ST-F5 | `partial` | actor-a→actor-b→actor-a의 다른 Spot으로 연속 이동하고, source node별 Message Follow route가 하나인지 검사한다. Route 제거 뒤 old ref가 실패하는지도 검사한다. 그러나 public caller에서 route를 숨기는 transport fixture, 세 node multi-hop, process recovery와 queued byte 정리는 검증하지 않는다. |
| ST-F6 | `implemented` | source와 target의 구조화 evidence에서 같은 request id와 request flag가 보존되는지 비교한다. 같은 request id의 재시도는 backlog와 target handler에 한 번만 남고, 긴 timeout은 원래 caller reply로, 짧은 timeout은 일반 timeout과 late reply로 끝나는지 검사한다. |
| ST-G1 | `미구현` | yielded continuation과 모든 실행 lane을 포함한 relocation barrier process E2E가 없다. |
| ST-G2 | `미구현` | 큰 participant inventory와 typed capacity aggregate all-or-none E2E가 없다. |
| ST-G3 | `미구현` | PerActor Spot authority 선전환과 Actor별 source·target route 분할 E2E가 없다. |
| ST-G4 | `미구현` | relocation 중 stale `ToActor` Message Follow와 target queue 순서를 검증하는 E2E가 없다. |
| ST-G5 | `미구현` | Entry·PerActor Actor relocation interruption 목표와 초과 시 계속 진행을 검증하지 않는다. |
| ST-G6 | `미구현` | `ApplicationSignaled` readiness와 completion callback의 source·target owner를 검증하지 않는다. |
| ST-H1 | `component only` | `test_cpp_framework_execution`은 handler terminal 뒤 deferred activation과 handler failure 시 폐기를 검증한다. 실제 process의 immutable request와 Actor queue barrier E2E는 없다. |
| ST-H2 | `runtime integrated; process E2E blocked` | Admission reply와 prepare·finalize commit envelope가 immutable root reference와 checksum을 전달한다. Target은 materialization 전에 root를 검증하고 process-local admission이 없으면 root 또는 exact Actor authority가 가리키는 최신 cursor로 복구한다. Location commit 뒤 authority에 `Committed` root를 publish하고 callback 성공 뒤 `Delivered` root로 CAS한 다음 backlog를 제출하며, authority reference를 해제한 뒤 root를 정리한다. Focused runtime test는 통과한다. 현재 ActorNode E2E host는 context-owned factory, MessageContext handler signature와 location option 전환이 끝나지 않아 compile되지 않는다. 따라서 process E2E 완료 증거는 없다. |
| ST-H3 | `blocked` | Exact Context factory overload는 compile contract로 고정했다. ObjectGeneration을 유지하는 target Context와 source fencing을 process 간 E2E로 검증해야 한다. |
| ST-H4 | `partial` | `defer()`의 detached·duplicate·64개 제한과 absolute timeout 경로는 source와 focused test에 있다. 모든 허용·거부 문맥과 오류 37..39 parity E2E가 남았다. |
| ST-H4A | `미구현` | Deferred Join 등록량·payload·timeout 경계와 Relocate·Shutdown race process E2E가 없다. |
| ST-H4B | `미구현` | Join 뒤 Yield, awaited cycle과 reply terminal process E2E가 없다. |
| ST-H5 | `blocked` | MessageContext와 containing Spot handler signature를 사용하는 E2E host 전환이 남았다. |
| ST-I1 | `미구현` | 실제 encoded Actor·Spot payload profile과 경계·초과 크기 E2E가 없다. |
| ST-I2 | `미구현` | 다량 `RecreateOnRelocation`·`PreserveStateWith` Actor relocation의 처리 시간과 Actor·control service 연속성 E2E가 없다. |
| ST-I3 | `미구현` | 다량 Instance Spot·SpotWide relocation의 처리 시간과 Spot·Actor·control service 연속성 E2E가 없다. |
| ST-I4 | `미구현` | Actor·Spot × one-way·request × commit 전·후 Message Follow matrix가 없다. |
| ST-I5 | `미구현` | Message Follow 기간 종료, duplicate, deadline, generation, loop와 bound E2E가 없다. |
| ST-I6 | `미구현` | Actor·Spot multi-hop relocation과 Message Follow route 정리 E2E가 없다. |

## Message Follow 검증 경계

현재 runner가 실제로 확인하는 범위는 Actor one-way의 단일 relay, duration 경과 뒤 거부와
두 source node의 route 제거다. 이 검증은 ST-F4·F5의 일부다.

Track I는 아직 시작하지 않았다. Spot Message Follow route, Actor·Spot request reply correlation,
실제 encoded payload 크기, 대량 relocation 처리 시간, relocation 중 서비스 연속성, duplicate·deadline·generation·loop·hop·bound,
세 node multi-hop과 process recovery를 검증하는 scenario와 runner가 없다.

## 실행 범위

`run_e2e.sh all`은 ST-A1부터 ST-F6까지 기존 A~F 시나리오를 선택한다. source process 중단이 필요한
ST-B2, ST-C1, ST-C2는 다른 시나리오와 분리해 실행하며, 그 사이에 actor-a를 다시 시작한다. 모든
시나리오는 client가 역할별 evidence와 응답을 직접 판정하고, runner는 process 수명과 실행 순서만
관리한다. `ST-F3A`, `ST-G1~G6`, `ST-H1~H5`, `ST-I1~I6`이 모두 process E2E로 등록되기 전에는 Config 10 전체
완료로 판정하지 않는다.

## 설계 재검토

Actor transfer는 현재 actor를 소유한 node가 handoff 계약을 시작한다. 별도 controller가 remote actor를
생성하는 public API는 계약에 없다. client는 최초 소유 node에 생성과 join을 요청하고, 연속 이동은
이동할 때마다 현재 소유 node에 요청한다.

join 내부 제한 시간보다 HTTP 응답 제한 시간이 짧으면 runner 순서에 따라 정상 이동도 응답 전에 끊긴다.
HTTP server의 응답 제한은 정상 join 요청 제한보다 길게 한 곳에서 설정했다. callback 실패를 기다리는
ST-C3만 실패 판정 시간을 5초로 제한하고, 정상 이동은 12초를 사용한다. 또한 같은 node의 bound session은
이미 local sink가 있으므로 remote mesh route를 중복 등록하지 않고, 다른 node에 있는 actor만 mesh
route를 등록한다.
