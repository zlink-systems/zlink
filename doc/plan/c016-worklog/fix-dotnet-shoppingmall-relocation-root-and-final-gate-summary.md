# .NET ShoppingMall relocation root 수정과 최종 gate 결과

2026-09-05. ShoppingMall은 root 불일치와 종료 중 정규화 충돌을 수정한 뒤 모든 역할이
`Stopped/None`이다. **최종 gate 전체는 미완료**다. 새 Core에서도 ZoneWorld G4가
`Unavailable`을 관찰하지 못해 요청된 STOP 조건을 적용했다. 나머지 6개 sample과
unit 두 절반(1948/1948, 16/16), sample regression(157/157)은 최종 수정 후 통과했다. 이 경우 Core는 실제 request에
`NotConnected/109`를 전달했다. 이후 Framework의 durable sender와 join 완료 경계가 남는다.

증거의 기준 경로는
[`scratchpad/fix-dotnet-shoppingmall-relocation-root-and-final-gate/`](../../../scratchpad/fix-dotnet-shoppingmall-relocation-root-and-final-gate/)다.
아래 증거 파일은 이 디렉터리를 기준으로 한다. `main`에서 작업했으며 commit하지 않았다.
Core·binding·sample·다른 언어·보호 문서는 수정하지 않았다. 기존 사용자 변경은 유지했다.

## B4 원인과 수정

### Target staging과 canonical root

`diagnosis-fields/evidence/ShoppingMall/logs/workflow-a.log:212`의 relocation
`efc15fe7-e134-4cbe-bccc-63ebac640d78`에서 source generation은 **11**, staged target과
published authority generation은 모두 **12**다. 후보였던
`ZLinkFrameworkRuntimeSpotRetire.cs:61-65`의 계산은 이 관측에서 불일치 원인이 아니다.
필드 대조는 `b4-root-fields.json`에 보존했다.

수정 전 `ZLinkSpotRetireTransport.cs:1384`의 `BindCanonicalAuthorityInventoryAsync`는
source authority의 CAS version·payload를 `RecoveryPayload`로 만들어 staged envelope에
추가했다. Canonical codec은 이 로컬 정보를 전송·저장하지 않는다
(`ZLinkRelocationEnvelope.cs:233-235,727`). 따라서 publication을 다시 읽으면 이 필드는
비어 있고, 기존 prefix 검사 `ZLinkSpotRetireTransport.cs:1732-1733`가 이를 거부한다.
Generation 11→12의 전환은 기존 target-generation 비교 규칙으로 일치한다.

로컬 CAS 정보를 stage의 `SourceRecoveries`로 옮겼다. Root는 전달받은 state·timer·journal을
보관하고, CAS와 정규화의 기존 입력 경계에서만 로컬 recovery 정보를 사용한다.
`IsStagingPrefix`의 검사와 generation 거부 조건은 수정하지 않았다.
Prefix 검사에서 `RecoveryPayload` 비교를 제외하는 대안은 root 검사를 완화하므로 채택하지 않았다.

### Source의 publication 관찰

수정 전 `ReconcilePublishedAuthorityAsync:379-382`는 target이 source의 임시 root reference와
checksum을 그대로 게시한다고 가정했다. Target의 `CommitWireTargetAggregateAsync`는
자신의 staged root를 publication coordinator에 넘겨 새 immutable reference를 만든다.
Source의 임시 root 주소는 target authority 전환의 식별자가 아니다.

Source는 target descriptor·lifecycle·owner·lease, 같은 object generation과 증가한 authority
generation을 확인한다. 진행 중 publication은 같은 relocation fence로 확인하고, 이미
정규화된 authority는 기존 `IsExactNormalizedTargetAuthority`의 byte-exact 검증을 재사용한다.
Deadline·재시도 횟수·root prefix 조건은 유지했다. Provider의 global counter가 발급하는
owner generation이므로 source 관찰에서 `source + 1`로 제한하지 않는다.

### 종료 중 정규화

Root 수정 후 `final-ShoppingMall/phase-01/logs/workflow-b.log:189`에서
`steady authority normalization conflicted`가 남았다. 같은 로그 `:188`의 target은
`Draining`이다. Provider repository는 `Preserve` aggregate의 prepare와 commit에서도
신규 placement의 `Serving` 조건을 적용했다
(`ZLinkProviderLocationRepository.Authority.cs`, 수정 전 `:1093,1469`).

기존 `requireNewPlacementEligibility` 옵션을 사용해 `NewOwner` participant가 있을 때만
신규 placement eligibility를 검사한다. `Preserve`는 기존 owner·lease·version·allocation
조건을 유지한다. 정규화 완료를 위해 host의 종료 순서나 deadline을 변경하지 않았다.
최종 `final-tree-ShoppingMall`에서는 root 불일치와 정규화 충돌이 모두 사라졌다.

### Unit gate의 remote UserSpot admission

첫 unit 절반은 1944/1946이며 remote UserSpot 생성과 ClientServer liveness가 실패했다.
분리 실행에서 liveness는 통과하고 remote 생성은 다시 실패했다.
`unit-remote-create-trace.log:16-24`는 target이 Hello를 보내고 source가 Admit를 반환했는데도
같은 target이 이를 `stale_admit`로 거부하는 것을 보여 준다. Source만 admitted이며 target은
Connecting 상태에 남아 command 47 create가 완료되지 않는다(`:265-268`).

원인은 수정 전 `ZLinkMeshPeerAdmission.cs:46-47,83-84`의 물리 방향 필터다. Configured
outbound intent가 유효해도, Core가 선택한 pair의 관찰 방향이 inbound이면 같은 logical
peer의 Admit와 기존 peer를 거부했다. 이 중복 필터를 제거했다. Configured RID·endpoint,
기존 intent의 존재, admission descriptor의 lifecycle·security 검증은 유지했다. 새로운
connection·generation 상태나 reconnect 처리는 추가하지 않았다.

`Admit_on_selected_inbound_pair_matches_current_logical_outbound_intent`의 pending/admitted
두 경우는 수정 전 모두 실패했다. 수정 후 이 회귀, 기존 matcher 테스트, remote 생성과
ClientServer 전체 테스트가 47/47 통과했다. 삭제된 intent의 늦은 Admit 거부도 회귀에서
확인했다. 기존 test assertion과 fixture는 변경하지 않았다.

### Unit gate의 liveness 대기 시간 측정

Admission 수정 뒤 첫 절반은 1947/1948이었다. 유일한 실패는
`BlockingRequestHandler_DoesNotBlockClientServerLivenessControl`의 8초 liveness 관찰 대기다
(`unit-v10-main-final.log:108-116`). TRX가 기록한 전체 테스트 실행 시간은 4.145초이며,
첫 실행의 다른 liveness 실패도 3.271초다. Helper는 수정 전
`ClientServerChannelRuntimeTests.cs:992-995`에서 `DateTime.UtcNow`로 상대 timeout을 계산했다.

기존 테스트를 직접 호출하며 측정한 `unit-liveness-clock.log:23-24`에서 UTC 경과는
2241.376→7345.341 ms, `Stopwatch` 경과는 2241.394→2344.900 ms다. UTC가 약 5초
앞으로 이동했다. 이 진단 실행 자체는 통과했다(`:27`). 시스템 시각 이동의 외부 원인은
이번 작업에서 조사하지 않았다.

Helper의 상대 시간 측정을 `Stopwatch.GetElapsedTime`으로 바꿨다. 같은 8초 한도와
기존 조건·assertion을 유지한다. UTC 이동분을 더해 budget을 보정하거나 대기 시간을
늘리는 대안은 사용하지 않았다. Runtime의 liveness 정책과 timer는 수정하지 않았다.
이 변경의 소유자는 테스트의 경과 시간 측정이며, 교차언어 runtime 변경에는 해당하지 않는다.

이후 join 절반도 같은 원인의 setup timeout 2건으로 14/16이었다
(`unit-v10-join-verified.log:19-38`). `CanonicalActorJoinIngressReplyTests.cs:1104-1112`는
UTC로 2초를 계산했고, 실패한 테스트의 전체 경과 시간은 각각 0.320초와 0.753초다.
이 helper도 기존 2초 한도를 유지한 채 단조 시간으로 교정했다. Runtime terminal deadline과
수동 `TimeProvider`를 사용하는 테스트의 시간 전진·assertion은 바꾸지 않았다.


- 소유 계층: **Framework SPOT relocation staging/publication 관찰**, **Framework provider authority repository의 aggregate mutation 검증**, **Framework logical peer admission matcher**. 별도 test 수정은 상대 경과 시간 측정 helper가 소유한다.
- Spec 조항: **04-relocation-flow §2, §4.3–§4.6**의 immutable 전달 state·고정 source fence·target 전용 CAS; **05-host-relocation-flow §9, §14**의 target 준비와 Draining 중 진행 중 unit 완료; **01-location-runtime §6.1 Read와 CAS**, **02-glossary AuthorityOwnerGeneration**의 provider-issued global counter; **05-transport-liveness:239-249**의 physical pipe 선택은 Core 소유라는 경계.
- 교차언어 대조: Java `ZLinkUserSpotAggregateStagingOwner.java:474-545`는 state·timer·actor inventory·journal prefix를 비교한다. 로컬 CAS projection을 root에 혼합하는 .NET 경계를 수정했다. Java provider `ZLinkProviderAuthorityRepository.java:1428-1447`도 `Preserve`에서 owner·allocation을 유지한다. Java의 Draining 정규화 재현은 실행하지 않았다. Java `ZLinkJavaRawMeshNode.java:7007-7011`은 Core가 선택한 route의 logical identity를 Hello·Admit에 재사용한다. .NET의 monitor 방향에 따른 추가 Admit 거부를 제거했다.
- 변경 분류: **B — 기존 Framework 결함 수정**. Core completion·binding transport 변경은 없다.
- 수정 전/후 규칙 수: **6 → 2**. Canonical root 내용의 해석 2→1, authority mutation의 신규 placement 조건 적용 규칙 2→1. Source의 target 확인은 기존 authority 검증을 재사용한다. 로컬 CAS 정보는 stage 한 곳으로 옮겼다. Logical admission을 다시 제한하던 physical-direction 조건 2→0이다. 각 Test helper의 상대 timeout 규칙은 1→1이며 시간원만 교정했다.

## 변경 파일과 focused 검증

코드 경로의 기준은 `framework/languages/dotnet/`이다.

| 파일 | 변경 |
|---|---|
| `src/Zlink.Framework/Runtime/Spots/ZLinkSpotRetireTransport.cs:353,1345,2548` | Source의 target authority 관찰, canonical root와 stage 로컬 CAS 정보 분리 |
| `src/Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:1093,1472` | Preserve aggregate의 prepare·commit에서 기존 mutation eligibility 규칙 재사용 |
| `tests/Zlink.Framework.UnitTests/Runtime/RelocationRuntimeTests.cs:1001,1043` | 실제 staging binder를 통한 root reconciliation, target 고유 root와 정규화 뒤 source 관찰 회귀 |
| `tests/Zlink.Framework.UnitTests/Runtime/ProviderLocationRepositoryAuthorityTests.cs:1836` | Prepare 전·commit 전 Draining 전환에서 Preserve 완료와 NewOwner 거부 검증 |
| `src/Zlink.Framework/Runtime/Service/ZLinkMeshPeerAdmission.cs:45,81` | Core가 선택한 pair 방향으로 logical Admit를 다시 거부하던 필터 제거 |
| `tests/Zlink.Framework.UnitTests/Runtime/ZLinkMeshPeerAdmissionTests.cs:34` | Pending/admitted logical intent의 Admit 수용과 삭제된 intent 거부 회귀 |
| `tests/Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs:989` | 상대 timeout을 단조 경과 시간으로 측정; 기존 한도·assertion 유지 |
| `tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:1105` | Setup 상대 timeout을 단조 경과 시간으로 측정; 2초 한도 유지 |
| 이 문서 | 원인·검증·blocker 기록 |

| 검증 | 결과 | 증거 |
|---|---|---|
| B4 결함 재주입 | 신규 회귀 3/3 FAIL | `b4-before.log` |
| B4 수정 후 회귀와 잘못된 target generation 거부 | 4/4 PASS | `b4-focused.log` |
| RelocationRuntimeTests | 141/141 PASS | `b4-relocation.log` |
| Draining 정규화 회귀, 수정 전 | prepare와 commit 경계 2/2 FAIL | `normalization-before.log` |
| 최종 RelocationRuntimeTests + ProviderLocationRepositoryAuthorityTests | 198/198 PASS | `b4-final-focused.log`, `tests/b4-final-focused.trx` |
| Logical Admit 회귀, 수정 전 | 2/2 FAIL | `admission-before.log` |
| Matcher + remote create + ClientServer 전체, 수정 후 | 47/47 PASS | `admission-focused.log`, `tests/admission-focused.trx` |
| 상대 timeout 시계 수정 후 ClientServer 전체 | 36/36 PASS | `liveness-clock-focused.log`, `tests/liveness-clock-focused.trx` |

## 최종 gate

Sample의 정상 terminal은
`termination_changed state=Stopped outcome=Stopped reason=None`으로 확인했다.
`client` CLI는 host 역할 종료 집계에서 제외한다. 의도적인 crash 역할에는 정상 종료 결과를
붙이지 않는다. 각 실행의 `phase-*` 디렉터리는 child와 parent의 로그 덮어쓰기를 막기 위한
증거 사본이다.

| Gate | 결과 | 역할별 outcome | 증거 |
|---|---|---|---|
| ShoppingMall | exit 0, PASS | api-a, api-b, workflow-a, workflow-b 모두 Stopped/None | `final-tree-ShoppingMall/phase-01/logs/` |
| TicTacToe | exit 0, PASS | api-a, api-b, play-a, play-b 모두 Stopped/None | `final-tree-TicTacToe/phase-01/logs/` |
| Bingo | exit 0, PASS | api-a, api-b, matchmaking, play-a, play-b, session-a, session-b 모두 Stopped/None | `final-tree-Bingo/phase-01/logs/` |
| SupportChat | exit 0, PASS | api, session, support 모두 Stopped/None | `final-tree-SupportChat/phase-01/logs/` |
| DeliveryDispatch | exit 0, PASS | courier-node-1, courier-node-2, courier-session, customer-gateway, dispatch, tracking 모두 Stopped/None | `final-tree-DeliveryDispatch/phase-01/logs/` |
| GameQuest | exit 0, 정상 cleanup PASS | api-a, api-b, mission-a는 Stopped/None; mission-b는 시나리오가 SIGKILL | `final-tree-GameQuest/phase-01/logs/` |
| ZoneWorld 1회차 | exit 1, G4 FAIL; 나머지 full 시나리오 미진행 | gateway, ops, zone-node-1, zone-node-3는 Stopped/None; zone-node-2는 G4 SIGKILL | `gate-ZoneWorld/phase-01/logs/` |
| ZoneWorld 2회차 | STOP 조건으로 미실행 | — | G4 blocker |
| `bash samples/run_samples.sh` aggregate | STOP 조건으로 미실행 | — | G4 blocker |
| Unit v10 — CanonicalActorJoinIngressReplyTests 제외 | **1948/1948 PASS**, exit 0 | — | `unit-v10-main-verified.log`, `tests/unit-v10-main-verified.trx` |
| Unit v10 — CanonicalActorJoinIngressReplyTests | **16/16 PASS**, exit 0 | — | `unit-v10-join-final-clock.log`, `tests/unit-v10-join-final-clock.trx` |
| SampleRegressionTests | **157/157 PASS**, exit 0 | — | `sample-regression-final.log`, `tests/sample-regression-final.trx` |

첫 unit 실행의 실패는 위 원인 수정과 해당 절반 재검증으로 해소했다. 최종 unit의
남은 실패는 없다. 마지막 수정은 join 절반의 test helper만 대상으로 하므로 이미 통과한
나머지 절반의 결과를 유지했다.

모든 .NET 실행은 요청된 TMPDIR, `UseSharedCompilation=false`,
`MSBUILDDISABLENODEREUSE=1`, telemetry off, SHA별 NuGet cache와
`flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다. Sample Redis는 각 runner의 Docker
scope를 사용했다. Unit 명령은 `unit-gate-v10.sh`에 있으며 각 절반에
`--blame-hang --blame-hang-timeout 10m`을 적용했다.

- NuGet SHA256: `5ee5b15325b9d631ed06051bccb13dc40d0e33ee5c9cc96b6087d964b46e041d`.
- 패키지 내부·`core/build-dev/lib/libzlink.so.0.17.0` SHA256: `c5f62fb17f26f68345c58fed35009fb9b22adacdad911a4cc94f60a9bfa7aa52`.
- ShoppingMall 실제 출력의 native library도 같은 `c5f62fb1…`이다. 이전 `98f34996…` library를 사용하지 않았다.

## BLOCKERS

### ZoneWorld G4: 새 Core의 terminal 이후 Framework join deadline

ShoppingMall 수정 후 첫 G4 실행은 actor `g4-crash-0c8c4d`, flow
`01a07036-663d-7385-83f9-11fbe5ebbeeb`다. `gate-ZoneWorld/phase-01/logs/zone-node-1.log:1006`
에서 probe를 받고, target `zone-node-2.log:1037`에서 crash boundary에 도달했다.
Source `zone-node-1.log:1969`는 `DeadlineExceeded/TaskCanceledException`, `:1982`는
같은 flow의 `CrashRelocationProbeRes` 전송이다. Client의 Unavailable 조건은 충족하지 못했다.

Native terminal을 확인한 별도 focused 진단은 actor `g4-crash-681980`, flow
`01a07039-02f3-798e-bfb0-00eb5675c161`이다.
`g4-terminal/evidence/ZoneWorld/logs/zone-node-1.log:577`에서 operation
`2f5daad9593649a50000000000000020`의 admitted request가 **98.0194 ms에 NotConnected/109**를
받았다. `:580-623`에서는 같은 operation의 native 재제출이 `NotConnected/2`로 거부된다.
`:1594`의 상위 join은 deadline까지 대기한 뒤 취소되며 `:1607`에서 probe 응답을 전송한다.

`ZLinkDurableRequest.cs:30-44,50-65`는 admitted `NotConnected`와 submit `NotConnected`를
재제출한다. `ZLinkManagedMeshNode.cs:8920-8929`의 peer 부재·미admitted 검사도 submit
`NotConnected`를 반환한다. 최종 상위 대기는 `ZLinkActorRemoteJoiner.cs:1046`이다.
Java `runtime/binding/ZLinkJavaDurableRequest.java:87-134`도 동일한 request/submit 재제출
분류를 사용한다. 이번 관측에서 Core의 pair 종료 completion은 전달됐다. 남은 경계는
Framework의 고정 target lifecycle 처리와 durable operation terminal이다.

요청된 STOP 조건을 적용해 이 경계의 runtime은 수정하지 않았다. 별도 monitor 상태·poller·
재시도·deadline 우회도 추가하지 않았다. Native terminal 진단용 임시 코드는 제거했으며
`ZLinkManagedMeshNode.cs`는 최종 diff에 없다. 이 focused 진단을 ZoneWorld full 2회차로
계산하지 않는다. 이후 unit gate의 admission matcher 수정 뒤에도 G4는 요청된 STOP 상태를 유지했다.

### 별도 Core 작업: 명시적 endpoint 제거

`public-api-new-core.log`의 .NET 공개 API 재현에서 target close는 제거 13 ms 시점 뒤
15 ms 시점에 `NotConnected`로 완료됐다. 명시적 source endpoint 제거도 즉시
`NotConnected`로 끝난다. 후자의 계약은 `core/doc/spec/core/socket/README.ko.md:1147`의
`NOT_FOUND`이며, 요청에 지정된 별도 Core 작업이다. 이 재현에서 `TIMED_OUT/101`은
관찰되지 않았다. Framework에서 errno를 재분류하지 않았다.
