# .NET SpotActorTransfer E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| ST-A1 | runtime evidence gap | Same-node join을 강제하고 `admission -> authority_committed -> leave -> joined -> success_reply`와 Relocation Store artifact 0건을 요구한다. Client와 ActorNode는 warning·error 0으로 build됐지만 실제 process `logs/20260728-042012-2770643`은 same-node authority commit marker가 없어 실패했다. Actor별 Message Follow route가 생성되지 않았다는 사실도 현재 public observation으로 판정할 수 없다. 이전 순서 일부만 확인한 실행은 현재 계약의 완료 증거로 사용하지 않는다. |
| ST-A2 | current Deferred Join actual-process 통과 | Public Actor handler의 즉시 응답은 Deferred Join registration 성공으로 판정하고, typed `Rejected` terminal은 `OnJoinCompletedAsync` callback에서 확인한다. Same-node target admission 뒤 Actor ObjectGeneration·owner node·state 12가 유지되고 leave·joined·authority commit·Capture·Restore와 User Spot handler는 모두 0건이다. 이후 global Actor one-way는 source Entry Spot handler에서 정확히 한 번 실행되며 evidence 순서는 `admission → reject_reply → entry_handoff_packet`이다. Actual-process 증거는 `logs/20260729-042702-217477`이다. |
| ST-A3 | current Deferred Join actual-process 통과 | Same-node target의 `OnJoinedActorAsync` gate가 닫힌 동안 public Actor request를 실제 제출하고 request terminal·source/target handler·leave·joined·completion이 모두 시작되지 않는지 확인한다. Gate release 뒤 queued request와 별도 follow-up request는 target Spot에서 state 13으로 각각 정확히 한 번 처리된다. Evidence 순서는 `admission → joined_wait → joined_released → joined → success_reply → queued request → follow-up request`다. Capture·Restore·transfer in/out·Message Follow와 relocation completed evidence는 0건이다. Actual-process 증거는 `logs/20260729-043746-628620`이다. |
| ST-B1 | current Deferred Join actual-process 핵심 경로 통과 | Actor를 먼저 만든 뒤 다른 node에 User Spot을 배치한다. Deferred Join handler가 끝나기 전에 global Actor one-way를 수락하고, relocation 완료 뒤 target handler에서 정확히 한 번 처리하며 source handler에서는 처리하지 않는다. Source `Capture`와 target `Restore`의 application payload byte 수·SHA-256이 같고, opaque Relocation Store write와 read도 reference hash·byte 수·payload hash가 같다. ObjectGeneration은 생성 때 발급된 값을 그대로 유지하고 owner만 target으로 바뀌며, 이동 뒤 global Actor request는 target Spot에서 state version 21을 조회한다. Application evidence 순서는 `admission → Restore → transfer_in → joined → success_reply → accepted packet replay → follow-up request`다. ST-B2와 함께 다시 실행한 actual-process 증거는 `logs/20260729-062527-95677`이다. `source_sealed`, `journal_staged`, `prepared`, `source_cleanup`, `completed`, `route_ack`, `steady_normalized`, `ready`, `admission_open`처럼 public application·provider 경계에서 직접 구분할 수 없는 private phase의 전체 순서는 이 결과로 완료를 주장하지 않는다. |
| ST-B2 | current Deferred Join source cleanup one-way과 source crash actual-process 검증 | Actor를 먼저 만들고 remote User Spot으로 Deferred Join한 뒤, source Entry Spot의 public `OnLeaveActorAsync`를 source cleanup 경계에서 대기시킨다. Target의 `transfer_in`·`joined`와 `success_reply`가 source cleanup gate 해제를 기다리지 않고 정확히 한 번 기록되는지 확인한 뒤 source process를 SIGKILL한다. Source process가 종료되어도 target owner와 Actor state는 유지되고, target은 completion callback을 replay하지 않으며 후속 request를 한 번 처리한다. 이 scenario는 process restart 뒤 다른 runtime takeover·completion replay를 성공 조건으로 사용하지 않는다. |
| ST-B3 | 전환 대상 | adapter 없는 Actor는 `RecreateOnRelocation` policy에서만 허용하는 현재 relocation 계약으로 전환해야 한다. |
| ST-B4 | 전환 대상 | 명시적 empty state를 `PreserveStateWith` adapter의 정상 payload로 처리하는 현재 relocation 계약으로 전환해야 한다. |
| ST-C1 | 구현 | target admission 뒤 source process 종료 시 transfer 실패와 복구를 검증한다. |
| ST-C2 | 구현 | target commit 뒤 source process 종료 시 target actor가 유지됨을 검증한다. |
| ST-C3 | 구현 | callback 실패와 transfer 실패의 public error 분류를 검증한다. |
| ST-D1 | 구현 | target commit 전후 location row의 owner와 generation 전환 시점을 검증한다. |
| ST-D2 | 구현 | stale source release가 새 generation location을 제거하지 못함을 검증한다. |
| ST-E1B | 미구현 | Relocation mode별 binding route를 검증하는 actual-process selector가 없다. |
| ST-E1C | 미구현 | Session location update retry를 검증하는 actual-process selector가 없다. |
| ST-E1 | 참조 묶음 | 공통 문서가 ST-E1A~ST-E1C를 함께 가리키는 상위 reference다. 판정은 하위 scenario 행으로 나눈다. |
| ST-E1A | 구현 | bound Actor를 destroy하고 같은 ActorId를 새 ObjectGeneration으로 만든 뒤 이전 binding의 request가 `ActorLocationStale`로 끝나는지 검증한다. 새 generation은 explicit bind 뒤에만 등록되며, 같은 Session에 bind된 다른 Actor의 route와 push도 유지되는지 함께 확인한다. 실제 process 실행은 `logs/20260725-094700-2778437`에서 통과했다. |
| ST-E2 | 구현 | 실패한 transfer가 기존 bound session route를 바꾸지 않음을 검증한다. |
| ST-F1 | 구현 | handoff 중 도착한 packet의 순서와 target replay를 검증한다. |
| ST-F2 | 구현 | direct packet이 handoff backlog를 추월하지 않음을 검증한다. |
| ST-F3 | 구현 | 같은 session의 `S1,S2,S3,S4` 순서와 다른 session 진행 격리를 묶음 반복 `logs/20260720-042454-2074240`, `logs/20260720-042515-2075011`, `logs/20260720-042523-2076425`에서 검증했다. |
| ST-F3A | 미구현 | Session owner pause와 owner lease fence를 실제 process에서 검증하는 시나리오가 없다. |
| ST-F4 | 외부 transport process 통과 | 실제 ROUTER는 서로 다른 loopback 주소에 bind하고 public `SetAdvertiseHost`로 외부 TCP proxy를 게시한다. Proxy는 Framework protocol을 해석하지 않고 application payload의 고유 marker 바이트만 streaming 검색한다. Placement 대상이 아닌 Object Client-only caller의 one-way와 request를 relocation 전에 보류한다. Commit 뒤 one-way는 final owner handler가 정확히 한 번 처리하고 이전 owner handler는 처리하지 않는다. Message Follow 만료 뒤 request는 handler에 도달하지 않고 public `Unavailable`로 끝난다. 최종 회귀 증거는 `logs/20260729-035740-3699697`이다. |
| ST-F5 | 외부 transport multi-hop process 통과 | ST-F4와 같은 외부 proxy에서 Object Client-only caller의 delivery를 보류한 뒤 A→B→C relocation을 실행한다. User Spot handler가 등록한 deferred Join은 제출한 Spot의 serial turn에서 실행될 때 Spot activation context도 함께 복원한다. Commit 뒤 one-way는 A와 B의 Message Follow를 거쳐 C handler에서 정확히 한 번 처리되고 이전 owner handler는 0회다. 만료 뒤 request는 handler에 도달하지 않고 public `Unavailable`로 끝난다. 기존 3회 연속 증거에 이어 caller-only topology 회귀 `logs/20260729-035809-3700993`도 통과했다. Route entry의 실제 제거와 next-hop 내부값은 public black-box로 관찰할 수 없어 provider/runtime contract test가 별도로 필요하다. |
| ST-F6 | 구현 | handoff 중 request의 원래 caller completion 상관관계와 늦은 reply timeout 격리를 같은 세 실행에서 검증했다. |
| ST-G1 | 미구현 | SpotWide·PerActor의 yielded continuation과 모든 실행 lane을 포함한 relocation barrier E2E가 없다. |
| ST-G2 | 미구현 | Actor 10,000개 inventory chunk와 typed capacity aggregate all-or-none E2E가 없다. |
| ST-G3 | 구현·process 통과 | Actor 100개가 속한 `PerActor` User Spot을 actor-a에서 actor-b로 이전한다. Spot·Actor의 ObjectGeneration 유지와 최종 owner, Actor별 Capture payload와 Restore payload의 byte 수·SHA 일치, Actor별 `transfer_in` 정확히 1회, 이동 뒤 Actor 100건과 Spot 1건의 target dispatch를 검증했다. Fresh process 증거는 `logs/20260728-075736-1950230`이다. |
| ST-G4 | 미구현 | 이동 중 stale `ToActor` Message Follow와 target queue 순서를 검증하는 E2E가 없다. |
| ST-G5 | Entry Actor·SpotWide 10 continuity 통과·추가 selector 실행 대기 | 공통 production histogram은 `zlink.relocation.interruption`과 `unit_kind`·선택형 `execution_mode`를 사용한다. Entry Actor selector는 relocation 전·중·후 request·one-way를 계속 보내고 metric duration과 source 마지막 handler→target 첫 handler/reply gap, loss·duplicate를 수치로 남긴다. Fresh `ST-G5-SMALL` 실행 `logs/20260728-092319-3148154`는 interruption 0.388267초, application handler gap 377 ms, request 71건·one-way 80건, loss 0·duplicate 0과 원래 operation ID·deadline·request correlation 보존을 통과했다. SpotWide 10 Actor 회귀는 `logs/20260728-233944-3394603`과 `logs/20260728-234157-3450332`에서 연속 통과했다. SpotWide 100 Actor가 정식 1초 SLO gate이며 1초를 넘으면 selector가 실패한다. PerActor Actor와 Instance Spot의 1.25초 느린 Capture·Restore, PerActor Spot shell에서 state adapter를 호출하지 않는 negative control selector를 추가했고 Client·ActorNode build와 정적 registration을 통과했다. 이 여섯 selector와 `ACTORS-100` actual-process 증거는 아직 없다. Atomic publication과 stale-route Spot Message Follow의 public 관측 gap도 남아 있어 ST-G5 전체 완료는 아니다. |
| ST-G6 | 구현·actual-process 실행 대기 | `ApplicationSignaled` Spot을 public fluent factory로 등록했다. Relocation 요청이 없을 때 source `Continued`, Capture에서 주입한 precommit abort 뒤 source `Continued`, 성공한 relocation 뒤 target `Relocated`, 기본 callback no-op을 각각 독립 object로 검증한다. Callback 뒤 일반 Spot request 순서와 같은 turn의 두 번째 `Defer()`·Framework operation `InvalidOperation`도 확인한다. Client·ActorNode build와 정적 selector gate는 통과했으며 Core handoff 전 actual process는 실행하지 않았다. Callback 중 target crash recovery와 API가 존재하지 않는 Entry·Instance context의 compile-time negative는 별도 증거가 필요하다. |
| ST-H1 | Actor handler cross-node 핵심 동작 통과 | Public Actor handler에서 `Defer()` 직후 mutable request를 변경하고 후속 Actor one-way를 제출한다. Handler terminal 전 target admission 0건, joined callback 대기 중 source·target handler 0건, target completion callback 뒤 accepted journal replay exactly-once와 source handler 0건을 확인한다. Runtime은 `Defer()` 등록 시 후속 direct frame capture를 열고 cross-node Handoff가 시작되면 기존 accepted journal로 승격한다. Target은 callback 성공 전 journal을 dispatch하지 않는다. Immutable snapshot과 `success_reply → handoff_packet` 순서는 actual process `logs/20260729-041951-5242`에서 통과했다. User·Entry Spot의 Spot·Timer handler와 absolute deadline 반복은 남은 ST-H1 matrix다. |
| ST-H2 | 미구현 | Join completion outcome, 128-bit operation ID와 crash recovery E2E가 없다. |
| ST-H3 | 미구현 | Context identity와 relocation 이후 source fence E2E가 없다. |
| ST-H4 | 미구현 | 허용 execution context, 중복 등록과 relocation error parity E2E가 없다. |
| ST-H4A | 미구현 | Deferred Join 등록량·payload·timeout 경계와 Relocate·Shutdown race E2E가 없다. |
| ST-H4B | 미구현 | Join 뒤 Yield, awaited cycle과 reply terminal E2E가 없다. |
| ST-H5 | 미구현 | MessageContext와 Actor handler signature parity를 실제 transport로 검증하는 E2E가 없다. |
| ST-I1 | 부분 구현·diagnostic only | Actor는 4 KiB·64 KiB·8 MiB·64 MiB, Instance Spot과 SpotWide는 64 KiB·1 MiB·32 MiB·64 MiB profile을 사용한다. Actor와 SpotWide small은 실제 cross-node relocation 뒤 `Restore`, public location, deterministic SHA와 opaque Store read-back을 확인했다. Actor adapter가 정확히 64 MiB를 반환하는 독립 selector도 있다. SpotWide 64 MiB 실행 `20260728-003937-1922103`은 typed failure 없이 외부 5분 제한에 도달했다. 수정한 Instance 실행 `20260728-022204-146992`도 네 fixture 생성 뒤 source `Capture` 3건, target `Restore` 0건에서 5분 relocation deadline과 HTTP 499로 끝났다. 따라서 Instance와 SpotWide 전체 profile은 완료 증거가 아니다. 이전 Instance activation-only 결과도 relocation 증거에서 제외했다. Queue·journal·timer, permit contention, 320 MiB·5-participant aggregate가 미구현이므로 전체 `ST-I1`은 `diagnostic_only`로 표시한다. |
| ST-I2 | 부분 구현·diagnostic only | 정식 selector는 `ST-I2-RECREATE-ON-RELOCATION`과 `ST-I2-PRESERVE-STATE-WITH`다. 기존 `ST-I2-RECREATE`와 `ST-I2-SNAPSHOT`은 runner compatibility alias로만 유지한다. 각 profile은 fresh host process에서 10,000/180초·64 units/s와 1,000/90초·16 units/s를 독립 계산한다. Moving target request·one-way가 0건이면 실패한다. Actor request는 original operation과 connection-scoped correlation을 일대일로 대조하고 duplicate 0건을 확인한다. Handler evidence는 sequence 집합이 아니라 도착 순서로 검증한다. Public terminal·최종 location·`ObjectGeneration`·final owner admission도 확인한다. 1초 interruption, encoded bytes/s, payload latency, CPU, peak RSS와 Store byte는 아직 측정하지 않으므로 두 profile은 `diagnostic_only`다. 현재 정본 규모는 creation-reservation production blocker를 우회하지 않고 재검증해야 한다. |
| ST-I3 | 부분 구현·diagnostic only | Instance Spot 1,000개와 SpotWide 100개×Actor 100개, control traffic 측정 scenario를 연결했다. Spot message flow는 relocation traffic 시작 전 flow ID watermark 이후의 received·replied pair만 사용한다. Public terminal·최종 location·generation과 final owner admission은 확인하지만, SpotWide final owner equality를 atomic publication 증거로 사용하지 않는다. Commit 전 participant 0개 공개와 commit 뒤 전체 공개를 관찰하는 수단이 없어 `spotwide_pre_post_visibility` blocker를 출력한다. 1초 interruption과 resource·Store 측정도 남아 있어 두 selector는 `diagnostic_only`다. |
| ST-I4 | Actor follow actual 통과·queue/hold/Spot 구현 실행 대기 | Placement 대상이 아닌 Object Client-only caller가 public global Actor ID로 one-way와 request를 제출하는 follow case는 `logs/20260729-035557-3692122`에서 통과했다. Source queue blocker 뒤 수락한 one-way를 relocation payload로 넘기는 Actor·Spot queue case, 느린 Capture 동안 이전 route에 도착한 request를 commit까지 보관하는 Actor·Spot hold case, commit 뒤 이전 Spot route로 보낸 one-way·request follow case를 추가했다. 모든 case는 public target API와 process 밖 TCP gate만 사용하며 handler exactly-once, 이전 owner handler 0건과 request operation ID를 판정한다. Build와 정적 gate는 통과했지만 여섯 새 selector의 actual-process 증거는 Core handoff 뒤 실행해야 한다. `MF-PA-SPLIT`은 남아 있다. |
| ST-I5 | Actor safety 부분 구현·external process 통과 | 두 Object Client connection에 pre-resolved request를 보류하고 B를 A보다 먼저 release해 correlation과 reply marker를 검증한다. 별도 paired TCP reply gate를 original 2초 deadline 뒤 release해 `TimeoutException` terminal이 바뀌지 않는지 확인한다. 5초 handler deadline과 Message Follow 만료 뒤 public `Unavailable`, target handler 0회도 검증한다. `logs/20260729-035615-3693303`에서 actual-process `passed` terminal을 확인했다. Duplicate, 이전 generation, loop, 8-hop과 1,024-message·16 MiB bound 및 Spot 조합은 남아 있다. |
| ST-I6 | Actor multi-hop 부분 구현·external process 통과 | Object Client-only caller의 pre-resolved request를 외부 TCP proxy가 보류한 뒤 A→B→C relocation을 실행한다. Release 뒤 C handler가 정확히 한 번 처리하고 A·B handler는 0회이며 reply state와 owner가 C인지 확인한다. `logs/20260729-035538-3690993`에서 actual-process `passed` terminal을 확인했다. Route cleanup 내부값, recovery와 Instance·SpotWide 조합은 별도 gap이다. |

정적 registration gate는 공통 계약의 정식 selector 이름을 직접 검사한다. Entry Actor의
기존 호환 이름과 `ST-G5-ENTRY-ACTOR-*` 이름은 같은 case를 실행한다. `PerActor` Actor,
`PerActor` Spot, Instance Spot의 `SMALL`, SpotWide Actor 10개·100개 selector도
각각 독립 실행으로 등록했다. 느린 adapter 반복, `ST-G6`과 여섯 Message Follow
authority-boundary case도 정식 selector로 등록했다. 남은 case는 unknown selector로
숨기지 않는다. Client dispatcher가
`public contract gap`으로 분류하고 non-zero로 끝낸다.

SpotWide 10개·100개 evidence는 Spot과 member Actor의 application state 합계만 기록하지
않는다. Public `IZLinkRelocationStore` wrapper가 관찰한 opaque blob의 실제 encoded byte
합계, 가장 큰 blob, reference·payload checksum read-back 일치와 최대 동시 Store I/O를
함께 기록한다. `MF-AO-FOLLOW`, `MF-AR-FOLLOW`, `MF-CORR`은 정식 selector로 등록했으며
case별 시작·완료, terminal 수와 이전 owner handler 0건을 별도 evidence로 출력한다.
`verify_relocation_contract.sh`가 구현 selector 34개, 명시적 gap selector 7개, 필수
evidence field와 non-public 접근 부재를 process 실행 없이 검사한다.

## Message Follow case별 구현 상태

Config 10의 Message Follow 전용 section은 case별 evidence를 요구한다. 현재 `.NET` process E2E의
구현 상태는 다음과 같다. Runtime route 구현이나 unit test 통과를 process E2E 통과로 계산하지 않는다.

| 대상·경계 | One-way | Request |
|---|---|---|
| Actor, seal 전 queue | 구현·실행 대기 | Public Actor queue blocker 뒤 one-way를 수락하고 relocation을 시작한다. Target exactly-once와 source application handler 0건을 판정한다. |
| Actor, commit 전 host ingress hold | 해당 없음 | 구현·실행 대기. 외부 TCP gate를 느린 Capture 중 release하고 original operation ID·reply route를 확인한다. |
| Actor, commit 직후 Message Follow | 외부 transport one-way·request 통과 | ST-F4의 one-way와 ST-I4의 request가 final owner에서 exactly-once이고 previous owner handler는 0회다. ST-I4는 paired TCP reply도 별도로 보류해 request terminal 경계를 검증한다. Seal 전 queue와 host ingress hold는 별도 case다. |
| Actor, duration 0·route 없음·만료 | 만료 request 외부 transport 통과 | ST-F4에서 만료된 request가 handler에 도달하지 않고 public `Unavailable`로 끝나는 것을 확인했다. Duration 0과 route 없음은 미구현이다. |
| Actor, duplicate·generation·loop·hop·bound | 부분 구현 | Correlation, reply backpressure deadline과 late terminal 불변은 실제 process에서 확인했다. Duplicate, generation, loop, hop과 bound는 미구현이다. |
| Actor, multi-hop·route cleanup | 외부 transport multi-hop 통과·route cleanup 부분 구현 | ST-F5에서 relocation 전 선택한 one-way를 A→B→C 두 Message Follow route로 전달하고 final owner exactly-once와 이전 owner handler 0회를 확인했다. 만료 request도 public `Unavailable`로 끝났다. Route entry의 실제 제거는 public observation이 없어 별도 provider/runtime contract test가 필요하며 recovery는 미구현이다. |
| Instance·`SpotWide` Spot, 모든 authority 경계 | SpotWide queue·follow 구현·실행 대기 | SpotWide hold·follow 구현·실행 대기. Instance 조합은 남아 있다. |
| `PerActor` Spot·Actor split | `MF-PA-SPLIT` 미구현 | `MF-PA-SPLIT` 미구현 |

Spot public call은 global Spot ID만 받으므로, commit 전에 Framework가 선택한 delivery를 process 밖
transport에서 지연하는 harness가 필요하다. 새 owner를 commit 뒤 다시 resolve해 보내는 call은 정상
direct delivery이며 Message Follow 증거가 아니다.

이전 전체 실행 `logs/20260720-044205-2109114`는 당시 기본 17개 시나리오와
별도 process generation을 사용하는 `ST-B2`, `ST-C2`, `ST-C1`이 모두 통과했다.
`ST-C1`은 공통 스펙에 따라 target의 `pending_admission_expired` marker를
30초 이내에서 기다리며, 전체 runtime drain을 admission 정리 증거로 대신 사용하지 않는다.

현재 `run_e2e.sh all`은 19개 시나리오만 실행한다. Transport delivery fixture를 연결한
`ST-F4/F5`, 부분 구현인 `ST-I1~I6`,
`ST-F3A`, ST-G·ST-H는 실행하지 않는다. 따라서 위 과거 실행과 현재
selector 모두 현행 Config 10 전체 완료 증거가 아니다. Runtime M6 gate가 끝난 뒤
전환 대상 시나리오를 현재 공개 API로 고치고, 미구현 행을 추가한 다음 전체 selector와
scenario registration을 다시 고정한다.

`ST-I1~I6`의 개별 selector도 현재 연결된 일부 case만 실행하므로 결과를
`diagnostic_only`로 출력한다. 각 case를 독립 selector와 evidence로 분리해 Config 10
matrix를 모두 채우기 전에는 selector exit 0을 Track I 완료로 해석하지 않는다.

## 현재 diagnostic transport fixture와 남은 교체

기존 transport에는 resolver가 선택한 delivery 하나만 멈추는 fault injection 표면이
없었다. Backend node 전체를 decorator로 감싸면 multipart frame, request completion과
socket 오류 변환을 fixture가 다시 구현해야 한다. 이 방식은 transport 내부 지식을 E2E에
중복시킨다.

ST-F4/F5는 process 밖 TCP proxy로 교체했다. ActorNode는 별도 loopback 주소에 ROUTER를
bind하고 public `SetAdvertiseHost`로 proxy endpoint를 게시한다. Proxy는 application
payload의 고유 marker 바이트만 streaming 검색한다. Marker가 TCP read 사이에서 나뉘어도
같은 connection의 후속 바이트를 보류하며 service wire, owner, generation과 Message Follow
hop은 해석하지 않는다.

ST-I4~I6도 같은 외부 proxy로 교체했다. 네 번째 ActorNode는 Object Client role만
등록하므로 placement 후보가 아니며, held application delivery가 relocation control
connection을 차단하지 않는다. Proxy는 marker가 있는 delivery를 보류하고 필요하면 그
connection의 다음 reverse bytes를 별도 gate로 보류한다. Framework protocol, authority와
reply capability는 해석하지 않는다.

## 현재 실행 blocker

Public opaque Store 위에 provider-backed authority repository를 연결했다. Redis↔in-memory
authority·generation parity와 실제 Redis 2-process focused test가 통과했고,
AutomaticTurnDispatch는 `TD-A1~TD-C5`를 연속 통과했다.

SpotActorTransfer의 User Spot actor handler는 sample과 같은 `Configure()` public
registration으로 고쳤다. Reservation 기반 Actor creation commit의 authority를 local
ownership coordinator에 연결하고 Actor publish를 그 뒤로 옮겼다. Actor와 User Spot을
같은 owner에 만든 `ST-A1`은 `logs/20260727-224418-4175123`에서 통과했다.

현재 User Spot fixture는 세 Actor node에 같은 stable type을 등록한다. Create DTO의
`TargetNodeRid`는 제거했고 public node-wide placement weight를 바꾼 뒤 Location descriptor에
반영됐는지 확인하고 global create를 실행한다. `ST-A1`은 이 경계에서 통과했다.
`ST-B1`은 Actor를 먼저 배치하고, 그 owner와 다른 node에 User Spot을 배치한다. Deferred Join
handler가 끝나기 전에 제출한 packet은 target restore와 joined callback 뒤에 target handler에서
정확히 한 번 처리된다. Source capture와 target restore의 payload, opaque Relocation Store
write·read, ObjectGeneration 유지, owner 전환과 후속 global Actor request를 함께 검증한 실행은
`logs/20260729-044949-1108908`이다. ST-H1과 ST-B1을 같은 fresh process에서 다시 실행한
회귀 증거는 `logs/20260729-045056-1141323`이다.

Track I의 Relocate 기반 host workload에는 public lifecycle 구현 차이도 남아 있다.
정식 .NET 계약은 mode를 받는 `RelocateAsync(...)`와 별도 `ShutdownAsync(...)`를
정의하며 production singleton과 DI 연결도 완료됐다. Actor·Spot scheduler도
`PlannedMaintenance`의 same-version과 `RollingUpdate`의 caller 지정 exact-version을
preflight와 실제 relocation에 동일하게 적용한다. Track I host workload도 연결했지만
이전 축소 process 실행의 relocation nonterminal 원인 가운데 target replay가 일반
dispatch guard에 막히던 문제를 수정했다. 최신 runtime으로 service continuity와 정본
규모를 다시 검증해야 한다.

추가 E2E 리뷰 뒤 scale 실행은 `diagnostic_only`로 분리하고 request와 one-way를 각각
독립 open-loop pacer로 바꿨다. 생성 count를 완료 수로 출력하던 부분도 제거했다.
Process E2E는 public host terminal, 최종 location과 terminal 이후 handler owner를
검증한다. Exact authority commit·target admission 순서와 SpotWide aggregate CAS는
provider·runtime contract test가 검증한다. 이 두 검증 계층이 모두 통과하기 전에는
정본 완료 증거로 사용하지 않는다. ST-I4~I6의 현재 연결된 Actor case는 외부 transport
harness에서 `passed`이지만 표에 남긴 Spot·recovery·bounded-capacity gap까지 완료했다는
뜻은 아니다.

Relocation payload 측정용 wrapper는 구현했다. Wrapper는 private envelope나 Store key를
해석하지 않고 public `IZLinkRelocationStore`의 opaque blob 크기와 SHA-256만 기록한다.
ActorNode와 Client project build는 warning 0, error 0이다. Instance Spot 네 profile은
과거 activation-only process에서만 통과했다. 현재 relocation selector는 Capture 3/4,
Restore 0에서 timeout되어 완료 증거가 아니다. `ST-I1`은 SpotWide commit 직후 public
lookup과 남은 workload profile을 해결하기 전에는 완료로 표시하지 않는다.

현재 fixture는 Object role에 허용되지 않는 fixed RID·manual peer를 제거했다. Framework가
`actor-a`·`actor-b`·`actor-c`와 Session prefix에 UUID suffix를 붙여 full RID를 발급하며,
runner는 실제 automatic peer 연결과 owner RID를 관측한다. ActorNode·SessionGateway·Client는
warning·error 0으로 build된다. 같은 stable type·caller 비지정 배치와 handler registration을
정렬한 process 실행이 통과하기 전에는 Config 10 완료 증거가 아니다.
