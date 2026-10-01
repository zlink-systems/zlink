# #1083 Java 매직 값 리뷰 반영 보고

## 범위와 판정

- 브랜치: `framework-java/1083-magic`
- 기준 커밋: `a08ee3d8b2`; `origin/main` 병합 커밋: `561db9c649` (충돌 없이 완료)
- 병합 뒤 `origin/main`이 `7773a6bc9e9c91747f8ac25b8040987d0165c36a`까지 전진했습니다(마지막 확인 시 branch `ahead 4, behind 27`). 이후 main diff 중 이 작업이 수정한 Java 파일 6개가 겹칩니다. 시작 시 요구된 병합은 완료했고, 작업 변경이 있는 상태에서 추가 병합은 하지 않았습니다.
- protected spec 경로는 수정하지 않았습니다.
- Java 변경 분류: **B — 기존 구현의 소유권·중복 결함 수정**. metric 출력과 record wire 형식은 유지합니다.
- 빌드·테스트는 WSL mirror `/home/hep7/worktree/zlink-1083-java-magic`에서 실행했습니다. Java unit 실행은 Node 의존성 준비 단계에서 기존 integrity 오류로 막혔고, origin/main snapshot에서도 같은 오류를 확인했습니다. Java contract suite와 format check는 통과했습니다.

## 발견 및 수정

| 발견 및 원인 | 수정 위치 | 결과 |
|---|---|---|
| sink가 inflight gauge·pause duration·실패 카운터 단위를 다른 metric 정의에서 빌렸고 단위 상수 사본을 뒀습니다. 기준 HEAD 원인: `ZLinkMicrometerMetricSink.java:29-31,169,275,287,295,320`. | `ZLinkRuntimeMetrics.java:22`의 `Unit`; `ZLinkMicrometerMetricSink.java:125` 이하의 단위 사용 | 단위 기호 13개를 하나의 `Unit` enum이 소유합니다. 각 metric 정의와 sink가 같은 `Unit`을 참조합니다. Micrometer의 `baseUnit` 문자열은 이전과 같습니다. |
| `GaugeState`, `ChannelSelectionFailure`, pressure state의 소문자 wire 값이 enum 이름 변환에 암묵적으로 결합됐습니다. 기준 HEAD 원인: `ZLinkMicrometerMetricSink.java:47,304-311,336`; `ZLinkMeshMessageMetrics.java:92`. | `ZLinkMicrometerMetricSink.java:38`, `ZLinkServiceTopologyRegistry.java:220`, `ZLinkApplicationJobQueuePressureState.java:4`; `ZLinkMeshMessageMetrics.java:90` | 각 enum에 명시적 wire 값을 두었습니다. `current`, `in_use`, `no_member`, `running`, `paused` 등 이전 문자열을 보존합니다. |
| `ZLinkSpotLifecycle`가 내부 metrics 정규 이름과 동일한 Map 기반 emission을 여러 지점에서 반복했습니다. 기준 HEAD 원인: `ZLinkSpotLifecycle.java:73,82,111,120,222,231,329,338,373,382,550,562,681,691`. | `ZLinkSpotLifecycle.java:69-87` | `recordSpotCount`, `recordSpotCreated`, `recordSpotClosed`로 emission 구현을 모았습니다. lifecycle trigger는 기존 위치에서 호출합니다. |
| descriptor record의 `recordVersion: 1` 쓰기·검사 값이 세 형식에서 literal로 반복됐습니다. 기준 HEAD 원인: `ZLinkProviderDescriptorRepository.java:501,603,778,815,845,880`. | `ZLinkProviderDescriptorRepository.java:95`, `:502`, `:604`, `:779`, `:816`, `:846`, `:881` | 세 descriptor schema가 하나의 `DESCRIPTOR_RECORD_VERSION`을 공유합니다. owner-lease record codec과는 별도 형식이므로 그 codec 상수와 합치지 않았습니다. |
| Authority record도 같은 숫자·field literal을 write/read에 반복했습니다. 기준 HEAD 원인: `ZLinkProviderAuthorityRepository.java:2708,2939`. | `ZLinkProviderAuthorityRepository.java:41-42`, `:2710`, `:2941` | 별도 record 형식이므로 `AUTHORITY_RECORD_VERSION`과 `FIELD_RECORD_VERSION`을 이 repository가 소유합니다. |

## wire 출력과 교차언어 대조

변경 후 테스트는 기존 metric name·tag 기대값과 단위·상태 문자열을 확인합니다. 위치 record golden conformance 테스트는 `recordVersion: 1`과 record field set을 비교합니다. 아래 wire 문자열은 변경 전후 동일합니다.

| 결정 | C++ | .NET | Node | Java 변경 후 / 스펙 |
|---|---|---|---|---|
| 단위 문자열 소유 | `host_capacity_runtime.hpp:50-62`; `service_topology_registry.cpp:115-118` | `ZLinkRuntimeMetrics.cs:55-67`, `:837-845` | `runtime-metrics.ts:154,214`; `:70,316-320` | 단일 `ZLinkRuntimeMetrics.Unit` (`ZLinkRuntimeMetrics.java:22`). 값은 metrics spec §3-4와 동일합니다. |
| pressure-state 표현 | `host_capacity_runtime.hpp:189-190,236-239` | `ZLinkRuntimeMetrics.cs:837-845` | `runtime-metrics.ts:70,316-320` | `running`, `paused`를 각 enum의 명시적 wire 값으로 유지합니다. Java interface spec은 `monitoring.ko.md:62`에 두 enum 값을 정합니다. |
| channel selection failure | `service_topology_registry.cpp:512,613-619` | `ZLinkManagedMeshNode.cs:11612-11627` | `channel-transports.ts:493,558`; `runtime-metrics.ts:521-525` | Java enum은 `no_member`, `not_ready`, `draining`을 명시합니다. metrics spec `02-runtime-metrics.ko.md:163,171`의 허용값과 일치합니다. |
| Spot metric 기록 | `spot_runtime.cpp:12737-12738,13087-13088` | `ZLinkRuntimeMetrics.cs:334-341`; `ZLinkSpotNodeCatalog.cs:778,960,1261,1848` | `spot-lifecycle-metrics.ts:10-14` | Java `ZLinkSpotLifecycle.java:69-87`의 세 helper가 count·created·closed emission을 소유합니다. Spot count spec `02-runtime-metrics.ko.md:222`과 일치합니다. |
| location record version | `provider_location_repository.hpp:2085,2132-2135,2861,3579,3594` | `ZLinkProviderLocationRepository.cs:881-914,927-956` | `location-store-repository.ts:105,3537,3553` | descriptor와 authority 형식마다 Java repository가 별도 version 상수를 소유합니다. location runtime spec `01-location-runtime.ko.md:403,490`을 따릅니다. Kotlin은 별도 runtime counterpart 없이 Java core를 공유합니다. |

### 분류 D로 보류한 교차언어 차이

`.NET`의 `no_target` 및 Node의 `no_ready_target` 경로는 Java enum이 표현하지 않는 selection reason을 전달합니다. 최신 `origin/main`의 runtime monitoring spec은 ClientServer target unavailable reason으로 `no_ready_target`을 정의하지만 (`01-runtime-monitoring.ko.md:258-264`), metrics spec은 `selection_failures.reason`으로 `no_member`, `not_ready`, `draining`만 열거하며 (`02-runtime-metrics.ko.md:171`) 이 두 분류 사이의 mapping을 정하지 않습니다. 이 Java 변경은 기존 Java 출력만 명시적으로 보존했으며 다른 언어의 경로는 수정하지 않았습니다. 원인 위치는 위 표의 .NET·Node 파일:line입니다. 이 mapping은 **D — spec gap**으로 보고합니다.

## 런타임 소유권·스펙·분류

- **소유 계층:** unit symbol은 `ZLinkRuntimeMetrics.Unit`, 각 enum wire 값은 해당 enum, Spot metric emission은 `ZLinkSpotLifecycle`, descriptor/authority version은 각각의 repository가 소유합니다. Micrometer sink는 unit wire 문자열을 출력합니다.
- **스펙 조항:** host metric unit 및 pressure metric은 `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:101-123`; channel metric과 허용 reason은 같은 파일 `:142-171`; Spot count는 같은 파일 `:222`; public pressure enum은 `framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md:62`; location record version은 `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:403,490`입니다.
- **교차언어 대조:** 위 표와 같이 C++·.NET·Node·Java 구현의 정적 위치를 대조했습니다. 이 작업에서는 Java 출력만 보존·명시했고 다른 언어는 수정하지 않았습니다. 별도 selection reason의 미정 mapping은 D입니다.
- **변경 분류:** 구현된 Java 변경은 **B**입니다. 미정 selection reason mapping은 **D**로 분리해 보류했습니다.

## 판정 위치 수

| 결정 | 수정 전 → 수정 후 | 설명 |
|---|---:|---|
| metric unit 값 | 13 → 13 | 값별 정의 수는 유지하고 소유 지점을 Metric·sink에서 `Unit` enum 하나로 모았습니다. |
| enum wire mapping | 3 → 3 | 암묵 lowercase 변환을 enum별 명시 값으로 바꾸되 판정 위치 수는 늘지 않았습니다. |
| Spot count / created / closed emission | 8→1 / 2→1 / 4→1 | 14개의 emission 구현을 세 private helper로 모았습니다. |
| descriptor record version | 6 → 1 | 세 descriptor record의 반복 literal을 하나의 형식 상수로 모았습니다. |
| authority record version | 2 → 1 | authority record의 write/read literal을 하나의 형식 상수로 모았습니다. |

## supervisor-guide §2 검토

| 기준 | 판정 |
|---|---|
| 스펙 gap | Java 출력은 정의된 값과 일치합니다. 다른 언어의 `no_target` 계열 mapping은 D로 보류했습니다. |
| 불필요한 규칙 | 단위 차용, enum 이름 변환, 반복 emission, record version literal을 제거했습니다. |
| 제어 분산 | 단위는 `Unit`, 각 enum 문자열은 해당 enum, lifecycle metric은 `ZLinkSpotLifecycle`, record version은 각 record repository가 소유합니다. |
| 동기화·execution gate | lock·queue·대기·추가 상태를 만들지 않았습니다. |
| hot path 성능 | 새 lock·copy·retry는 없습니다. `name().toLowerCase(Locale.ROOT)` 변환을 상수 wire 참조로 바꿨습니다. 성능 회귀 근거는 없으며 benchmark는 실행하지 않았습니다. |
| 리팩토링 잔여 | 변경한 Java runtime 및 Spring sink 호출 경로에 동일 helper나 unused option은 남기지 않았습니다. 인접한 location descriptor codec은 별도 직렬화 경로 후보로 넘겼습니다. |
| 매직 값 | 의미 있는 단위, enum wire, record version을 각각 단일 owner로 모았습니다. |
| 계층 소유권 | runtime metric·location record 구현의 기존 소유자를 유지하고 Spring sink는 typed unit을 출력으로 변환합니다. |

## 검증 로그

| 로그 | 결과 |
|---|---|
| `job-logs/01-pre-change-wire-values.log` | 초기 실행은 local package root 설정 오류로 `systems.zlink:zlink:1.12.0`을 찾지 못했습니다. 이 로그는 코드 실패 판정에 사용하지 않았습니다. |
| `job-logs/02-pre-change-wire-values.log` | 변경 전 typed ownership/wire characterization test가 새 `Unit`/`wire()` API 부재로 `cannot find symbol`에서 실패했습니다. 동일 문자열은 유지되는 정적 소유권 변경이므로 이를 runtime 값 차이의 재현으로 해석하지 않습니다. |
| `job-logs/03-focused-wire-values.log` | 초기 수정 후 focused test: `BUILD SUCCESSFUL`, 17 executed, 6 up-to-date. Authority record 상수 추가 전 실행이므로 최종 검증을 대체하지 않습니다. |
| `job-logs/03b-format-changed-files.log`, `job-logs/03c-post-format.diff` | 변경 Java 파일 10개 format 적용 exit 0. 포맷 후 `git diff --check`도 통과했습니다. |
| `job-logs/04-java-unit-contract.log` | Java `test` task 집합은 `:zlink-stream-connector:buildNodeStreamConnector`에서 TypeScript가 없어 실패했습니다. 48 tasks 중 32개 실행. `:zlink-framework-core:test`와 `:zlink-framework-spring-boot-starter:test`는 통과했고 assertion 실패는 없습니다. |
| `job-logs/04b-origin-main-unit-contract.log` | 동일한 unit 명령을 `origin/main` snapshot `76d2fda004d303ac69431175fa91c815c991aaa9`에서 실행했습니다. 같은 TypeScript `MODULE_NOT_FOUND`로 실패했습니다. |
| `job-logs/06-node-http-package.log` | 공식 HTTP client package 빌드 통과. 결과 tarball `0.26.0`을 job artifact 경로에 만들고 materialize했습니다. |
| `job-logs/07-node-npm-ci.log` | `npm ci`가 lockfile SHA-512와 로컬 HTTP client tarball의 SHA 불일치(`EINTEGRITY`)로 실패했습니다. |
| `job-logs/07c-origin-main-npm-ci.log` | 같은 `npm ci`와 같은 artifact를 origin/main snapshot에서 실행했고, 같은 `EINTEGRITY`가 재현됐습니다. Node HTTP client source와 lockfile은 두 snapshot에서 동일합니다. |
| `job-logs/08-java-contract-run.log` | 올바른 `ZLINK_LOCAL_PACKAGE_ROOT`와 JDK 25로 Java 전체 `contractTest` 통과: `BUILD SUCCESSFUL`, 50 tasks 중 12 executed, 38 up-to-date. |
| `job-logs/05-format-java-check.log` | `scripts/format/format.sh --check java` 통과. |

리팩토링 점검: 성능 0건·POSDDD 5건·불필요 코드 6건 발견, 고친 항목 5건: 단위 ownership·명시적 enum wire 값·lifecycle emission 소유·descriptor version·authority version; 넘긴 항목: `ZLinkLocationDescriptorCodec.java:259,279,324` (인접한 canonical descriptor serialization의 enum-name mapping, 별도 cross-language/spec 검토 필요), `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:11612-11627`, `framework/languages/node/packages/framework/src/runtime/channels/channel-transports.ts:493,558`, `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-metrics.ts:521-525` (선택 실패 reason mapping, D).
