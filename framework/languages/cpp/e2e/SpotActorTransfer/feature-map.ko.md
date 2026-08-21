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
| ST-B1 | `implemented` | stateful remote transfer와 target state를 검사한다. Target authority commit(Location Store CAS 한 번, `location_committed`로 관측)이 `joined`보다 먼저 완료되는지 확인하고, target의 `location_committed`와 source의 `source_cleanup`이 같은 transfer id와 message-flow correlation을 공유하는지 확인한다. (mesh `commit_request`/`commit_ack` packet 교환은 8bae89dc0f 이후 존재하지 않는다 -- relocation commit은 target 쪽 Location Store CAS 한 번으로 바뀌었다.) |
| ST-B2 | `implemented` | target의 Location Store commit(`location_committed`) 뒤 success reply를 확인하고 source cleanup evidence가 아직 없을 때 source를 중단한 뒤 target generation과 packet 처리가 유지되는지 검사한다. |
| ST-B3 | `implemented` | adapter 미등록 시 빈 state transfer와 source·target callback 순서를 검사하고, commit 경계 marker가 같은 transfer id와 message-flow correlation을 공유하는지 확인한다. |
| ST-B4 | `implemented` | 명시적인 빈 state transfer 뒤 target state를 검사한다. |
| ST-C1 | `implemented` | admission 뒤 commit 전 source를 중단하고, target의 구조화된 `pending_admission_expired` evidence와 target membership·dispatch 부재를 검사한다. |
| ST-C2 | `implemented` | target commit 뒤 source 중단 후 location과 bound push를 검사한다. |
| ST-C3 | `implemented` | callback failure 네 종류를 실행하고, joined callback 실패 뒤 실제 actor packet 요청이 실패하며 target handler evidence도 생기지 않는지 검사한다. |
| ST-C4 | `부분` | exact-identity-conflict variant는 구현했고 3회 연속 통과를 확인했다 -- 같은 Actor를 같은 target Spot으로 공개 Join HTTP API로 동시에 두 번 호출하면, 정확히 한 건만 accepted(deferred)로 응답하고 나머지 한 건은 그 자리에서 즉시 `accepted=false`, `error_kind=FrameworkError:4`(rejected), evidence로는 "Actor join is already reserved or moving"라는 명시적 conflict 응답을 받는다. 승자 쪽은 `source_cleanup`까지 정상적으로 완료되고 target에서 request를 계속 처리하며, target에 conflict_restore evidence가 없는지도 확인한다. checksum-mismatch variant는 role-server E2E로는 구현하지 않았다 -- 대신 target 쪽 contract 절(spec 28 §12: "체크섬 불일치 시 target은 CAS로 진행하지 않고, 부분 조립된 payload로 restore하지 않으며, 명시적 실패 응답을 보낸다")을 `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp`의 `verify_relocation_assembly_rejects_checksum_mismatch`로 직접 커버했다 -- 자신의 manifest checksum과 일치하지 않는 chunk를 `relocation_state_assembly_t::accept()`에 넣어 conflict 결과와 restore할 것이 남지 않음을 확인한다. E2E variant는 살아 있는 두 role-server 프로세스 사이 wire에서 chunk가 실제로 손상되어 도착해야 하는데, 이 harness에는 아직 그 fault-injection seam이 없다. seam의 위치는 이번에 확인했다 -- `mesh_node_runtime.cpp`의 `relocate_application_actor`, `.send_state_chunk` 람다(약 1165행)는 인코딩 전 source 프로세스 메모리에 완성된 `protocol::relocation_state_t`를 들고 있어, `chunk.chunk_data` 바이트 하나를 뒤집으면 wire framing을 건드리지 않고 정확한 identity는 유지한 채(target의 assembly가 identity-mismatch로 무시하지 않고 conflict로 링크하도록) 깨끗하게 손상시킬 수 있다. 남은 문제는 이를 e2e client 프로세스에서 단일 Join 하나에만 범위를 좁혀 one-shot으로 arm하는 경로다 -- client와 ActorNode가 별도 프로세스라 `public_host_runtime_t`에 새 debug 전용 메서드를 추가하는 것만으로는 부족하고, `fw::app_t`(framework/include, 다른 모든 언어 바인딩과 공유하는 public 계약 표면)에 새 public API를 추가하거나 mesh 연결에 proxy를 세워야 한다. 둘 다 이번에는 보류했다 -- public API는 이 시나리오 하나를 위해 공유 계약 경계를 넘는 것이고, proxy는 ST-C4가 batched `all` 실행(run_e2e.sh:246) 안에서 actor-a<->actor-b mesh 연결 하나를 다른 십여 개 시나리오와 공유하는 상태라 안전하지 않다 -- 독립 실행되는 ST-F3A의 session-route proxy와는 다른 상황이다. `st_c4_scenario.hpp` 상단 주석에도 같은 내용을 기록했다. 2026-08-22 추가 조사: proxy가 batched `all` 안에서 위험하다는 위 판단은 해소 가능하다 -- 두 actor router를 127.0.0.2로 bind하고 127.0.0.1을 advertise해(ST-F3A와 같은 split) `Support/relocation_chunk_conflict_proxy.py`를 앞에 세우면 mesh가 정상 동작하고 ST-C1도 그대로 통과한다. 그러나 더 근본적인 blocker가 드러났다: 이번에 관측한 범위(ST-C4 Join, ST-C1, 수동 create-후-drain)에서 wire에 relocationState(52)가 한 건도 나오지 않았다. 원격 Actor Join은 capture한 state를 spot_actor_commit_route_request_t.transfer_state로 인라인 전송하고(mesh_node_runtime.cpp prepare_remote_application_actor_join, core_transfer=true), relocationState chunk를 만드는 유일한 경로인 maintenance_runtime.cpp relocate_send_state_chunks는 relocate_application_actor / relocate_application_unit 두 caller뿐이며 둘 다 app.cpp의 종료(drain) relocation에서만 호출된다. 실측: 두 actor router를 proxy 뒤에 두고 위 셋을 돌렸을 때 kind 30/31/34/40/52/53가 0건이었고 같은 연결로 actorJoin(28)은 흘렀다. 다만 DiscoveryRegistryHa SF-F7은 같은 join_spot 호출로 chunk-limit 경계를 시험한다고 주석에 적혀 있어(4096/4097/12289B fixture) 서로 어긋난다 -- SF-F7 주석이 낡았거나 그쪽 경로가 다른지는 확인하지 못했다(그 harness에 proxy를 붙여 kind=52를 세면 1런에 판정 가능). 즉 ST-C4의 계약 fault point는 source가 살아 있는 동안 canonical chunk wire를 쓰는 relocation 형태가 있어야 재현 가능하며, 이는 harness가 아니라 코디네이터/스펙 판정 사항이다. 또한 이번에 e2e 바이너리를 HEAD로 재빌드하니 ST-C4가 5/5 결정적으로 실패한다 -- 동시 Join 계약 부분(수락 1건 + FrameworkError:4 거절 1건, target transfer_in/joined)은 정상인데 마지막 `get_actor_ref(actor-b)`가 404 "actor was not found"로 떨어진다(ST-B1은 통과). 2026-08-21 05:45 빌드 바이너리로는 통과했으므로 dd234c3110/ea7805d54b 이후 cpp canonical-28·authority payload 작업의 회귀로 보이며, D1 검증 자체가 이 회귀에 막혀 있다. |
| ST-D1 | `implemented` | local·remote location은 joined 완료 전 기존 ref를 유지하고 완료 뒤 committed ref로 바뀌며, local 지연 중 packet이 target handler에 먼저 도달하지 않고 commit 뒤 target에서 처리되는지 검사한다. |
| ST-D2 | `implemented` | commit 뒤 source cleanup queue가 stale owner release를 실행하기 전후에 target packet과 generation snapshot이 유지되는지 검사한다. |
| ST-E1 | `implemented` | transfer 전후 같은 connector의 bound push 수신을 검사한다. |
| ST-E1A | `runtime partial; E2E blocked` | Public `actor_t`·`actor_factory_t<TActor>`·`actor_join_completion_t`와 source-generated operation ID를 사용하는 User Spot remote callback 경로는 구현했다. Target은 location commit 뒤 callback을 실행하고 성공 전에는 backlog를 열지 않으며, 실패한 callback은 같은 operation ID로 재시도하고 성공한 operation은 중복 실행하지 않는다. Relocation Store root에 reply와 cursor를 기록하고 target replacement에서 복구하는 경로는 아직 없다. |
| ST-E2 | `implemented` | transfer-out adapter 실패로 commit 전 transfer를 거절하고, source의 기존 bound session이 follow-up notify를 받으며 target에는 `bound_push`·`joined` evidence가 없는지 검사한다. |
| ST-F1 | `implemented` | `old-1` handler를 gate에서 대기시킨 상태에서 `old-2`와 Join, `moving-1`, `moving-2`를 같은 bound Session stream으로 제출한다. gate를 연 뒤 `new-1`을 보내고, source와 target의 handler evidence를 합쳐 `old-1`→`old-2`→`moving-1`→`moving-2`→`new-1` 순서와 각 ID의 단일 처리를 확인한다. |
| ST-F2 | `implemented` | moving 중 B1/B2를 보내고 target의 `backlog_enqueued`가 `location_committed`보다 앞서는지 검사한다. location 공개 직후 D1을 보내 join caller가 완료를 읽기 전에 B1→B2→D1 순서가 유지되는지도 확인한다. |
| ST-F3 | `implemented` | moving 중 S1/S2를 보내고 target의 구조화된 location commit evidence 직후 기존 bound session으로 S3/S4를 보내 join caller가 완료를 읽기 전에 전체 순서를 검사한다. |
| ST-F3A | `blocked` | actor-c와 actor-b→Session owner 한 방향의 raw byte hold proxy, bound Session의 public `ActorRef` snapshot 검증을 runner에 등록했다. Proxy가 actor-b의 transport handshake는 통과시키고 이후 bytes를 보관한 상태에서 A→B를 실행하면 target commit과 reply 생성은 완료되지만 Session owner의 ActorRef snapshot은 갱신되지 않는다. 정식 command 42/44가 요구하는 Session owner node generation·owner ID·owner lease generation·binding generation·relocation ID·high-water가 현재 C++ 직접 Join 전달 구조에 없어, 추정값으로 우회하지 않고 `BLOCKER-CPP-SESSION-ROUTE-ID`로 보류한다. `blocked`는 시나리오 폐기가 아니라 전달 구조 결정 및 구현이 필요하다는 뜻이며, 공통 spec은 수정하지 않았다. |
| ST-F4 | `implemented` | internal transport fixture가 Actor type·ID·node RID·generation을 포함한 exact old Actor ref를 사용한다. Follow 기간 안의 G1은 source의 `message_follow_relay`와 target의 단일 처리를 확인하고, route 제거 뒤 G2 request는 target handler에 들어가지 않으며 caller가 `Unavailable`을 받는지 확인한다. Public Actor API는 변경하지 않았다. |
| ST-F5 | `implemented` | 단일 A→B relocation과 Message Follow route 제거를 확인한다. 제거 뒤 exact old route request는 `Unavailable`로 끝나고 target handler에 들어가지 않으며, global Actor ID로 보낸 current request는 bounded convergence 안에 target B에서 한 번 처리되는지 확인한다. |
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

`run_e2e.sh all`은 ST-A1부터 ST-F6까지 기존 A~F 시나리오를 선택한다. ST-F3A는 개별 scenario로
등록했지만 위 production GAP 때문에 아직 aggregate 목록에는 넣지 않았다. source process 중단이 필요한
ST-B2, ST-C1, ST-C2는 다른 시나리오와 분리해 실행하며, 그 사이에 actor-a를 다시 시작한다. 모든
시나리오는 client가 역할별 evidence와 응답을 직접 판정하고, runner는 process 수명과 실행 순서만
관리한다. ST-F3A가 통과하고 `ST-G1~G6`, `ST-H1~H5`, `ST-I1~I6`이 모두 process E2E로 등록되기 전에는 Config 10 전체
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
