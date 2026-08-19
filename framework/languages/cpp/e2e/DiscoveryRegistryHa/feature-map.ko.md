# C++ DiscoveryRegistryHa (Config 6 StoreFailure) E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

`DiscoveryRegistryHa`는 C++ build target과 runner directory의 식별자다. 이 실행 lane이 검증하는
공개 계약과 시나리오 ID는 Config 6 StoreFailure의 `SF-*`만 사용한다.

이 문서는 C++ Config 6 E2E의 현재 구현 상태를 기록한다. 실행 구성은 Redis
location store를 공유하는 provider와 consumer, scenario를 구동하는 client로 나뉜다.
열 개 client 검증은 `Client/Scenarios/sf_*_scenario.hpp`에 ID별로 분리되어 있고
`Client/main.cpp`는 설정과 dispatch만 담당한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| SF-A1 | 구현 | Redis location store가 정상일 때 provider 2개가 MeshNode descriptor로 보이고, consumer request가 provider에 도달한다. consumer/provider runtime status는 store와 owner lease가 healthy이고 실제 owner lease 갱신 시각과 last refresh 시각이 있음을 보여 준다. |
| SF-A2 | 구현 | C++ Redis store는 watch 없이 polling 경로로 동작한다. 초기 부재 확인 뒤 별도 `api-c`를 추가하고 polling 제한 안의 peer 반영과 실제 routing 응답을 확인한다. 정상 종료 뒤에는 같은 제한 안의 peer 제거와 후속 routing 제외를 public `/query/*`와 `/profile/request`로 검증한다. |
| SF-B1 | 구현 | Redis container process를 정지한 동안 기존 연결 request가 계속 성공하고, runtime status가 store unhealthy로 바뀐 뒤 빈 store 재기동 후 healthy로 회복된다. |
| SF-B2 | 구현 | Redis 정지 중 `api-b`를 새 channel endpoint에서 재기동한다. store failure grace를 넘길 때까지 기존 `api-a` 연결의 request만 성공하고, 빈 store 복구 뒤 새 endpoint row가 등록되어 `api-b`가 다시 요청을 처리한다. |
| SF-C1 | 구현 | provider `api-b`를 SIGABRT로 crash시키면 raw Redis descriptor record는 남지만 framework의 owner lease join이 lease 만료 뒤 live descriptor/runtime snapshot에서 제외하고, 이후 request는 survivor `api-a`로만 간다. |
| SF-C2 | 구현 | provider `api-b`가 공개 drain lifecycle로 `Draining=true`를 게시하고 실제 polling 전파 상한 동안 lease를 갱신한다. consumer는 연결을 유지한 채 신규 request 선택에서 `api-b`를 제외한다. terminal `drained` 뒤 owner row와 lease가 TTL 만료 전에 제거되고 process가 정상 종료된다. |
| SF-D1 | 구현 | 두 provider 연결을 실제 request로 준비하고 장애 전부터 복구 뒤까지 traffic을 유지한다. local row 재등록과 heartbeat 유예 뒤 status가 회복되며, 두 endpoint의 Connected/Disconnected count가 늘지 않는다. |
| SF-D2 | 구현 | 장애 전부터 지속 traffic을 흘리고 최대 성공 간격을 제한한다. Redis 정지 중 `api-b`가 crash된 뒤 `api-a` socket count는 유지되고 `api-b` Disconnected만 증가하며, owner lease join에서 dead row가 제외된다. |
| SF-D3 | 구현 | Redis process 정지·재기동 동안 runtime heartbeat 상태가 healthy → unhealthy(last error 포함) → healthy 순서로 관측된다. 장애 중에는 마지막 성공 시각을 보존하고, 복구 뒤 owner lease 갱신 시각과 last refresh 시각이 장애 전 값보다 증가하는지 확인한다. 상태 조회 자체는 store probe를 실행하지 않는다. |
| SF-E1 | 구현 | harness의 TCP proxy가 실제 Redis 응답에 300ms 지연을 주입한다. 지연된 descriptor query가 실제로 느려지는 동안 같은 consumer process의 runtime status 조회와 application request p99가 baseline 기반 budget 안에 남고, 지연 해제 뒤 request가 정상 복구되는지 검증한다. |

표준 `/profile/request`는 내부 retry 없이 5초 제한의 framework request 한 번만 실행한다. 따라서 각 scenario의 request 성공은 늦은 재시도로 복구된 결과가 아니라 해당 시점 연결의 실제 결과다.

## Track F — Verify Public Results Of Relocation And Owner Recovery

Track A-E는 `Server/Provider`·`Server/Consumer`만 사용하며 stateful Actor/Spot을 두지 않는다.
Track F(SF-F2, SF-F3, SF-F7, SF-F11)는 relocation 검증이 필요해 별도 참가자 `Server/Relocation`을
추가했다 -- `e2e/SpotActorTransfer/Server/ActorNode`와 같은 entry spot·user spot·Actor 구조를
따르되, 이 네 시나리오에 필요한 만큼만 남기고 stream/bound-session/message-follow 경로는 들어
있지 않다. relocation node 두 개(`df-a`, `df-b`)는 Track A-E의 provider/consumer와 별도
Redis(Relocation Store 전용, Location Store와 분리)를 사용하며 `SCENARIO`가 SF-F2/F3/F7/F11일
때만 기동한다. 전부 direct-transfer 경로만 사용한다 -- base/delta capture는 이번 세션에
product에서 제거되어 어떤 시나리오도 그 경로를 참조하지 않는다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| SF-F2 | `통과` | variant 1(source capture를 owner-lease renew interval(1초) 여러 번을 넘겨 gate로 붙잡았다가 풀어도 relocation이 성공하고 state가 보존되는지)과 variant 2(target 명시적 실패 `df-fail-transfer-in` 후 source가 location/state를 유지하고 계속 응답하며, 별개의 새 relocation은 정상 완료)가 모두 통과한다. 이전 기록의 variant 2 probe 무한 정지는 receiver-side fence read가 설정된 `owner_lease_fencing_margin`(이 harness 500ms) 대신 하드코딩 5초 기본값을 쓰던 결함(public_host_runtime.cpp `read_route_owner_fence` 기본 인자)이 owner-lease TTL(3초)과 결합해 fence를 영구 nullopt로 만들던 문제와 같은 계열로, margin 전달 수정 후 해소됐다. 검증 로그: `logs/20260819-121924-3273947`, `logs/20260819-121944-3274776`(2회 연속 green). 단, 수정 직후 첫 2회 실행(`logs/20260819-121232-3268496`, `logs/20260819-121709-3271398`)에서는 RelocateReq가 actor에 ~19초 늦게 도달해 wait_evidence(20초)가 timeout되는 간헐 스톨이 재현됐다(rebuild 직후 부하 의존으로 추정, 이후 재현 안 됨) -- 재발 시 이 로그를 기준으로 비교할 것. |
| SF-F3 | `통과` | Relocation Store 전용 Redis container만 정지한 상태에서 Relocate가 direct transfer로 성공하고 target state checksum이 원본과 일치하는지 검사한다 -- core claim 통과, 검증 로그: `logs/20260819-122005-3275572`. Store record에 의존하는 pending-request terminal 재구성 하위 variant(relocation 도중 완료되는 요청의 caller 결과가 store 복구 뒤에만 재구성되는 경로)는 구현하지 않았다 -- 이 harness는 bound-session/backlog 경로를 들고 있지 않으며, 그 경로는 `e2e/SpotActorTransfer`의 ST-F3가 이미 별도로 검사한다. |
| SF-F7 | `부분` | 이전의 `DEBUG_LEAVE_GATE ... leave_submitted=0` 정지는 사라졌고, 첫 번째 fixture(chunk 한계와 정확히 같은 4096바이트)는 relocation 자체가 완료된다 -- target evidence에 `transfer_in|4096`, `joined`, `join_completion_accepted`, probe handler 실행(`packet_handler|at-chunk-limit`)까지 기록된다. 하지만 relocation 직후 cross-node로 relay되는 probe의 **응답**이 caller node로 돌아오지 않아(ProbeStateReq가 target에서 수신·처리되지만 reply가 유실, message-follow relay reply 경로) client HTTP가 timeout으로 실패한다. 두 번째(4097바이트)·세 번째(in-flight budget 초과) fixture는 아직 도달하지 못했다. 3회 재실행 모두 동일 지점에서 결정적으로 실패: `logs/20260819-122020-3276368`, `logs/20260819-122111-3277783`, `logs/20260819-122208-3278726`(mesh trace 포함 -- fence-read/bind-admission 거부는 0건, 즉 fence/margin 계열 아님). relay reply 경로가 수리된 뒤 재검증이 필요하다. |
| SF-F11 | `부분` | 공통 variant만 구현했다 -- A는 `df-fail-transfer-out`(capture가 relay-ready 전에 즉시 throw)으로 direct transfer가 명시적으로 실패하고 source memory의 기존 payload가 그대로 유지되는지, B의 독립된 relocation이 A의 바이트와 섞이지 않고 B 자신의 checksum으로 target에 복원되는지 검사한다. waiter 취소 하위 variant는 harness에 취소 seam이 없어 미구현. 이전의 `DEBUG_LEAVE_GATE` 정지는 사라졌고 evidence 수준에서는 전부 정확하다 -- A의 `transfer_out_failed`·`join_completion_failed|12`·source probe 성공(`packet_handler|after-transfer-failure`), B의 `transfer_in|36`·`joined`·`join_completion_accepted`·target handler 실행(`packet_handler|after-b-relocation`)까지 기록된다. 하지만 SF-F7과 동일하게 B relocation 직후 cross-node probe의 reply가 유실돼 client가 timeout으로 실패한다(message-follow relay reply 경로, SF-F7 행 참조). 로그: `logs/20260819-122255-3280224`. relay reply 경로가 수리된 뒤 재검증이 필요하다. |

## 검증 방법

`run_e2e.sh all`은 SF-A1부터 SF-E1까지와 Track F 중 현재 green인 SF-F2/SF-F3를 실행하고,
알려진 실패 상태인 SF-F7/SF-F11은 건너뛰되 aggregate 출력에 실행/건너뜀 목록을 명시한다
(거짓 "all passed" 인상 금지 -- 스크립트 상단 SKIPPED_SCENARIOS 주석 참조). 단일 시나리오는
해당 ID를 인자로 전달해 실행한다. runner는 Redis 장애와 process
종료가 시나리오에서 요청한 fault injection인지 구분하고, 예상하지 않은 역할 종료는 실패로
처리한다.
