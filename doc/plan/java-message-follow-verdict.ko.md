# java Message Follow 수렴 판정 보고서 (B4)

> 작성: codex sol medium 조사, 검수: Claude 감독관 (2026-08-30). 판정: (a) 채택 — raw forward를 기존 MF 스택/단일 owner로 통합(중대형). 구현은 B5 커밋 후 착수.

## 판정

**[H] NOT-CLEAN — (a) 채택.** 다만 정확한 판정은 “특정 Java MF API를 반드시 호출해야 한다”가 아니라, **양수 `MessageFollowDuration`에서는 스펙이 정의한 Message Follow 의미 전체를 구현해야 한다**입니다.

Java의 `relocationActorForwards + timer`도 조건을 모두 만족하면 Message Follow 구현이 될 수 있습니다. 현재 구현은 일부 relay 기능은 있지만 commit 기준 수명, 공개 구성값, command 50 cache invalidation, typed stale 결과가 빠진 별도 축이라 계약을 충족하지 못합니다.

## 1. 스펙 판정

### 요구되는 동작

Routing은 commit 뒤 이전 owner의 행동을 직접 규정합니다.

> “이전 owner는 commit된 source→target Message Follow route가 있을 때만 같은 operation을 current owner로 relay한다.”

또한 exact fence, 최대 8홉, 무상한 route queue, original operation/payload/reply route 보존, 만료·loop·generation mismatch 결과까지 정합니다.  
[08-routing.ko.md:192](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:192), [08-routing.ko.md:199](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:199), [08-routing.ko.md:209](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:209)

가장 결정적인 조문은 relocation spec입니다.

> “이 경로가 없으면 이동한 Actor에 연결된 session은 이동 자체가 성공해도 조용히 끊긴다. 따라서 Message Follow는 선택적인 성능 최적화가 아니다.”

[04-relocation-flow.ko.md:584](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:584), [04-relocation-flow.ko.md:593](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:593)

Source도 owner 변경 전에는 temporary queue, 변경 뒤에는 Message Follow로 전달해야 합니다.  
[04-relocation-flow.ko.md:257](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:257)

### 허용되는 예외

`MessageFollowDuration=0`은 명시적으로 Message Follow를 끕니다. 양수이면 route cache가 MF보다 최소 5초 먼저 만료돼야 합니다.  
[08-routing.ko.md:204](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:204)

만료 뒤 도착한 message가 `Unavailable`로 끝나는 것은 의도된 계약입니다.  
[05-spot-actor-membership.ko.md:832](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md:832), [02-glossary.ko.md:663](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:663)

따라서 결론은 다음과 같습니다.

- MF라는 특정 class/API 사용: 구현 재량
- commit 후 bounded source→target follow semantics: 양수 duration에서 필수
- 단순 forward table도 모든 조건을 만족하면 “대체 모델”이 아니라 MF의 한 구현
- Java의 현재 raw forward는 그 조건을 모두 만족하지 않음

## 2. Java 경로와 깨지는 시나리오

### 실제 production 경로

저장소 전체 Java 검색:

```text
rg -n "beginRemoteMove|retainMessageFollowSource|finishRemoteMoveBacklog|takeRemoteMoveBacklog" \
  framework/languages/java --glob '!**/build/**' --glob '!**/.gradle/**'
```

결과는 `src/main`의 네 method 정의와 test 호출뿐입니다. 즉 production relocation은 [ZLinkActorRuntime.java:1801](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:1801), [ZLinkActorRuntime.java:1881](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:1881), [ZLinkActorRuntime.java:2104](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2104)의 explicit MF stack을 연결하지 않습니다.

대신 다음 경로를 사용합니다.

1. Target 준비 중, 아직 commit 전 `installExpectedRelocationForward()` 실행  
   [ZLinkStandaloneActorRelocationSourceBuilder.java:979](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkStandaloneActorRelocationSourceBuilder.java:979), [ZLinkStandaloneActorRelocationSourceBuilder.java:1039](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkStandaloneActorRelocationSourceBuilder.java:1039)

2. Exact source fence→target fence를 map에 넣고 그 시점부터 retention timer 시작  
   [ZLinkJavaRawSpotNode.java:245](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:245)

3. 늦은 ingress는 이 map을 explicit MF handler보다 먼저 조회  
   [ZLinkJavaRawSpotNode.java:1892](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:1892)

4. Operation/correlation/payload/bound session을 다시 encode하고 hop을 1 증가시켜 target으로 전달  
   [ZLinkJavaRawMeshNode.java:2082](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:2082), [ZLinkJavaRawMeshNode.java:2106](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:2106)

### 시나리오 판정

| 시나리오 | 판정 |
|---|---|
| 정상 retention 만료 뒤 늦은 도착 | **계약 위반 아님.** 만료 후 `Unavailable`이 규범 결과다. |
| commit 전에 timer가 시작됨 | **계약 위반 가능.** 스펙은 duration을 commit부터 재지만 Java는 relay capture 때 설치한다. |
| public duration 변경/0 설정 | **계약 위반.** 실제 raw forward는 별도 고정 30초 값을 사용한다. |
| A→B→C 다단 이동 | Map들이 모두 살아 있으면 hop을 증가시키며 chain 가능. 하지만 수명·통지 결함 때문에 안정적으로 계약을 충족한다고 볼 수 없다. |
| sender cache 수렴 | **누락.** raw forward 성공 경로에서 command 50 통지를 만들지 않는다. |
| generation mismatch | **오류 종류 발산.** exact map miss 뒤 generic `Unavailable` terminal로 수렴한다. |

구체적인 실패 창은 다음과 같습니다.

- `t0`: Java가 commit 전 forward를 설치하고 timer를 시작
- `t0+20s`: relocation commit
- `t0+30s`: Java forward 삭제
- 스펙상 삭제 시점: `commit+30s`, 즉 `t0+50s`
- commit 직전 old route를 갱신한 sender cache는 계속 유효할 수 있으므로 `t0+32s` 도착은 스펙상 relay돼야 하지만 Java에서는 forward가 이미 없습니다.

재설치도 완전한 해결이 아닙니다. 동일 target 값이면 이전 timer의 `remove(source, target)`가 새 등록과 동등한 value까지 제거할 수 있습니다.  
[ZLinkJavaRawSpotNode.java:256](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:256), [ZLinkJavaRawSpotNode.java:277](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:277)

구성 발산도 명확합니다. Public option은 0 이상을 허용하지만, raw forward가 읽는 `ZLinkFrameworkRegistration.messageFollowDuration`은 별도 30초 field이고 setter/복사 경로가 없습니다.  
[ZLinkLocationOptions.java:95](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/locations/ZLinkLocationOptions.java:95), [ZLinkFrameworkRegistration.java:54](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/configuration/ZLinkFrameworkRegistration.java:54), [ZLinkSpotRuntime.java:2038](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRuntime.java:2038)

Raw path에는 command 50 송신도 없습니다. 반면 explicit MF path는 notification과 suppression을 구현하지만 production relocation이 source를 retain하지 않아 도달하지 않습니다. Routing은 relay 사실을 sender에게 알려 cache를 갱신하도록 요구합니다.  
[08-routing.ko.md:266](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:266), [08-routing.ko.md:271](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:271)

마지막으로 forward miss는 inbound에서 `102+1`로 답합니다. 이는 generation mismatch도 `InvalidOperation`이 아니라 generic unavailable 쪽으로 평탄화합니다.  
[ZLinkJavaRawMeshNode.java:6239](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6239)

## 3. 4언어 대조

| 언어 | production 모델 | Commit/MF 전환 | 판정 |
|---|---|---|---|
| .NET | `_sourceHoldFrames`를 handoff boundary에 포함하고 explicit `ZLinkActorMessageFollowRoute`로 전환 | `CutoverCaptureToMessageFollow` 뒤 committed authority를 확인하고 `CommitMessageFollow(duration)`에서 수명 시작 | 통합 MF 모델. [ZLinkActorHandoffState.cs:1055](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1055), [ZLinkActorHandoffState.cs:1078](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1078), [ZLinkActorHandoffState.cs:1157](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1157) |
| C++ | `actor_transfer_coordinator`가 backlog와 fenced MF route, suppression, hop/accounting을 함께 소유 | `complete_remote_actor_transfer`에서 `now + message_follow_duration`으로 활성화 | 통합 MF 모델. [spot_runtime.cpp:8764](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:8764), [actor_transfer_coordinator.cpp:303](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:303), [actor_transfer_coordinator.cpp:490](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:490) |
| Node | `ZLinkActorHandoffCoordinator.complete()`가 exact owner fence로 route를 설치하고 tail queue·operation/reply context를 보존 | `complete()` 시점에 `Date.now()+duration`, hop/operation queue와 command 50 통지 연결 | 통합 MF 모델. [actor-handoff.ts:1014](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:1014), [actor-handoff.ts:1210](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:1210), [actor-handoff.ts:1452](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:1452), [index.ts:522](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/index.ts:522) |
| Java | explicit MF stack은 존재하지만 production relocation은 별도 raw `relocationActorForwards`를 우선 사용 | Forward를 commit 전 설치하고 별도 timer로 제거. Public duration과 command 50 경로가 연결되지 않음 | **부분 MF/병렬 구현, NOT-CLEAN** |

## 4. 제안

### (a) 채택 — Java 구현 수렴

새 public API는 필요 없습니다. 권장 방향은 raw forward를 별도 “fallback 모델”로 유지하지 않고 기존 Java MF stack 또는 동일한 단일 state owner로 통합하는 것입니다.

필수 완료 조건:

- Pre-commit ingress hold/relay와 post-commit MF route를 서로 다른 phase로 소유
- MF 만료를 target authority commit 시점부터 측정
- `ZLinkLocationOptions.messageFollowDuration()` 하나만 사용
- 0이면 route를 만들지 않되 relocation은 정상 완료
- command 50 notification과 exact-fence suppression 연결
- operation ID, payload, reply route, bound-session fence 보존
- 8-hop, loop, generation mismatch 결과를 스펙대로 분류
- A→B→C와 A→B→A의 old tenure fence를 독립 유지

규모 추정은 **중대형**입니다. 기존 latent MF 코드를 재사용해도 대략 production 6–10파일, contract/unit/E2E 6–10파일, 약 700–1,500 LOC 범위가 합리적입니다. 핵심 난점은 relay 코드 자체보다 두 queue/state owner의 통합과 commit 선형화점입니다.

### (b) “두 모델 허용”은 비권장

현재 스펙은 이미 내부 자료구조에는 재량을 줍니다. 추가 문구가 필요하다면 완화가 아니라 다음 정도의 clarification만 적합합니다.

> Message Follow route의 내부 저장 형태는 구현 재량이다. Relocation forward table을 사용할 수 있으나, commit 기준 수명, exact source·target fence, operation·payload·reply-route 보존, 최대 hop, typed 만료·generation 결과와 command 50 cache invalidation을 모두 제공할 때 그 table을 Message Follow 구현으로 본다. 이 조건을 만족하지 않는 retention-only forward는 별도 허용 모델이 아니다.

### (c) 타 언어 단순화 근거 없음

Java raw forward가 우월하다는 증거는 없습니다. C++/Node/.NET도 실제 전달은 결국 forward지만, 이를 lifecycle·fence·reply·cache convergence와 묶어 MF 상태로 관리합니다. 타 언어 단순화는 최소한 commit 수명, ingress hold, command 50, multi-hop fence, exact terminal completion을 동일하게 보존한다는 설계 증명 후에만 검토해야 합니다.

빌드·테스트는 요청대로 실행하지 않았고 파일도 수정하지 않았습니다. 조사 중 다른 작업의 concurrent 변경이 C++/Java 관련 파일에 나타났으며, 위 행 번호와 판정은 그 변경이 보이는 최종 worktree를 다시 읽어 확정했습니다.


