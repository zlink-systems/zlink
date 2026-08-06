# Framework 구현 차이 기록

[스펙 목차](README.ko.md) · [RouteMesh topology](07-channel-topology.ko.md) ·
[내부 target 선택과 route cache](../internals/06-routing-and-cache.ko.md)

이 문서는 정식 public contract가 아니다. 공통 spec이 요구하는 동작과 현재 구현을
대조해 확인한 차이와 그 차이를 닫은 근거를 기록한다. 정식 계약은 공통 spec과
언어별 exact interface가 소유하며, 이 문서만으로 다른 기능의 완료를 판정하지 않는다.

## 1. 이번 검토 범위

이번 기록은 JVM Java/Kotlin Framework의 Location Store 기반 object peer 연결과
TicTacToe sample의 실행 경로를 대상으로 한다. Manual endpoint는 connection intent이고,
descriptor가 제공하는 lifecycle generation과 security identity는 runtime admission fence로
전달되어야 한다.

## 2. 닫은 구현 차이

| 항목 | 대상 | 이전 구현 | 수정 결과 | 상태 |
|---|---|---|---|---|
| `JVM-TOPO-001` | `07-channel-topology`의 peer handshake와 Java/Kotlin runtime | `connectManualObjectPeers`와 `ensureManualObjectPeer`가 endpoint와 RID만 전달해 lifecycle generation `0`과 RID 기반 security identity fallback을 사용했다. | 두 owner-layer 경로가 descriptor의 endpoint, RID, lifecycle generation과 security identity를 extended `connectPeer`에 함께 전달한다. | closed |

이 수정은 sample 호출부에 fence 값을 추가하지 않는다. Application은 기존 public manual
endpoint API를 그대로 사용하고, descriptor와 transport 사이의 매칭은 runtime이 담당한다.

## 3. 검증 근거

- Java raw binding regression은 잘못된 lifecycle generation/security identity를 가진 peer
  intent가 admission되지 않고, descriptor 값으로 다시 연결하면 admitted 되는지를 확인한다.
- Java raw regression은 다음 명령으로 통과했다.
  `cd framework/languages/java && ./gradlew --no-daemon --no-parallel :zlink-framework-core:test --tests '*ZLinkJavaRawMeshNodeM6ATest' --tests '*ZLinkActorCreationCoordinatorTargetSelectionTest'`
- Java aggregate는 다음 명령으로 통과했다. preflight 뒤 TicTacToe, Bingo, DeliveryDispatch,
  GameQuest, ShoppingMall, SupportChat의 실제 client/server self-check가 모두 완료됐고,
  `PASS TicTacToe.Java`와 최종 성공 marker를 확인했다.
  `cd framework/languages/java/samples && ZLINK_SAMPLE_LANGUAGES=java timeout 1800s ./run_samples.sh`
- Kotlin aggregate는 Java aggregate 완료 후 다음 명령으로 통과했다. `PASS TicTacToe.Kotlin`과
  Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat을 포함한 Kotlin 6개 실제
  process self-check가 완료됐다.
  `cd framework/languages/java/samples && ZLINK_SAMPLE_LANGUAGES=kotlin timeout 1800s ./run_samples.sh`
- Kotlin Bingo는 aggregate 전후의 단독 반복 실행도 각각 exit `0`으로 완료됐다. 이 과정에서
  event를 받기 전에 등록되어야 하는 `waitFor`를 `CoroutineStart.UNDISPATCHED`로 시작하도록
  sample을 수정했다.

## 4. Runtime shutdown과 sample runner의 경계

공통 shutdown spec은 먼저 public 30초 drain deadline을 적용하고, deadline 뒤에도 bounded
teardown을 수행할 수 있다고 정의한다. 현재 JVM host runtime의 resource cleanup은 이 순서를
따르므로 sample runner가 30초에서 즉시 process를 강제 종료하면 sample 결과와 runtime
teardown 결과를 혼동할 수 있다. 공통 runner와 Java `DeliveryDispatch` runner는 이제 최대
90초 동안 이 bounded cleanup을 관찰한 뒤에만 `SIGKILL`을 사용한다. 강제 종료가 발생하거나
cleanup이 실패하면 runner는 성공으로 숨기지 않고 non-zero로 종료한다. 이 90초는 sample
runner의 관찰 상한이며 Framework public shutdown deadline을 변경한 값이 아니다.

## 5. 판정 경계

이 기록에서 `JVM-TOPO-001`을 닫은 것은 해당 runtime 경로와 Java/Kotlin sample 실행 증거의
범위에 한정된다. 전체 Framework의 contract, package provenance, clean consumer, 공통
cross-language E2E와 performance gate의 상태를 변경하지 않는다.

## 6. Node.js 0.10.0 검토

이번 절은 Node.js/NestJS Framework와 Node sample의 `0.10.0` 기준 검토 결과다. Node
runtime의 동작 기준은 공통 internals와 공통 formal spec을 따르고, public interface의
정확한 표현은 Node exact interface가 소유한다.

### 6.1 닫은 구현 차이

| 항목 | 대상 | 이전 구현 | 수정 결과 | 상태 |
|---|---|---|---|---|
| `NODE-ROUTE-001` | `16 Spot 주소 메시징`, `18 Spot·Actor routing`, Node `requestToSpotAddress` | 기존 Ready Instance route에서 `ActorLocationStale`가 반환되어도 route를 갱신하고 같은 request를 다시 제출할 수 있었다. | `RequestTargetNotFound`처럼 admission 전에 route 부재를 확인한 경우에만 Instance intent의 cold activation route를 다시 판단한다. transport 경계를 넘었을 수 있는 `ActorLocationStale` 결과는 terminal error로 전달하고 같은 operation을 hidden retry하지 않는다. | closed |
| `NODE-RELOC-001` | Entry Spot Actor relocation membership restore | M6C production-target fixture가 실제 `ZLinkActorRuntimeState.clearJoinedSpot()` 계약을 구현하지 않아 Entry membership 복원에서 `TypeError`가 발생했다. | fixture가 실제 state contract를 구현하고, Entry Spot membership 복원 시 joined Spot 상태를 지우는 runtime 경로를 검증한다. | closed |
| `NODE-SAMPLE-001` | Node sample README와 Bingo routing-id static gate | sample README가 `0.10.0`과 다른 framework 버전을 가리켰고, Bingo 검증기가 runner의 parameterized default marker를 고정 문자열로 잘못 검사했다. | Node sample metadata를 `0.10.0`으로 통일하고, 검증기가 실제 runner의 marker 선언과 사용을 검사하도록 수정했다. | closed |

### 6.2 검증 근거

- `npm ci`가 fresh local package와 갱신된 provenance를 사용해 완료됐다. Node package와
  Framework package version은 `0.10.0`이다.
- `npm run typecheck`, `npm run lint`가 통과했다.
- `npm run verify:m5-foundation`: 5/5.
- `npm run verify:m6a-runtime`: 35/35.
- `npm run verify:m6b-runtime`: 84/84.
- `npm run verify:m6c-runtime`: 79/79.
- 실제 process runner인 `npm run verify:samples`가 TicTacToe.Ts, Bingo.Ts,
  DeliveryDispatch.Ts, SupportChat.Ts, GameQuest.Ts, ShoppingMall.Ts, ZoneWorld의
  일곱 sample을 모두 통과시켰다.
- process aggregate를 제외한 Node sample static contract test는 35/35로 통과했다.

### 6.3 남은 검증 조건

- 전체 `test/contract/channel-client.test.js`는 앞의 27개 test 이후 public dealer/router
  binding socket test에서 Core `core/src/runtime/utils/mutex.hpp:108`의
  `Invalid argument` abort가 발생한다. 해당 test를 단독으로 실행하면 통과하고, 선택한
  앞선 test와 함께 실행해도 통과하므로 현재 증거는 Node public routing 결함보다 Core
  native mutex lifecycle 또는 test teardown 문제를 가리킨다. 이 항목은 Node 구현 차이를
  닫은 것으로 표시하지 않는다.
- sample-regression이 내부에서 process aggregate를 다시 실행하는 test는 한 실행에서
  ZoneWorld의 log marker timeout으로 실패했다. 같은 변경 상태에서 독립적인
  `npm run verify:samples`는 일곱 sample을 모두 통과했으므로, 이 항목은 sample 계약
  차이가 아니라 process-runner의 재실행 안정성 검증 조건으로 남긴다.
- 공통 cross-language process E2E, clean consumer, package provenance의 전체 언어 범위와
  performance gate는 이 Node 검토로 완료 처리하지 않는다.
