# kotlin 조사 — (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다.

## 결론

**kotlin L1은 전환 대상 없음**입니다.

Kotlin 모듈은 자체 C1/C2 상태 소유자를 갖지 않으며, Java의 `ZLinkSessionActorsRuntime` 전환 결과를 `ZLinkSessionActors` 인터페이스의 suspend 확장 뒤에서 그대로 받으면 됩니다. 구체 runtime 참조는 0건이고, 인터페이스 확장만 존재합니다: [ZLinkFrameworkExtensions.kt:98](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98).

## 조사 근거

검색 범위:

- `zlink-framework-kotlin`
- `zlink-http-client-kotlin`
- build 산출물 제외 전체 35파일
- Kotlin 31파일, 5,816줄
  - 운영 코드 18파일, 2,258줄
  - test 11파일, contractTest 1파일, integrationTest 1파일

검색 패턴:

- `synchronized`, `@Synchronized`, `wait/notify`
- `Mutex`, `withLock`, `Semaphore`
- `@Volatile`, `volatile`, `Atomic*`
- `ReentrantLock`, `ReadWriteLock`, `StampedLock`
- `Concurrent*`, `CopyOnWrite*`, `Collections.synchronized*`
- mutable collection 생성·필드
- `Channel`, `MutableStateFlow`, `MutableSharedFlow`
- 모든 `var`/`lateinit var`, `object`/`companion`
- `CoroutineScope`, `Job`, `SupervisorJob`, `launch`, `future`, `callbackFlow`, `CompletionStage`

운영 코드에는 `synchronized`·`Mutex`·`@Volatile`·JVM lock/semaphore·mutable collection 소유자가 모두 0건입니다. 발견된 동시성 장치는 다음과 같지만 전환 후보는 아닙니다.

| 발견 항목 | 판정 |
|---|---|
| [ZLinkStateLane.kt:24](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:24)의 queue·atomic flags | 전환 대상이 아니라 이미 구현된 lane primitive입니다. 운영 사용자는 0건이고 test에서만 참조됩니다. |
| [ZLinkOneWayCalls.kt:100](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:100)의 `AtomicBoolean` | 단일 사용 여부만 보장하는 기존 C3 플래그입니다. 컬렉션·필드 교차 불변식이 없습니다. |
| [ZLinkPublisherFlowBridge.kt:18](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:18)의 `AtomicReference`와 `Channel` | 각 Flow 수집 시 생성되는 호출 지역 bridge입니다. 컴포넌트 장기 상태나 lane 전환 대상이 아닙니다. |
| [ZLinkSuspendingHandlers.kt:346](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkSuspendingHandlers.kt:346)의 `bridgeScope` | coroutine 실행용 전역 scope일 뿐 Kotlin이 관리하는 collection/field 상태 기계는 없습니다. |

추가로 `ZLinkOneWayCalls.kt`에 builder adapter용 `private var call`이 6개 있지만 잠금으로 보호되는 공유 상태가 아니며, 요청 단위 fluent wrapper입니다. 이를 thread-safe라고 판정한 것은 아니지만 L1의 기존 gate→lane 전환 후보에는 해당하지 않습니다.

테스트에서 발견된 `@Volatile`, `AtomicInteger/Reference`, mutable collection은 test fixture·관측값뿐이며 운영 상태 소유자로 계수하지 않았습니다.

소요 시간은 **약 3분**입니다. 파일 수정, 빌드·테스트, Git 명령은 실행하지 않았습니다. 따라서 지정 브랜치는 별도로 확인하지 않고 제공된 checkout의 정적 소스를 기준으로 조사했습니다.
