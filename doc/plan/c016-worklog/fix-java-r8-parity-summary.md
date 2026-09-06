# Java/Kotlin R8 parity 수정 결과

감독이 F-R8-14·15·16의 수정 범위와 검증 결과를 확인하기 위한 기록이다.
지정된 구현 변경은 적용했지만 **전체 gate는 통과하지 못했다**. Kotlin에서 owner turn 밖의
Yield 오류 kind를 검사하는 회귀 테스트 2건이 실패한다. 최초 R8 진단에 없던 공통 execution
guard의 오류 분류 결함에 대해 B 승인을 요청했으며, 해당 추가 변경은 아직 적용하지 않았다.

`main`에서 작업했으며 commit·push는 수행하지 않았다. 기존 bindings 변경과 다른 작업 기록을
보존했다. `framework/languages/java/AGENTS.md`는 현재 저장소에 없어 루트 및
`framework/AGENTS.md`를 적용했다. Runtime 변경은 Java 트리 안에 한정했다.

| 원인 | 적용 결과 | 소유 계층·계약 | 분류 |
|---|---|---|---|
| F-R8-15 | Create/GetOrCreate의 중복 Mesh·request·timeout과 재제출을 `ZLinkFrameworkException(INVALID_OPERATION)`으로 거부한다. 최초 옵션과 제출 횟수를 보존한다. | Framework의 Java `ActorCreationCall`; Actor 모델 §6.2, Java actors interface:272 | B — 기존 결함 |
| F-R8-16 | Kotlin의 두 Actor creation wrapper에서 `KotlinSingleUse` 필드와 terminal CAS 호출을 삭제했다. Java가 제출 상태와 제출 전 거부를 소유하고 Kotlin은 suspend 완료를 제공한다. | Java `ActorCreationCall` 및 execution context; Actor 모델 §6.2, Submit §2, Kotlin actors interface:20·27 | B — 기존 결함; 공통 guard 추가 수정 승인 대기 |
| F-R8-14 | 공식 Redis provider가 encoded blob을 `64 MiB + 23 bytes`까지 수락한다. 상한을 1 byte 넘으면 Redis command 획득 전에 거부한다. | Framework Redis provider의 입력 검증; Relocation Store Redis §3·§9 | B — 기존 결함 |

수정 전/후 규칙 수: F-R8-15의 Java/Kotlin 오류 분류 2종 → 1종;
F-R8-16의 Actor creation call별 제출 상태 소유자 2개 → 1개;
F-R8-14의 provider 상한 검사 1개 → 1개이며 비교값을 encoded 입력의 계약에 맞췄다.
Kotlin의 다른 operation이 사용하는 `KotlinSingleUse`는 수정 대상이 아니다.

교차언어 대조: C++ `actor_client.cpp:103·156`과 .NET
`ZLinkActorManagerService.cs:732·743`은 중복 옵션·재제출을 `InvalidOperation`으로 분류한다.
.NET `ZLinkActorManagerService.cs:768`은 Yield 문맥 확인 뒤 단일 제출 소유자를 호출한다.
.NET `ZLinkRedisRelocationStore.cs:18·339`는 encoded 상한을 이미 적용한다.
Kotlin은 Java call과 Java Redis provider를 공유하므로 별도 오류 변환·상한 검사를 추가하지 않는다.
Node의 중복 옵션 및 Redis 상한 불일치는 이번 Java/Kotlin 수정 범위에 포함하지 않았다.

[R8 진단](spec-review/R8-fw-language-projections-summary.md)의 F-R8-14·15·16에 인용된 구현과
공통·언어별 계약 위치를 다시 확인했다. 현재 C++ Redis provider는
`extensions/framework-locations-redis/include/zlink/locations/redis.hpp:970–972`에 이미
`+23` 상한이 있으므로 진단 보고서 당시의 C++ 결함 설명과 현재 코드는 다르다.

변경 파일은 원인별로 다음과 같이 분리된다. F-R8-16의 Kotlin 회귀는 F-R8-15의 Java 오류
분류 정합을 전제로 한다.

적용한 원인별 diff와 새 테스트는 `/tmp/zlink-java-r8-f15.patch`,
`/tmp/zlink-java-r8-f16.patch`, `/tmp/zlink-java-r8-f14.patch`로도 분리했다.

- F-R8-15: [ZLinkActorRuntime.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java),
  [ZLinkActorCreationCallContractTest.java](../../../framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/actors/ZLinkActorCreationCallContractTest.java).
- F-R8-16: [ZLinkOneWayCalls.kt](../../../framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt),
  [KotlinActorCreationCallOwnershipTest.kt](../../../framework/languages/java/zlink-framework-kotlin/src/test/kotlin/systems/zlink/framework/kotlin/KotlinActorCreationCallOwnershipTest.kt).
- F-R8-14: [ZLinkRedisRelocationStore.java](../../../framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationStore.java),
  [ZLinkRedisRelocationBlobBoundTest.java](../../../framework/languages/java/zlink-framework-locations-redis/src/test/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationBlobBoundTest.java).
- 결과 기록: 이 문서.

회귀는 public call을 실제 Java runtime에 연결해 검사한다. Actor 테스트의 가짜 제출기는
제출 횟수·최초 request·timeout을 관찰하고, 미완료 및 완료 뒤 재제출을 확인한다.
Kotlin은 실제 Java call의 Yield 거부 뒤 동일 wrapper로 정상 `await()`하는 경우를 검사한다.
Redis 테스트는 기존 connection 테스트와 같은 방식으로 가짜 Lettuce 연결을 주입한다.
공식 provider의 `put`이 `64 MiB`, `+1`, `+23` 입력을 그대로 복사해 전달하고 `+24` 입력은
command 획득 전에 거부하는지 확인한다. 실제 Redis 서버는 이 경계 회귀에 사용하지 않는다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 수정 전 회귀 | Java 4/4 실패, Kotlin 최초 6건 중 4건 실패, Redis 4건 중 2건 실패. 기존 오류 타입·Yield 오분류·누락된 envelope 상한을 재현했다. | `/tmp/zlink-java-r8-red.log`, `/tmp/zlink-java-r8-red-wrappers-provider.log` |
| Java 새 회귀 5회 | 매회 4건 통과. 각 실행에서 `:zlink-framework-core:test --rerun`으로 실제 재실행했다. | `/tmp/zlink-java-r8-actor-repeat-{1..5}.log` |
| Redis 새 회귀 5회 | 매회 4건 통과. 각 실행에서 `:zlink-framework-locations-redis:test --rerun`으로 실제 재실행했다. | `/tmp/zlink-java-r8-core-provider-repeat-{1..5}.log` |
| Kotlin 통과 회귀 5회 | 매회 6건 통과: 중복 옵션, 미완료·완료 후 재제출, Yield 거부 뒤 정상 제출을 Create/GetOrCreate 각각 검사했다. 아래 실패 2건은 이 통과 횟수에 포함하지 않는다. | `/tmp/zlink-java-r8-kotlin-repeat-{1..5}.log` |
| 지정 gate | 실패. Core 1,289건 중 0 실패·1 skipped, Kotlin 75건 중 2 실패. Kotlin 실패로 후속 module 실행이 중단됐다. | `/tmp/zlink-java-r8-gate.log` |
| 중단 뒤 남은 module test | Redis provider, provider abstractions, Spring starter, testkit, HTTP client, Kotlin HTTP client, stream connector test task 통과. 합계 253건 중 실패 0건, Redis live test 14건 skipped. | `/tmp/zlink-java-r8-remaining-modules.log` |
| `git diff --check` | 통과. | 명령 출력 없음 |

문서 작성 원칙 §9의 독립 2축 리뷰를 수행했다. 문서와 변경 코드·테스트 6개 파일의
원칙 준수·코드 부합에서 수정필요 finding은 없었다. 실행 횟수와 XML 합계는 작업자가
보존한 로그 및 Gradle 결과 파일로 별도 확인했다.

지정 gate 명령:

```bash
cd framework/languages/java
flock -w7200 /tmp/zlink-java-gate.lock ./gradlew test
```

남은 실패는 `KotlinActorCreationCallOwnershipTest.yieldOutsideOwnerTurnUsesInvalidOperation`
의 Create/GetOrCreate 두 경우다. 기대값은 `INVALID_OPERATION`, 실제 값은 `NOT_CONFIGURED`다.
제출 상태 보존과 오류 kind를 독립 테스트로 구분했으며 정확한 kind assertion은 유지한다.

추가 원인은 [ZLinkSuspendInvocationContext.java:160](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/handlers/ZLinkSuspendInvocationContext.java)의
`invalid()`가 모든 execution-context 거부에 `NOT_CONFIGURED`를 만드는 것이다.
소유 계층은 Framework Java execution context이며,
[Submit §2](../../../framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md)와
[Execution Gate §3·§6](../../../framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md)가
문맥 밖 Yield·같은 gate 대기의 `InvalidOperation`을 요구한다.
.NET [ZLinkApplicationExecutionContext.cs:43·134](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkApplicationExecutionContext.cs)의
`RequireYieldTurn`·`SameGate`도 이 kind를 사용한다. 분류는 B(기존 결함)다.

검토용 추가 diff는 `/tmp/zlink-java-r8-yield-guard.patch`에 있다. 공통 `invalid()`의 kind와
기존 `ZLinkExecutionGuardTest.java:62`, `DefaultZLinkWorkerCallTest.java:60`의 잘못된 기대값을
계약의 `INVALID_OPERATION`으로 정정한다. 원인별로 분리된 이 diff는 아직 적용하지 않았다.
루트 `AGENTS.md` §3의 “감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다”에 따라
최초 진단에 없던 이 수정의 승인을 요청했다. 승인 뒤 해당 guard 및 Kotlin 전체 회귀를
검증하고, 새 테스트 전체 5회와 지정 gate의 0 failures를 확인해야 작업이 완료된다.

문서 수정 대기 사항: Java `languages/java/interfaces/location-maintenance.ko.md:238`과 Kotlin
`languages/kotlin/interfaces/location-maintenance.ko.md:25`의 “Blob 하나는 최대 64 MiB”는 공통
Redis Store §3의 encoded 입력 상한과 다르다. 문서 경로는
`framework/doc/framework/common/spec/server/` 아래이며 이번 작업의 수정 금지 범위다.
감독이 별도 문서 작업에서 공통 §3을 참조하도록 정리해야 한다.
