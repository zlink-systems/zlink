# 재구성 스펙 ↔ dotnet 구현 대조 (2차, 6개 주제)

검토 기준: `22949bcedb67c01e9f4c70d7fd0cd194e817c676`
검토 범위: `framework/languages/dotnet/src/Zlink.Framework/{Contracts,Runtime}` 전반
(`Runtime/Configuration`, `Runtime/Dispatch`, `Runtime/Streams`, `Runtime/Channels`,
`Runtime/Service`, `Runtime/Locations`, `Runtime/Spots`, `Runtime/Actors`,
`Runtime/Host`, `Runtime/Timers`, `Runtime/Diagnostics`), 대상 스펙 6개 주제 전 문서의
`## N. 검증 요구` 절, `doc/plan/spec-server-reorg/spec-gap.ko.md`의 G18–G21.

도달하지 못한 범위 (정직하게 명시):

- **00-foundation**: `## 8. 검증 요구`(01-public-contract-governance)의 export/dependency
  방향 static 검사(assembly 경계, package 간 참조 그래프)는 tooling 없이 육안 확인만
  가능해 시도하지 않음. `## 7. 검증 요구`(08-layering)의 벤치마크(throughput/latency/GC/lock
  contention) 항목은 코드 검토 범위 밖.
- **01-execution**: `01-submit-and-completion`·`02-handler-turn-and-execution-gate`의
  검증 요구 다수(재진입 InvalidOperation, FIFO wakeup, dispatcher 자리 예약 시점 등
  동시성 세부 동작)는 실행 트레이스 없이 코드 구조만으로 부분 확인. `06-payload-ownership-and-codec`의
  "내부 확인 조건" 절(할당·복사 계측 필요)은 스펙이 이미 공개 표면 밖이라 명시해 스킵.
- **02-channel-transport**: `06-wire-protocol.ko.md`는 검증 요구 절이 없어 스킵. Weight
  alternation(`B, A, B, B` 정확한 순서), 1:3 수렴 비율의 select-one 알고리즘 자체 동작
  검증(단위 테스트 실행 없이 코드만 읽음)은 로직 존재만 확인, 수치 시퀀스 재현은 안 함.
- **03-spot-actor**: `04-actor-model`·`05-spot-actor-membership`·`06-spot-address-messaging`·
  `07-stage-wrapper-on-spot`·`08-routing`·`09-object-lifecycle`은 검증 요구 절 자체가 문서에
  없어(파일에 `## N. 검증 요구` 섹션 미발견) 대조하지 않음 — 04-session 대조 때 이미 다룬
  Actor 관련 R-rule(R15~R43)과 중복되는 내용이 많아 이번 회차에서는 01·02·03만 확인.
- **05-location-relocation**: 7개 문서 모두 `## N. 검증 요구` 섹션이 없음(문서 자체에
  번호 붙은 검증 요구 절이 아직 작성되지 않은 것으로 보임) — 검증 요구 대조를 하지 못함.
  G20(page cap 1,024 vs 2,048-key) 관련 수치만 확인.
- **06-observability**: `## 12. 검증 요구`(runtime-metrics)의 "모든 언어에서 같다" 비교는
  dotnet 단독 관찰로는 확인 불가 — dotnet 자체 값만 기록.
- 4개 항목(G18–G21)에 집중했고 G9–G17은 이미 `topics/04-session/gap-dotnet.md`에서 dotnet
  값이 답변됨(session 주제, 이번 6개 주제 범위 밖) — 재조사하지 않고 기존 답변을 그대로
  인용만 함.

## 주제별 대조

### 00-foundation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| ErrorKind 13개·숫자 일치(07 §9) | 일치 | `src/Zlink.Framework.Contracts/Errors/ZLinkFrameworkException.cs:46-59` | `NotFound=0`…`InternalFailure=12`, 13개 값 |
| 재시도 hint 부재(07 §9) | 일치 | `ZLinkFrameworkException.cs:18,31-42` | `RetryAdvice`는 `internal` 필드, public `Exception` 표면(`Kind`, `Message`)에 없음 |
| Typed Rejected와 ErrorKind.Rejected 구분(07 §9) | 판단 불가 | — | typed Rejected 결과 타입을 찾아 두 경로를 나란히 확인하지 못함(예산 초과) |
| Host status 하나로 판단(08-layering §7, `01-runtime-monitoring` §11과 중복) | 일치 | `src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:147-157` | `ZLinkFrameworkRuntimeStatus`에 `State`·`IsReady`·`AcceptingWork`·`RelocationResult`·`TerminationResult`·`SafeToShutdown` 존재 |
| Public status에 endpoint/descriptor revision/owner lease/claim/reservation/native handle 없음(08 §7) | 일치 | `ZLinkDrainContracts.cs:147-157` | 필드 목록에 해당 항목 없음(정적 확인) |

### 01-execution

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Send timeout 0/음수/무한대/상한초과 거부(01 §16) | 일치 | `src/Zlink.Framework/Runtime/Configuration/ZLinkSocketConfigs.cs:71-83` | `timeout <= TimeSpan.Zero` 거부(0·음수·`-1ms` infinite 모두 포함), `int.MaxValue` ms 초과 거부 |
| DeliveryIndex 매 콜백 +1, ScheduledIndex 비감소, SkippedTicks 공식(04 §8) | 일치 | `src/Zlink.Framework/Runtime/Timers/ZLinkTimer.cs:434-479` | `SelectScheduledIndex`+`scheduledIndex-lastScheduledIndex-1` 공식 확인 |
| Overrun policy 3종(`SkipLateTicks`/`CatchUpBounded`/`DelayNextTick`)·`MaxCatchUpTicks`(04 §8) | 일치 | `src/Zlink.Framework/Contracts/Timers/IZLinkTimer.cs:13-24` | 이름·기본값(`MaxCatchUpTicks=1`) 일치 |
| 80% pause·60% resume 기본값(05 §10) | 일치 | `src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:8-9` | `ConfiguredPauseThresholdPercent=80`, `ConfiguredResumeThresholdPercent=60` (설정 가능, pause 1~100·resume 0~99·resume<pause 검증 `:94-118`) |
| Core profile·Application job queue profile 기본값 각각 `Balanced`(05 §10) | 판단 불가 | — | `Balanced` 리터럴을 가진 profile enum을 예산 내 특정하지 못함 |
| 형식 오류 입력이 handler에 도달하지 않음, 응답 대기 호출은 `ProtocolError`(05 §10) | 일치(부분) | `src/Zlink.Framework/Runtime/Streams/ZLinkStreamSessionRuntime.cs:562-575`(handler 예외/오류를 `ReplyErrorAsync`로 매핑, session 대조에서 이미 확인) | 04-session 대조(R8)에서 이미 확인한 것과 동일 경로 재확인 |

### 02-channel-transport

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Weight 0/기본 100/상한 10000, `-1`·`10001` 거부(01 §13, 03 §9) | 일치 | `src/Zlink.Framework/Runtime/Configuration/ZLinkSocketConfigs.cs:61-91`; 호출부 `Runtime/Configuration/Builders/ZLinkChannelBuilders.cs:75-78`(Server weight), `Builders/ZLinkSpotNodeBuilders.cs:89-92,720-723`(placement/channel weight) | 세 곳(Channel, ClientServer Server, Placement) 모두 같은 `ValidatePeerWeight` 공유 — 단일 검사 |
| RouteMesh·ClientServer 5초 probe(05 §10) | 일치 | `src/Zlink.Framework/Runtime/Service/ZLinkServiceLiveness.cs:7-8`(RouteMesh, `ProbeInterval=5s`); `Runtime/Channels/ZLinkClientServerServerIdentity.cs:12`(ClientServer, `ProbeInterval=5s`) | |
| Half-open 15초 not-ready(05 §10) | 일치 | `ZLinkServiceLiveness.cs:8`(`PeerTimeout=15s`); `ZLinkClientServerServerIdentity.cs:13`(`PeerDeadline=15s`) | |
| Fanout publisher 15초 record timeout(05 §10, `01-runtime-monitoring` §11과 중복) | 일치 | `src/Zlink.Framework/Runtime/Channels/ZLinkFanoutLivenessProtocol.cs:8`(`InboundTimeout=15s`) | |
| 수신 상한 3축(count·byte·경과시간)이 스펙 미정, "무한 독점 없음"만 판정 가능(05 §10) | 스펙 미정(코드가 값 선택) | `src/Zlink.Framework/Runtime/Dispatch/ZLinkReceiveBatchBudget.cs:11-13,15-22` | dotnet 값: `MaximumRecords=64`, `MaximumBytes=4MiB`, `MaximumMilliseconds=1`. G18 참고 |
| Automatic RID: prefix + lowercase canonical UUID v4(04 §8, 03-mesh-node §10과 중복) | 일치 | `src/Zlink.Framework/Runtime/Backend/DotNet/Adapters/ZLinkDotNetBackendRuntimeContext.cs:128`(`Guid.NewGuid()`); `.NET` 기본 `Guid.ToString()`은 lowercase 하이픈 canonical | prefix 결합부는 별도 확인 못함(판단 보류 표시 안 하고 상위 근거만 기록) |
| Entry Spot ID: 같은 diagnostic prefix + 별도 UUID v4(04 §8) | 일치 | `src/Zlink.Framework/Runtime/Spots/ZLinkSpotId.cs:68` | `$"{diagnosticPrefix}-entry-{Guid.NewGuid():D}"` |
| Server에 Client 대상 신규 호출 public API 없음(03 §9) | 일치(구조적) | `src/Zlink.Framework/Contracts/Configuration/MeshNodeBuilders.cs` 및 Server-role contract에 client 방향 send API 없음(session 대조 R7 근거와 동일 registry 구조) | 전수 API grep까지는 못함 |
| Manual과 automatic 모두 Client만 connection 시작(03 §9) | 판단 불가 | — | discovery 두 경로를 나란히 추적하지 못함 |
| ClientServer connection snapshot이 public monitoring 표면에 없음(G21 근거) | 불일치(스펙 전제와 어긋남 — 상세는 G21) | `Runtime/Channels/ZLinkClientServerClientRuntime.cs:291,755,1683`(모두 `internal`); `Contracts/Configuration/` 전체에 `ConnectionSnapshot` 타입 없음 | |

### 03-spot-actor (01–03만 검증 요구 절 존재)

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Actor 업무 payload가 Entry·User Spot callback을 거치지 않고 Actor queue에 직접 제출(01 §9, 02 §8.5) | 일치 | `src/Zlink.Framework/Runtime/Streams/ZLinkStreamSessionRuntime.cs:533-548`(session 대조 R3와 동일 경로); Actor mailbox 별도 `Runtime/Host/ZLinkActorDispatchMailbox.cs:46` | session 대조에서 이미 확인한 것 재확인 |
| Actor join/leave만 Spot control claim으로 전달(02 §8.5) | 판단 불가 | — | control-claim 채널과 일반 Actor payload 채널을 나란히 대조하지 못함 |
| Placement weight 0/100/10000/-1/10001(03-mesh-node §10) | 일치 | `Runtime/Configuration/Builders/ZLinkSpotNodeBuilders.cs:89-92` | 02-channel-transport와 동일 `ValidatePeerWeight` 재사용 |
| 같은 process 중복 MeshName startup 실패(03-mesh-node §10) | 일치 | `src/Zlink.Framework/Runtime/Configuration/ZLinkFrameworkRegistrationValidator.cs`(MeshName 중복 검사 — session 대조 R14/G6 근거와 동일 파일 계열) | 정확한 줄 번호는 이번 회차에서 재확인 못함(session 대조 R1 근거 인용) |
| Automatic RID active conflict 시 두 번째 claim 없이 startup configuration error(03-mesh-node §10) | 판단 불가 | — | conflict 발생 경로를 직접 추적하지 못함 |
| Actor 생성 상태 diagram 세번째 leaf 이름(G19) | — (G 항목 참고) | `src/Zlink.Framework/Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:86,117,1100,1633` | `ActivationFailed` bool 필드 — "Failed" 계열 이름 사용, "Aborted" 문자열은 미발견 |

### 05-location-relocation

검증 요구 절이 7개 문서 어디에도 없어 표를 만들지 못함. G20 수치만 확인:

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Page item 상한(G20, 21 §6.4 "1,024가 아니다" vs 22 "2,048-key가 아니다") | 스펙 미정 항목의 사실관계 확인 | `src/Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:23`(`MaxInventoryPageEntries=1024`); `Runtime/Locations/ZLinkInMemoryProviderLocationStore.cs:260`(condition+mutation key 합계 `>2048`이면 거부); `ZLinkInMemoryLocationStore.cs:537`(`ObjectCapabilities.Count>1024`) | dotnet에는 **두 값 모두 실재** — `1,024`는 inventory page entry 상한, `2,048`은 트랜잭션 조건+변경 key 개수 상한. 서로 다른 메커니즘이라 21과 22가 가리키는 대상이 다르면 모순이 아닐 수 있음(판정은 하지 않음) |

### 06-observability

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| 계기 label에 topic/Actor ID/Spot ID/RID/endpoint/correlation ID/flow ID 없음(02 §12) | 일치(dotnet 단독 확인) | `src/Zlink.Framework/Runtime/Diagnostics/ZLinkRuntimeMetrics.cs:283,291,400,538,552,1107,1121,1141,1201` | grep한 모든 `TagList` 생성부에 `mesh_name`·`spot_kind` 등만 존재, 위 7개 값 문자열 리터럴 미발견(부재 증거) |
| Mailbox·Spot·Actor queue·turn 단위 metric 없음(02 §12) | 일치 | `ZLinkRuntimeMetrics.cs` 전체에 `mailbox`/`queue_depth`/`QueueDepth` 리터럴 없음 | 부재 증거 |
| Public Framework interface에 exporter/reader/storage/bucket/metric event DTO 없음(02 §12) | 판단 불가 | — | Contracts 전체 grep까지는 예산 내 하지 못함 |
| `event_id`/`phase`/`surface` 등 닫힌 값 집합 일치(03 §7) | 판단 불가 | — | message-flow trace 구현 파일을 이번 회차에서 열지 못함 |

## G 항목 — 이 구현의 실제 동작

| G# | 이 구현이 하는 것 | 근거 (파일:줄) |
|---|---|---|
| G9–G17 | session 주제 항목 — 이미 `doc/plan/spec-server-reorg/topics/04-session/gap-dotnet.md`의 G1–G8 및 R-rule 표에서 dotnet 값이 답변됨(예: G9=SpotWide 직렬화 dotnet 채택, G14=envelope에 binding generation만 존재 등). 6개 주제(00/01/02/03/05/06) 범위 밖이라 이번 회차에서 재조사하지 않음 | `doc/plan/spec-server-reorg/topics/04-session/gap-dotnet.md`(G1–G8, R14/R21/R47/R48/R49 행) |
| G18 | 수신 회전 3축 상한을 `records>=64 OR bytes>=4MiB OR elapsed>=1ms`로 구현. 첫 record는 byte 상한을 넘어도 admit(분할 안 함) | `src/Zlink.Framework/Runtime/Dispatch/ZLinkReceiveBatchBudget.cs:11-32` |
| G19 | Actor 활성화 실패를 나타내는 내부 상태 이름이 `ActivationFailed`(bool 플래그) — "Aborted"라는 이름의 별도 상태·타입은 코드에서 발견되지 않음 | `src/Zlink.Framework/Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:86,117,1100,1633` |
| G20 | 두 개의 독립된 1,024/2,048 상한이 모두 실재. `MaxInventoryPageEntries=1024`(inventory page 항목 수 상한)와 store 트랜잭션의 condition+mutation key 합계 `>2048` 거부(개별 key byte 길이는 `1..1024` UTF-8 별도 제한)는 서로 다른 축 | `src/Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:23-24`; `Runtime/Locations/ZLinkInMemoryProviderLocationStore.cs:260,311`; `ZLinkInMemoryLocationStore.cs:537`(별도로 `ObjectCapabilities.Count>1024`) |
| G21 | ClientServer connection별 상태를 나타내는 `ZLinkClientServerConnectionSnapshot`이 `internal` record이며, `Zlink.Framework/Contracts/` 전체에 이를 노출하는 public 타입·API가 없음 — "connection projection" 성격의 공개 계약이 dotnet에 존재하지 않음(부재 증거) | `src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:291,755,1683`; `src/Zlink.Framework/Contracts/Configuration/`(grep 결과 `ConnectionSnapshot` 0건) |

## 요약

- 불일치 1건: G21 — dotnet에 public connection projection API가 없음(스펙 45 §4가 전제하는 표면 부재 확인)
- 스펙 미정 2건:
  - `05-application-job-queue-and-backpressure` §10의 수신 상한 3축 — dotnet 값은 count 64·byte 4MiB·경과 1ms(G18과 동일 메커니즘)
  - G20 — 21 §6.4·22가 가리키는 대상이 서로 다른 메커니즘(1,024 page entry cap과 2,048 key cap)일 가능성이 확인됨. 판정은 4언어 대조 이후로 보류
- 판단 불가 다수: Typed Rejected 구분, Core/Application job queue profile 기본값 `Balanced` 리터럴 위치, RID prefix 결합부, discovery 경로 대조, Actor conflict 경로, message-flow trace 닫힌 값 집합, public metric DTO 전수 확인 등 — 위 "도달하지 못한 범위"에 상세 사유 기재
