# Kotlin Location·Relocation 공개 인터페이스

[Kotlin 언어별 interface 목록](README.ko.md) ·
[Java Location·Relocation contract](../../java/interfaces/location-maintenance.ko.md)

Kotlin은 Java runtime과 provider SPI를 그대로 사용한다. 별도 Kotlin Store interface, abstract Store base
class 또는 Redis wrapper를 정의하지 않는다. Provider는 Java `ZLinkLocationStore` 또는
`ZLinkRelocationStore`를 구현하고 기존 Java
`ZLinkFrameworkOptions.addLocationStore(...)`와 `addRelocationStore(...)`로 등록한다.
두 Store와 primitive는 Java의 opt-in
`systems.zlink:zlink-framework-provider-abstractions` artifact에서 가져온다.

## Provider 계약

Java 계약의 다음 경계를 Kotlin에서도 그대로 적용한다.

- Location Store는 opaque key·value read, version condition을 포함한 atomic batch write와 bounded
 snapshot scan만 제공한다.
- Key·version·cursor, value·batch·scan 범위와 provider clock 기반 TTL 의미를 바꾸지 않는다.
- Relocation Store는 Framework가 미리 발급한 reference에 immutable blob을 저장한다.
- Actor·Spot relocation의 state handoff payload는 이 Store에 저장하지 않고 source에서 target으로
 직접 chunk 전송한다. Store에 남는 정상 실행 책임은 Instance Spot cold activation 기록과
 relocation 뒤 완료되는 pending request의 terminal 기록이다.
- 같은 reference와 같은 bytes의 재시도는 AlreadyStored, 다른 bytes는 Conflict다.
- Data chunk 하나는 application bytes 기준 최대 64 MiB이며 Framework가 최대 4,096개 chunk로 최대 256 GiB
 logical stream을 구성한다. Encoded blob 상한은 [Relocation Store Redis §3](../../../05-location-relocation/03-relocation-store-redis.ko.md#3-reference와-저장-크기)이 소유한다.
- Store 등록 뒤 ownership과 close 순서는 Java 계약과 같다. Shared connection lease는 provider가
 관리한다.

Kotlin provider를 작성할 때도 Java `CompletionStage` SPI를 구현한다. `suspend` Store interface를 별도로
복제하지 않는다. 따라서 coroutine scheduling은 atomic commit 경계, cancellation 재조정과 반환 buffer
수명을 변경하지 않는다.

Authority, owner lease, reservation, capacity, aggregate, fence와 relocation phase는 Framework private
record다. Kotlin public declaration이나 provider 결과 type으로 다시 노출하지 않는다. Redis key layout,
Lua script, private encoding, retry와 connection reference count도 공개하지 않는다.

## Coroutine 운영 조회

Kotlin package는 application이 사용하는 운영 조회에만 coroutine projection을 제공한다.

```kotlin
suspend fun ZLinkLocationRuntimeQuery.status(): ZLinkLocationRuntimeStatus

suspend fun ZLinkLocationRuntimeQuery.listTopology(
 filter: ZLinkLocationTopologyFilter,
 page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationTopologyEntry>

suspend fun ZLinkLocationRuntimeQuery.listServiceSummaries(
 filter: ZLinkLocationServiceSummaryFilter,
 page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationServiceSummary>

suspend fun ZLinkLocationRuntimeQuery.findActorLocation(
 actorId: String,
): ZLinkLocationObjectEntry?

suspend fun ZLinkLocationRuntimeQuery.findSpotLocation(
 spotId: String,
): ZLinkLocationObjectEntry?

suspend fun ZLinkLocationRuntimeQuery.listObjectLocations(
 filter: ZLinkLocationObjectFilter,
 page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationObjectEntry>

fun ZLinkLocationRuntimeQuery.topology(
 filter: ZLinkLocationTopologyFilter,
 pageSize: Int = 100,
): Flow<ZLinkLocationTopologyEntry>
```

조회 projection은 bounded page와 Java result type을 유지한다. 직접 lookup은 Missing이면 `null`,
Creating이면 `CREATING`, Ready이면 `READY`, commit 뒤 current owner를 사용할 수 없으면 `UNAVAILABLE`
entry를 반환한다. Spot 직접 lookup은 User Spot과 Instance Spot을 같은 Spot ID 조회 계약으로 다룬다.
Object list query는 kind를 필수로 받고 stable type과 MeshName을 선택 filter로 받는다. Page size는
`1..1000`, encoded page는 4 MiB 이하이며 continuation token은 query가 발급한 opaque 값이다. Store 조회
실패는 Java `ZLinkFrameworkErrorKind.UNAVAILABLE`이며 page 일부를 반환하지 않는다. Raw Spot·Actor authority row, Store key,
provider version과 scan cursor를 application 조회 결과에 추가하지 않는다.

## Redis extension

Kotlin application과 provider는 Java `ZLinkRedisLocationStore`,
`ZLinkRedisRelocationStore`와 각 options class를 그대로 사용한다. Kotlin 전용 registration helper나 두
Store를 묶는 wrapper는 제공하지 않는다. Redis 공개 표면은 두 public Store class의 최소 constructor와
connection, key namespace, operation timeout options로 제한한다.
