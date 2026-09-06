# Java provider creation terminal 저장 — F-R6-7

## 결과와 판정

Java provider repository가 `Created`·`Rejected`·`Failed` terminal을 authority/capacity 전이와 같은 조건부 쓰기로 저장한다. 같은 operation은 repository를 새로 구성한 뒤에도 저장된 envelope를 읽는다. Terminal 인자가 없는 recovery cleanup abort는 terminal을 만들지 않는다.

- 소유 계층: Framework의 `ZLinkProviderAuthorityRepository`가 생성 결과의 저장·조회와 authority/capacity 게시를 소유한다. Provider SPI는 opaque 값의 원자적 저장과 TTL을 소유한다.
- Spec 조항: [membership §2](../../../framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md), [Actor §8.1·§9](../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md), [Location runtime §7](../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md), [Location Store §3·§4](../../../framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md).
- 교차언어 대조: .NET·Node는 terminal을 authority/capacity와 같은 조건부 쓰기로 게시한다. Java in-memory authority store도 terminal을 보존한다. Kotlin은 Java core를 공유하므로 이 수정이 그대로 적용된다.
- 변경 분류: **B — 기존 결함**. D-105에서 감독자가 확인한 F-R6-7과 이번 작업 지시의 구현 승인을 적용했다.
- 수정 전/후 규칙 수: 생성 완료 batch를 구성하는 위치 **2 → 1**(`commit`·`abort`의 중복 쓰기 → `writeCreationTransition`). 저장소 종류에 따라 terminal을 보존하거나 버리던 동작 **2 → 1**. Recovery cleanup의 terminal 없는 계약은 유지한다.

## 계약과 원인

Membership §2 step 5·6·7과 terminal record 문단은 다음을 요구한다.

- Created는 Ready authority와 active capacity 전환을 함께 게시한다.
- Rejected와 Failed는 Creating authority 및 reserved capacity를 정리하면서 terminal을 게시한다.
- Recovery cleanup abort는 terminal record를 만들지 않는다.
- 식별자는 source Node RID, lifecycle generation, 128-bit OperationId의 조합이다.
- `creation-operation-terminal-v1` semantic envelope와 SHA-256을 최초 deadline 뒤 5분까지 보존한다.

수정 전 원인 위치는 다음과 같다. 경로는 `framework/languages/java/` 기준이다.

| 파일과 수정 전 위치 | 원인 |
|---|---|
| `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderAuthorityRepository.java:609` | `commit`이 terminal을 받지만 authority/capacity만 쓴다. |
| 같은 파일 `:677`, `:723`, `:736` | `abort`에는 terminal 저장이 없고, `reject`는 해당 abort로 위임하며, `readCreationTerminal`은 항상 Missing이다. |
| `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderLocationRepository.java:190` | terminal을 받는 abort overload가 terminal을 버리고 cleanup abort를 호출한다. |
| `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorCreationCoordinator.java:353` | 저장소 조회 결과에 의존하는 caller이므로 provider 경로에서는 이전 operation의 결과를 복원하지 못한다. 이 파일은 변경하지 않았다. |

Actor §8.1·§9, MeshNode §7.1, host relocation §14 step 1의 최신 문장도 읽었다. 이번 원인의 구현 범위는 target의 creation terminal 저장이며 transport replay나 mesh admission 동작의 변경은 필요하지 않다.

## 수정과 대안 비교

기존 commit/abort 각각에 terminal 쓰기를 추가하는 대안과 공통 batch 소유자로 통합하는 대안을 비교했다. 후자를 적용해 조건, capacity 변경, authority 변경, terminal 게시가 `ZLinkProviderAuthorityRepository.java:697`의 `writeCreationTransition` 한 곳에서 결정되도록 했다.

Terminal을 포함하는 완료는 authority와 capacity의 version 조건, terminal metadata의 Missing 조건을 검사하고 한 번의 `provider.write`로 다음을 게시한다.

1. Authority의 Ready 전환 또는 Creating authority 삭제.
2. Capacity의 pending → active 전환 또는 pending 반환.
3. Identity, reservation, state, SHA-256, expiry를 담은 terminal metadata.
4. 원문 semantic envelope bytes.

Metadata와 envelope를 한 value로 합치는 대안은 최대 envelope 크기에서 provider의 1 MiB value 상한을 넘는다. .NET처럼 항상 별도 value로 저장하고 같은 batch와 TTL을 적용했다. 최대 1 MiB envelope도 허용되며 크기에 따른 별도 분기를 두지 않는다. Metadata encoding은 repository 내부에 있고 semantic envelope는 다시 encode하지 않는다.

`readCreationTerminal`(`:764`)은 metadata와 envelope를 읽고 identity triple 및 저장된 expiry를 검사한 뒤 SHA-256을 검증한다. Identity 불일치, 만료, 누락은 Missing이고 checksum 손상은 명시적 오류다. Missing 조건 때문에 다른 reservation이 보존 중인 terminal을 덮어쓰지 못한다. Terminal 없는 commit/cleanup abort는 기존 계약대로 동작한다.

Expiry는 caller가 전달한 `expiresAt`을 그대로 보관한다. 기존 coordinator가 이를 original deadline + 5분으로 만든다(`ZLinkActorCreationCoordinator.java:658`). TTL 계산과 만료 판정은 Location Store §3·Location runtime §7에 따라 provider의 `StoreNow`를 사용한다. Local wall clock으로 경과 시간을 재지 않으며 새 timer·retry·deadline owner를 추가하지 않는다. 테스트 시간은 수동으로 증가시키는 elapsed-nanoseconds clock으로 제어한다.

교차언어 근거:

- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:833`: 조건 목록과 terminal metadata/payload·capacity·authority를 한 write로 게시한다. 두 terminal value에 같은 TTL을 적용한다.
- Node `framework/languages/node/packages/framework/src/runtime/locations/location-store-repository.ts:1423`: authority/capacity mutation과 terminal put을 같은 conditional write에 넣는다.
- Java `ZLinkInMemoryAuthorityStore.java:426`, `:470`, `:541`: 각 완료 전이에서 terminal을 보관한다. Java provider만 이 동작을 빠뜨렸던 기존 결함이다.
- Kotlin `framework/languages/java/zlink-framework-kotlin/build.gradle.kts:14`: `zlink-framework-core` 의존으로 Java runtime을 공유한다. Kotlin 파일 변경은 없다.

## 회귀 테스트와 gate

새 파일: [ZLinkProviderCreationTerminalTest.java](../../../framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderCreationTerminalTest.java).

Opaque SPI를 구현하는 recording fake provider가 실제 `ZLinkInMemoryProviderLocationStore`에 위임한다. Framework in-memory authority store를 대역으로 사용하지 않는다. Fake는 조건부 write 충돌과 provider value의 1 MiB 상한도 검사한다.

26개 실행 case가 다음을 검증한다.

- Created/Rejected/Failed의 단일 batch 게시, 정확한 authority/capacity 전이, 새 repository에서의 envelope·hash·expiry·reservation 복원.
- Conditional conflict에서 상태와 terminal 모두 미게시, 기존 terminal 덮어쓰기 방지.
- RID/lifecycle/OperationId high/low 각각의 불일치가 Missing이며 high-bit opaque token도 정상 처리.
- 게시가 늦어져도 original deadline + 5분 경계를 유지하며 metadata와 payload의 TTL이 동일하고, 만료 시 Missing.
- Recovery abort와 Failed abort의 차이, checksum·reservation·state·만료 입력 검증.
- 세 terminal 상태의 최대 1 MiB envelope 저장, 손상된 envelope의 무결성 오류, payload 누락의 Missing.

| 검증 | 결과 | 보존 로그 |
|---|---|---|
| 수정 전 최초 회귀 15건 | 12 실패, 3 통과. Terminal 누락과 덮어쓰기 재현 | `/tmp/zlink-java-r6-terminal-before.log`, `.xml` |
| 단일 value 방식의 최대 envelope 3건 | 3 실패. Provider 1 MiB 상한 위반 재현 후 metadata/payload 분리 | `/tmp/zlink-java-r6-terminal-size-before.log` |
| 신규 + 기존 authority repository focused test | 26 + 14건 통과 | `/tmp/zlink-java-r6-terminal-focused.log` |
| 신규 테스트 5회 | 매회 26건, 실패/오류/skip 0. 총 130건 통과 | `/tmp/zlink-java-r6-terminal-repeat-{1..5}.log`, `.xml` |
| 지정 전체 gate | **실패: Spring 테스트 2건**. Core 1,279건(실패 0, skip 1), Kotlin 67건(실패 0), Redis 27건(실패 0, skip 14), codec 3건(실패 0). Spring 39건 중 2 실패로 이후 task 중단 | `/tmp/zlink-java-r6-terminal-gate.log` |
| 수정 전 코드로 Spring 실패 격리 | 같은 두 테스트가 동일한 assertion으로 실패 | `/tmp/zlink-java-r6-terminal-baseline.log`, `/tmp/zlink-java-r6-terminal-baseline/results/` |
| 변경 diff 검사 | `git diff --check` 통과 | 최종 변경 범위 확인 |

명령은 `framework/languages/java`에서 실행했다.

```bash
flock -w7200 /tmp/zlink-java-gate.lock ./gradlew :zlink-framework-core:test --tests '*ZLinkProviderCreationTerminalTest' --tests '*ZLinkProviderAuthorityRepositoryTest'
# 아래 focused 명령을 5회 실행. --rerun은 test task를 실제로 다시 실행한다.
flock -w7200 /tmp/zlink-java-gate.lock ./gradlew :zlink-framework-core:test --tests '*ZLinkProviderCreationTerminalTest' --rerun
flock -w7200 /tmp/zlink-java-gate.lock ./gradlew test
```

Sample runner는 실행하지 않았다. 전체 gate를 같은 원인으로 반복하지 않았다.

## BLOCKERS

전체 Java gate의 잔여 실패는 다음 두 건이다.

- `zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/HostTest.java:51` — `host_startsAndStops_frameworkRuntimeContext`.
- `zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/ZLinkFrameworkAutoConfigurationTest.java:722` — `autoConfigurationAppliesCustomizersBeforeRuntimeStarts`.

두 assertion 모두 fake backend 호출 목록에 `socketMonitor.onEvent`를 기대하지만 실제 목록에는 없다. 현재 `zlink-framework-testkit/src/main/java/systems/zlink/framework/testkit/FakeZLinkBackendAdapterFactory.java:1671`의 `FakeSocketMonitor`는 `recv()`만 구현한다. 테스트의 Channel 시작/종료 경로는 creation terminal 저장과 독립적이다.

독립성을 확인하기 위해 수정한 production 파일 두 개만 `HEAD`(`d89fc776c0`) 원본에서 `/tmp/zlink-java-r6-terminal-baseline/sources/`로 추출했다. 임시 Gradle init script로 baseline class를 별도 디렉터리에 컴파일하고 두 Spring 테스트의 classpath 맨 앞에 적용했다. 다른 코드는 동일하게 유지했으며 두 실패가 그대로 재현됐다. 이는 전체 clean baseline gate를 실행한 결과가 아니라 해당 원인에 대한 좁은 격리 검증이다. 기존 assertion과 testkit 구현은 수정하지 않았다.

따라서 이번 원인의 구현·회귀 검증은 완료했지만, 요청에 제시된 전체 Java gate의 0-failure 조건은 현재 checkout에서 충족되지 않는다. 감독자가 이 별도 원인을 처리해야 한다.

## 변경 파일과 원인별 diff

F-R6-7은 다음 네 파일의 단일 원인 diff로 묶을 수 있다. Commit은 하지 않았다.

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderAuthorityRepository.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderLocationRepository.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderCreationTerminalTest.java`
- `doc/plan/c016-worklog/fix-java-r6-terminal-record-summary.md`

Spring gate 실패 원인은 위 diff에 포함하지 않았다. 기존 사용자 변경, 다른 언어, Core·binding 및 보호 문서는 수정하지 않았다.
