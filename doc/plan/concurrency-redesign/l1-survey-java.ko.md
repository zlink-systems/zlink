# L1 표본 조사 — java (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다. L1 전환 요청의 근거로 보존한다.

## 결론

Java 대응부는 별도 binding table이 아니라 `ZLinkSessionActorsRuntime` 하나에 binding·relocation·ingress·outbound FIFO가 결합된 구조다. 판정은 **C2**, 전환 난이도는 .NET 표본 대비 **약 2배**다.

### `ZLinkSessionActorsRuntime`

[ZLinkSessionActorsRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:46)

- 대응 역할: Session별 Actor binding 목록, binding generation·route, 교체 직렬화, relocation seal/route, ingress gate와 target outbound FIFO를 소유한다.
- 동기화 블록 **22개**:
  - `synchronized (sealTerminals)` **20개**.
  - `synchronized (this)` **2개**.
  - Java intrinsic monitor이므로 둘 다 **재진입 가능**하다. `recursive_mutex`나 `Lock`·`Semaphore`는 없다.
  - 보조 수단: `ConcurrentHashMap` 2개, `CopyOnWriteArrayList` 1개, outer `AtomicLong` 1개. 중첩 entry에는 `AtomicBoolean`, binding 생성 closure에는 지역 `AtomicReference` 2개가 있다.
- 보호 상태:
  - `this`: `_bindingTransitions`에 해당하는 `bindingTransitions`.
  - `sealTerminals`: `sealTerminals`, `routeTerminals`, `routeFlights`, `targetOutboundBindings`, `ingressGates`, `nextFallbackIngressSequence`, `relocationStopped`, `relocationSealTimedOut`.
  - 같은 monitor 안에서 `bound`, `bindingRoutes`, `bindingTransitions`와 중첩 `IngressGate`·`SealTerminal`·`TargetOutboundBinding/Epoch/Entry` 상태도 함께 접근한다.
  - `bound`와 `bindingRoutes`는 monitor 밖에서도 각각 조회·snapshot과 `computeIfPresent`로 변경된다([L308](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:308), [L583](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:583)).
- 여러 컬렉션 직접 접근: **9/22**. 주요 블록은 L374, L672, L729, L783, L840, L945, L1887, L1971, L2040이다. Lock 전용 helper가 내부에서 접근하는 컬렉션까지 펼치면 L1080·L1097·L1123이 추가되어 실질적으로 **12/22**다.
- 판정: **C2**. Binding install/remove가 여러 collection과 scalar fence를 함께 전이하고, ingress·outbound·route 결정 뒤 `CompletionStage` 작업이 이어진다. `bindingGenerations`만 고립하면 C3지만 혼합 클래스에서는 C2가 우선한다.
- 파급: production `src/main/java`에서 구체 타입으로 해석 가능한 생성·메서드·상수 접근은 **3파일/18지점**이다.
  - async 관용구(`CompletionStage` 반환 메서드) 안 **11/18, 약 61%**.
  - 동기 메서드 안 **7/18, 약 39%**.
  - Lane 전환에 직접 민감한 instance 생성·호출만 좁히면 **10지점**, async **3/10**이다.
  - Imports·형식 선언·중첩 `LocalActorReply` 사용까지 포함하면 production **10파일**이 이 클래스 symbol에 의존하지만, 정본 계수에서는 제외했다.
- 재진입 의심:
  - `notifyDisconnectedAll()` → 같은 공개 overload 호출: [L347](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:347).
  - `bind(ZLinkActor)` → `bindManagedAsync`: [L313](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:313).
  - Local Actor 경로의 `bindBackendRef` → `bindManagedAsync`: [L430](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:430).
  - `stopRelocationOwner` overload 재호출: [L364](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:364).
  - 실제 monitor 재진입 가능 경로: `tail.complete(null)`이 다음 replacement의 비동기 아닌 dependent continuation을 같은 스레드에서 실행한 뒤 `synchronized (this)`를 다시 획득할 수 있다([L592](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:592)).
  - `sealTerminals` monitor 안에서 `settlement.complete(...)`가 실행되는 경로도 있다([L1280](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1280), [L1550](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1550)). 외부 dependent continuation이 inline 실행되면 같은 monitor/class로 재진입할 수 있다.
- Lane 밖 snapshot·참조:
  - `List.copyOf(bound)`와 `Optional<ZLinkSessionActor>`는 얕은 snapshot이다. Actor 참조의 수명은 GC가 보장하지만 binding의 현재성은 보장하지 않는다([L308](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:308), [L341](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:341)).
  - `previousTransition`, 이전 binding 목록, `IngressGate`, `SealTerminal`, `TargetOutboundTarget`, queue head, `RouteFlight`, held-ingress 목록이 monitor 밖 비동기 작업이나 callback으로 전달된다.
  - 특히 ingress는 gate·sequence를 꺼낸 뒤 operation을 실행하고 완료 callback에서 gate를 재검사한다([L776](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:776)).
  - Outbound drain은 owner·queue head를 꺼내 transport submit을 수행한 뒤 다시 monitor에 들어가 identity를 확인한다([L1233](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1233)).
  - Route preparation은 `RouteFlight` 참조를 밖으로 내보낸 뒤 native prepare를 수행하고, 완료 시 원래 route·actor·seal identity를 재검증한다([L1886](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1886)).
- 장기 작업: **6개 논리 시작점**.
  - route-ready retry/wait: L450·L522 → [L2294](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:2294).
  - native binding retry: L455·L523.
  - relocation seal 만료 timer: [L1022](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1022), 실제 scheduler L1464.
  - held-ingress 순차 resume 체인: [L877](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:877).
  - target outbound drain과 비동기 재시작: L1111·L1134·[L1296](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1296).
  - relocation route native prepare·compensation: L1964·[L2019](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:2019)·L2092.
- `CompletableFuture` 경계:
  - `replaceBinding`, ingress completion, outbound physical drain, route preparation과 compensation이 모두 비동기 아닌 `thenCompose`/`whenComplete`를 사용하므로 완료 스레드에서 inline 실행될 수 있다.
  - 반대로 held-ingress의 `thenComposeAsync`와 outbound의 `runAsync`는 common pool로 실행을 넘기며 lane `ThreadLocal`을 전파하지 않는다.
  - 현재 Java `ZLinkStateLane`은 결과 future를 lane의 `ThreadLocal`을 해제하기 전에 완료한다([ZLinkStateLane.java L56](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkStateLane.java:56), [L194](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkStateLane.java:194)). 따라서 호출자가 붙인 비동기 아닌 continuation이 lane 위에서 inline 실행되어, 같은 컴포넌트 재호출 시 즉시 재진입 예외가 날 수 있다. L1 전환 전에 확인해야 할 Java 고유 위험이다.
  - 추가 선행 조건: Java `ZLinkStateLane`과 생성자·메서드는 package-private인데 대상 클래스는 다른 package에 있다. `execution` package는 module에서 공개 export되므로 단순 `public` 전환은 공개 API 확대가 된다([module-info.java L14](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/module-info.java:14)). Runtime 내부 접근 방법을 먼저 결정해야 한다.
- Kotlin 파급:
  - Kotlin 모듈은 구체 `ZLinkSessionActorsRuntime`을 직접 참조하지 않는다: **0파일/0지점**.
  - 대신 `ZLinkSessionActors` 인터페이스를 suspend 확장으로 한 번 감싼다([ZLinkFrameworkExtensions.kt L98](/home/hep7/project/zlink/framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98)).
  - 전체 Kotlin sample/E2E에는 `actors()` 사용이 **18파일/46지점**이다. 따라서 구현 내부만 lane으로 옮겨 기존 interface를 유지하면 Kotlin source 파급은 없지만, 동기 `bound()`·`find()`를 비동기로 바꾸면 Kotlin까지 확실히 전파된다.
- POSDDD:
  - 할당: `List.copyOf`·stream `toList`, held queue의 `new ArrayList`, ingress마다 새 `StoredBindingRoute`, outbound entry마다 `CompletableFuture`, drain마다 `runAsync` 작업이 생성된다.
  - 복사: `CopyOnWriteArrayList`는 binding 추가·삭제마다 backing array를 복사하고, `bound()`와 종료·route 처리에서 다시 snapshot을 만든다.
  - 경합: binding·ingress·relocation·outbound send가 하나의 `sealTerminals` monitor를 공유한다. 특히 current-binding send는 monitor 안에서 multipart decode와 transport send까지 수행한다([L1097](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1097), [L1221](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1221)).
  - Java lane 기본 생성자는 Session마다 별도 virtual-thread executor를 만든다([ZLinkStateLane.java L38](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkStateLane.java:38)). 대상은 Session별 객체이므로 executor 할당·종료 수명도 측정 후보이며, 현재 대상 클래스에는 lane 종료를 직접 연결할 명확한 close 표면이 없다.
  - 성능 측정은 하지 않았으므로 병목 확정이 아니라 할당·복사·경합 관찰이다. 확인된 죽은 코드는 없다.

예상 난이도는 .NET 표본 대비 **약 2배**다. 동기화 블록과 직접 caller 수는 표본보다 작지만, 한 클래스에 더 많은 상태 기계가 결합되어 있고 `CompletableFuture` inline continuation, lane 접근성, per-Session executor 종료가 추가 판단을 요구한다. Lane primitive 보완이나 Kotlin 동기 조회 API 전파까지 같은 작업에 포함하면 **2.5배 위험**으로 보는 편이 안전하다.

조사 소요는 **약 12분**이다. 파일 수정, 빌드·테스트, `git` 명령은 실행하지 않았다. 따라서 지정 브랜치는 독립 확인하지 않았고, 보고는 제공된 checkout의 정적 소스 기준이다.


