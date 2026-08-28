# Config 6 / Config 10 e2e Authoring 계획 (H-7·H-8)

작성 2026-08-20 (codex terra high 스코핑 리서치, Claude 검토·반영). relocation 캠페인
체크리스트 H-7/H-8의 근거 계획. **핵심 판정: 새 public API 불필요(c=0)** — 필요 public
surface(location_runtime_query, activation concurrency, job-queue limit, bound-session
등)는 4언어에 이미 존재. 필요한 것은 (1) E2E 하니스/fixture(대부분), (2) 소수 런타임
수정(SF-C3/R0 키스톤 + ST-I1/I2/I3). E2E 때문에 private route/frame API 추가 금지.

**착수 우선순위**: R0(SF-C3 same-RID replacement admission, Node raw-service-mesh-runtime.ts:989)
= 키스톤(SF-F9·multi-role replacement 기반) → H1 stateful-store fixture → H2 operational/
multi-role/capacity → H3 cross-lang → H4 config-10 base → H5 SpotWide/PerActor → H6 message-follow.

---

## 판정

현재 문서 기준으로 요청의 수량은 서로 맞지 않습니다.

- Config 6은 현재 `SF-C5A`까지 포함해 **27개** 시나리오이며, 열거하신 미구현 ID는 **15개**입니다.
- Config 10의 Track E/G/H/I는 `5 + 6 + 7 + 6 = 24개` ID입니다. “28개”가 되려면 다른 Track의 세부 ID까지 포함한 별도 기준이 필요합니다.
- 언어별 feature-map도 상충합니다. 특히 Node map은 SF-C3를 구현으로 기록하지만, 현재 런타임 코드에는 replacement candidate를 admission 전에 버리는 실제 결함이 있습니다. 따라서 그 완료 표시는 신뢰할 수 없습니다.

아래는 SF-C3의 코드 위치가 Node인 점에 맞춰, 현재 공통 계약과 Node E2E lane을 기준으로 한 최소 선행 조건입니다. 결론부터 말하면, 이 범위에서 **새 public API를 설계할 항목은 없습니다(c=0)**. 필요한 것은 런타임 교정과 E2E harness/fixture입니다. E2E 때문에 private route/frame API를 추가하면 안 됩니다.

## Config 6 선행 조건

분류: `B` = harness/fixture, `R+B` = 런타임 수정 뒤 fixture. 현재 public API만으로 바로 작성 가능한 `A` 항목은 없습니다.

| Scenario | 정확한 계약 | 분류 | 최소 선행 조건 |
|---|---|---:|---|
| SF-B3 | discovery grace와 무관하게 owner lease 만료 뒤 Instance Spot 신규 request는 `Unavailable`, timer도 더 실행되지 않음 | B | Instance Spot factory, public request endpoint, periodic timer evidence, 외부 Location Store pause |
| SF-C3 | old lifecycle 재개 뒤에도 same RID replacement가 current ready peer이며 old handler는 실행되지 않음 | R+B | same-RID process pause/resume fixture + 아래 R0 런타임 수정 |
| SF-C4 | RouteMesh 2개, ClientServer, fanout publisher를 한 host lifecycle로 교체하고 각 role marker를 정확히 한 번 처리 | B | multi-role host profile, role별 consumer/client/subscriber, replacement orchestration |
| SF-C5 | 1,001 objects를 page size 1/100/1000으로 중복·누락 없이 조회 | B | 1,001-object creator 및 page-drain client fixture |
| SF-C5A | Missing은 absent, Creating/Ready/Unavailable은 exact/page 모두 일치, Store failure는 partial page 없는 전체 error | B | creation gate, owner-loss fixture, Store-failure injection |
| SF-F1 | 서로 다른 언어의 create/request/relocation 뒤 global ID·ObjectGeneration·state가 동일 | B | 최소 2개 언어 provider/caller와 동일 typed packet/state fixture |
| SF-F4 | relocation은 ObjectGeneration 유지, close/destroy 후 recreate는 새 generation, old exact ref는 새 object에 무효 | B | Actor·User/Instance Spot의 relocate/close/recreate fixture |
| SF-F5 | Creating owner crash 뒤 pending과 follow-up 모두 bounded terminal 하나, stale owner dispatch 없음 | B | cold-activation initialization gate, SIGKILL/restart orchestration |
| SF-F6 | concurrent create/delete 중 scan은 page cap·unique IDs 보장, 다음 scan은 mutation 반영 | B | SF-C5 mass fixture + first-page mutation gate |
| SF-F7 | chunk limit 경계·초과·in-flight budget 초과 state가 checksum/length 보존으로 복원 | B | deterministic large-state capture/restore fixture와 public chunk-limit configuration |
| SF-F8 | target restore 중 target lease 만료면 relocation은 target unavailable, source owner 유지 | B | target Restore gate, SIGSTOP/SIGCONT, source/target evidence |
| SF-F9 | old Channel provider cleanup이 replacement descriptor를 제거하지 못하고 replacement만 선택됨 | R+B | SF-C3 replacement infrastructure 재사용; stateful object fixture 불필요 |
| SF-F10 | 다수 accepted request와 relocation이 겹쳐도 request별·relocation별 terminal 하나 | B | request reply gate, operation-ID journal, relocation trigger |
| SF-G1 | Actor/Spot/stable-type slot을 atomic reserve; capacity failure는 `CapacityExceeded`, factory failure는 slot 반환 | B | Actor+User Spot factories, concurrent create driver, capacity matrix |
| SF-G2 | population 0은 unlimited, activation concurrency만 factory active count 제한; Entry Spot 비집계 | B | activation gate fixture, public concurrency configuration, active-count evidence |

`SF-C5/C5A`는 새 API가 아닙니다. 4언어 모두 이미 public operational query를 규정합니다: C++ `location_runtime_query_t`, .NET `IZLinkLocationRuntimeQuery`, Java `ZLinkLocationRuntimeQuery`, Node `ZLinkLocationRuntimeQuery`. 각각 exact lookup, bounded `listObjectLocations`, `Creating/Ready/Unavailable`, partial-page 없는 Store failure를 포함합니다. [C++]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/cpp/interfaces/07-location-store.ko.md:389) [.NET]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md:162) [Java]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/java/interfaces/location-maintenance.ko.md:258) [Node]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:58)

## Config 10: Track E/G/H/I 선행 조건

| Scenario | 정확한 계약 | 분류 | 최소 선행 조건 |
|---|---|---:|---|
| ST-E1 | remote Join 뒤 rebind 없이 같은 Session이 target push를 한 번 수신 | B | existing bound-session fixture 확장 |
| ST-E1B | actor-only, PerActor, SpotWide relocation 모두 binding route 갱신 | B | 세 relocation mode와 bound client 3개 |
| ST-E1C | route update는 one-way; seal timeout 뒤 기존 physical session 종료, 늦은 update는 no-op, reconnect+explicit bind만 복구 | B | target→Session-owner opaque network blocker, connector lifecycle probe |
| ST-E1A | recreate된 Actor는 explicit bind 전 old Session에 push 금지 | B | destroy/recreate+bound-client fixture |
| ST-E2 | failed relocation 뒤 binding은 source Actor 유지 | B | target-connect failure variant |
| ST-G1 | yielded continuation도 동일 Actor turn; relocation과 동시에 실행 금지 | B | Actor Yield gate, global active-count evidence |
| ST-G2 | live job upper bound보다 큰 SpotWide backlog도 lazy permit으로 순차 처리; no-candidate/state mismatch는 source 유지 | B | SpotWide backlog generator, public job-queue cap, target state-mismatch fixture |
| ST-G3 | PerActor host relocation 후 Actor별 current route·membership·state 유지 | B | multi-location PerActor Spot fixture |
| ST-G4 | SpotWide move 중 ToActor message가 target에서 actor별 serial/exactly-once 처리 | B | opaque network delivery gate와 sequence journal |
| ST-G5 | relocation 중 loss/duplicate 없이 interruption p50/p95/p99 기록 | B | fixed-rate workload runner와 monotonic metric collector |
| ST-G6 | application turn 종료 뒤 relocation, target 준비 뒤 next turn; global active count ≤ 1 | B | SpotWide `ApplicationSignaled` fixture와 application gate |
| ST-H1 | handler `Defer` 후 nonblocking return, target `OnActorJoin`, immutable request payload | B | Actor handler evidence triplet: defer/return/admission |
| ST-H2 | Accepted/Rejected/failure completion이 operation ID별 정확히 한 번 | B | accept/reject targets, completion journal; crash-recovery variant 포함 |
| ST-H3 | admission은 Actor ID+request, completion은 Operation ID+result/ref로 분리 | B | callback evidence fixture |
| ST-H4 | invalid defer context/duplicate defer는 hidden Join 없이 config/operation failure | B | invalid-context and duplicate-turn variants |
| ST-H4A | timeout/accept race에서도 operation당 terminal 한 번, timeout 확정 뒤 새 target callback 없음 | B | admission gate와 deadline-controlled driver |
| ST-H4B | Yield/await 중에도 request reply와 Join completion이 각각 한 번, correlation 보존 | B | Yield handler, reply/completion correlation journal |
| ST-H5 | cross-language Actor handler `MessageContext`의 identity/correlation/deadline 의미 동일 | B | 최소 2언어 owner/client matrix와 공통 evidence schema |
| ST-I1 | single/multi-chunk payload와 moving message 모두 checksum·operation ID exactly-once | R+B | large-payload transport/runtime repair 후 size-profile fixture |
| ST-I2 | bounded concurrent Actor relocation 중 non-target control Actor traffic 지속 | R+B | control Actor + load driver; 현재 Node control request `routeNotConnected` blocker 해소 |
| ST-I3 | bounded SpotWide/Instance relocation 중 control Spot 지속, state checksum 유지 | R+B | bulk Spot fixture; 현재 relocation failure blocker 해소 |
| ST-I4 | old-route boundary message 최대 한 번, global route는 항상 target | B | frame을 해석하지 않는 opaque delivery-delay proxy |
| ST-I5 | expired/duplicate/loop input은 handler 중복 없이 bounded terminal | B | duplicate/expiry/loop proxy cases |
| ST-I6 | A→B→C 뒤 current global route는 C, old A/B routes는 max-once·expiry 뒤 미처리 | B | 3-node relocation topology와 delayed old-route proxy |

ST-G2/G6/G2와 관련된 설정 surface도 이미 계약에 있습니다. 4언어 모두 activation concurrency, relocation readiness, application-job-queue limit/status를 정의합니다. 따라서 “E2E 전용 capacity API”를 만들면 안 됩니다. Harness가 public configuration으로 작은 한계를 설정하고 public terminal/handler evidence만 판단해야 합니다.

## 최소 구현 순서

1. **R0 — same-RID replacement admission 교정**
   - SF-C3을 먼저 고칩니다. SF-F9와 multi-role replacement의 신뢰 가능한 기반이 됩니다.

2. **H1 — Config 6 stateful-store base fixture**
   - 분리된 Location/Relocation Store, Instance Spot, periodic timer, initialization/capture/restore/request gates, process pause/kill/restart, public evidence endpoint.
   - 해제: SF-B3, F4, F5, F7, F8, F10, F11.  
   - F2/F3도 이 fixture에 장기 capture와 Relocation Store 장애 variant만 추가하면 됩니다.

3. **H2 — operational/multi-role/capacity fixture**
   - 1,001 object creator, page-drain client, Creating/Unavailable variant, multi-role host, concurrent create driver.
   - 해제: SF-C4, C5, C5A, F6, G1, G2.

4. **H3 — cross-language Config 6 adapter**
   - H1 typed packet/state fixture를 두 언어 process에 연결.
   - 해제: SF-F1.

5. **H4 — Config 10 actor/session base**
   - source/target/session-owner processes, bound client, opaque bidirectional network blocker, actor/spot handler gates, shared operation journal.
   - 해제: ST-E1/E1A/E1B/E1C/E2, H1–H4B의 대부분.

6. **H5 — SpotWide/PerActor extensions**
   - Yield, ApplicationSignaled, PerActor split route, live job cap, ToActor sequence.
   - 해제: ST-G1–G6 및 ST-H5의 언어별 절반.

7. **H6 — Message Follow and multi-hop**
   - packet을 읽거나 만들지 않는 delay proxy와 3-node topology.
   - 해제: ST-I4–I6, 그리고 Track F3A도 함께 가능.

8. **R1/R2/R3 후 load profile**
   - R1 large payload inline/base64 handoff 문제, R2 Actor bulk relocation 중 control-route 단절, R3 Spot relocation pre-move failure를 각각 runtime에서 해결.
   - 그 뒤 ST-I1–I3을 workload/measurement fixture로 마무리합니다.

## SF-C3 런타임 결함

문제 위치는 Node의 [`raw-service-mesh-runtime.ts`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:989) 입니다.

- 기존 same-RID peer가 liveness-ready이면 새 physical candidate를 `disconnectUnexpectedMonitorPair()`로 즉시 폐기합니다.
- replacement의 descriptor/lifecycle generation을 확인하는 `topology.admit()`까지 도달하지 못하므로, discovery가 이미 replacement lifecycle을 가리켜도 current ready peer로 승격될 수 없습니다.
- 반대로 late old process 재개를 안전하게 막을 올바른 fence는 이미 존재합니다. expected lifecycle을 검증하는 admission 경로와, admitted 뒤 previous physical pair를 정확히 제거하는 경로입니다. [`raw-service-mesh-runtime.ts`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:1250) [`service-topology-registry.ts`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-topology-registry.ts:111)

수정 방향은 다음과 같습니다.

1. discovery의 current descriptor가 old admitted peer와 다른 lifecycle generation이면, READY candidate를 버리지 말고 provisional candidate로 보존하고 Hello/admission을 보냅니다.
2. admission은 `expected { endpoint, securityIdentity, lifecycleGeneration }`와 exact equality로 replacement만 승인합니다. generation의 크기 비교는 금지합니다.
3. 승인된 replacement만 semantic current peer, liveness record, monitor state로 승격합니다.
4. 그 뒤에만 old `transportPairId + transportPairGeneration`을 정확히 종료합니다.
5. old process가 재개해 보내는 late Hello/READY는 replacement descriptor와 lifecycle이 맞지 않아 reject하고, 현재 replacement의 connection/liveness를 절대 건드리지 않아야 합니다.

이는 “같은 RID, 다른 lifecycle generation은 새 process 실행”으로 다루고, replacement는 current discovery descriptor의 RID·identity·generation fence를 사용해야 한다는 계약에 직접 부합합니다. [Transport liveness](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/29-transport-liveness.ko.md:199) [Reconnect](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/29-transport-liveness.ko.md:245) [SF-C3](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md:175)

파일은 수정하지 않았고, 기존 untracked 변경도 건드리지 않았습니다.


