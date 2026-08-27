# CP3 JVM locations 상태 보호 전환 보고

## 결과

location·service 대상의 R 상태 보호 취득 23곳을 state lane으로 전환했다.
대상 여섯 파일의 `synchronized`는 모두 0개다. 감사표에서 E·S로 분류된
다른 파일의 취득은 변경하지 않았고, `Concurrent*` 컬렉션으로 C2 상태를
분할하지 않았다. STOP은 없다.

모든 동기 공개 표면은 `inStateLane(...).join()`으로 lane turn이 끝난 뒤에
반환한다. 따라서 이전 monitor가 반환 전에 보장하던 등록·판독·lazy 생성은
반환 뒤로 밀리지 않는다. Java lane 자체가 `completeAsync`로 결과를 완료하므로
caller dependent는 lane-current `ThreadLocal` 범위에서 inline 실행되지 않는다.

## 파일별 전환

| 파일 | synchronized 전 -> 후 | 분류·lane 편성 | 재진입 해소와 발견 5·9·10 |
|---|---:|---|---|
| `runtime/internal/service/ZLinkClassicFanoutLiveness.java` | 7 -> 0 | C2. `publishers`와 `nextBeaconNanos`를 `stateLane` 하나가 소유한다. `HashMap`은 평범한 컨테이너로 유지했다. | 같은 publisher의 연결 검증·ready/deadline 갱신과 만료 sweep을 한 turn에 둬 public 재호출이 없다. 동기 `connect`·`receive`·조회·만료는 join 후 반환(발견 9)한다. receive의 publisher 판독과 beacon 판정·갱신은 한 turn(발견 10)이며, lane의 `completeAsync` 완료(발견 5)를 사용한다. |
| `runtime/internal/service/ZLinkInMemoryLocationAuthority.java` | 4 -> 0 | C2. `rows`, `listeners`, store/object/owner generation과 sequence를 `stateLane` 하나가 소유한다. 모두 `HashMap`/`ArrayList`다. | 기존 `compareExchange` 안의 `read()` 재호출을 `readCore()`로 분리했다. 변경·sequence와 listener snapshot은 한 turn에서 만들고(발견 10), callback은 turn 밖에서 호출한다. 따라서 listener가 `read`·`compareExchange`·unsubscribe에 재진입해도 lane 재진입이 아니다. subscribe 등록/해제와 CAS 결과는 join 뒤 반환(발견 9)한다. |
| `runtime/locations/ZLinkInMemoryProviderLocationStore.java` | 3 -> 0 | C2. `rows`, `snapshots`, `version`을 `stateLane` 하나가 소유하며 두 map은 평범한 `HashMap`이다. | read/write/scan의 조건 판독, 만료 제거, version 부여, page/cursor 파생을 각각 하나의 turn으로 묶었다(발견 10). API가 반환하는 이미-completed stage도 lane turn 완료 뒤에 만들어져 반환 전 상태가 확정된다(발견 9). |
| `runtime/locations/ZLinkStatefulAuthorityRouteRuntime.java` | 2 -> 0 | C2. `applied`의 forget/remember/replacement와 clear/putAll을 `stateLane` 하나가 소유한다. | `reconcile`의 full-scan 결과 적용과 close cleanup을 같은 lane의 private `applyCore`로 분리했다. old/new route 비교와 그에 따른 적용은 한 turn(발견 10)이다. `close`는 join 후 반환(발견 9)한다. |
| `runtime/mesh/MeshNodeRegistration.java` | 1 -> 0 | C2. lazy `routingId`, `routingIdPrefix`, `entrySpotId`의 무효화/재생성을 `routingIdStateLane` 하나가 소유한다. | `routingIdCore`, `routingIdPrefixCore`, `ensureEntrySpotIdCore`로 public 표면 재호출을 제거했다. prefix 변경과 두 lazy 값의 invalidation, 그리고 prefix+UUID 조합은 각각 한 turn이다(발견 10). 모든 동기 builder/read 표면은 join 후 반환한다(발견 9). |
| `runtime/binding/ZLinkJavaRawMeshNode.java` | 6 -> 0 | descriptor·ready 설정은 C2 `descriptorStateLane`; lazy SpotNode와 receiver 적용은 C2 `spotNodeStateLane`; `userSpotTerminals` 단일 registry는 C1 `userSpotTerminalStateLane`이다. terminal map은 평범한 `HashMap`으로 유지했다. | 각각 `...Core()`로 분리해 lane 안 public 재호출을 없앴다. descriptor revision/ready publish, SpotNode 생성+receiver 캡처, terminal retention sweep+existing fingerprint/deadline/capacity 판정은 각기 하나의 turn(발견 10)이다. 동기 API는 join 뒤 반환(발견 9)하고 lane 결과는 `completeAsync`로 완료된다(발견 5). |

## 재진입·콜백 판정

- `ZLinkInMemoryLocationAuthority.compareExchange`의 conflict 경로는 기존 monitor 안에서
  `read` 공개 메서드를 호출했다. lane 전환 전 `readCore`로 분리해 실제 public-to-public
  재진입 지점을 제거했다.
- 같은 authority의 listener는 기존 monitor 안에서 호출됐다. 상태 전이, sequence 증가,
  listener snapshot을 turn A에서 끝내고 callback을 turn 밖에서 호출한다. listener가
  subscribe handle을 닫거나 authority API를 다시 호출해도 lane을 다시 진입하지 않는다.
- 나머지 대상의 R 경로에는 같은 객체의 public 표면을 다시 부르는 호출이 없었다.
  `ZLinkStatefulAuthorityRouteRuntime`과 RawMeshNode의 외부 호출은 기존에도 해당 상태
  전이 중에 수행되던 동기 작업이며 외부 await를 품는 E·S 작업 프로토콜은 이 변경에 없다.

## 발견 적용 요약

- 발견 5: 새 lane 호출은 모두 `ZLinkStateLane.runAsync`를 거치며, 그 구현의
  `CompletableFuture.completeAsync`가 lane-current scope 밖에서 dependent를 완료한다.
- 발견 9: 동기 signature를 비동기로 바꾸지 않았다. 등록·capture·lazy 생성은
  `inStateLane(...).join()`이 끝난 뒤 반환한다.
- 발견 10: CAS, scan page/cursor, full route diff, routing ID/entry ID, descriptor revision,
  terminal admission처럼 여러 read가 하나의 값을 만드는 구간을 분리 게시하지 않고 하나의
  lane turn으로 유지했다.

## 본문 조정 목록

없음. 공개 spec, 테스트 기대값, 오류 코드, timeout과 호출 순서를 변경하지 않았다.

## 테스트 결과

실행 명령:

```text
cd framework/languages/java
flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test
```

전체 실행은 compile과 test class 생성을 통과했고, 유일한 실패는 알려진 full-run flake였다.

```text
ZLinkJavaRawMeshNodeM6ATest > descriptorBackedPeerIntentRequiresLifecycleAndSecurityFence() FAILED
    org.opentest4j.AssertionFailedError at ZLinkJavaRawMeshNodeM6ATest.java:1486
```

규칙에 따라 해당 테스트를 단독 재실행했다.

```text
BUILD SUCCESSFUL in 3s
10 actionable tasks: 1 executed, 9 up-to-date
```

단독 명령:

```text
flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test --tests 'systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNodeM6ATest.descriptorBackedPeerIntentRequiresLifecycleAndSecurityFence'
```

SpotNode receiver를 같은 C2 lane으로 편성한 뒤 RawMeshNode 대상 class도 재실행했다.

```text
BUILD SUCCESSFUL in 3s
10 actionable tasks: 2 executed, 8 up-to-date
```

```text
flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test --tests 'systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNodeM6ATest'
```

## STOP 여부와 예상 밖 사항

STOP 없음. authority listener callback은 monitor의 재진입에 우연히 의존할 수 있었으므로,
lane 전환 때 callback을 lane 밖으로 이동해야 했다. 상태 전이와 listener snapshot은 callback
전에 유지해 callback이 관찰하는 stored/deleted 상태와 반환 순서는 보존했다.
