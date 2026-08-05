# 07. Location authority

[레퍼런스 목차](README.ko.md)

Store 등록(`addLocationStore`/`addRelocationStore`), `configureLocations()`, `isPeerReady`는 Java
타입과 builder를 그대로 사용한다 — 정확한 signature와 옵션 표는
[Java 레퍼런스 07. Location authority](../../java/reference/07-location-authority.ko.md)를 그대로
따른다. Kotlin이 추가하는 것은 운영 조회(`ZLinkLocationRuntimeQuery`)에 대한 suspend 확장과
`Flow` projection뿐이다. Provider를 직접 구현할 때도 Java `CompletionStage` SPI를 그대로
구현한다 — `suspend` Store interface를 별도로 복제하지 않는다. 정확한 signature는
[Kotlin Location·Relocation exact interface](../../common/spec/server/languages/kotlin/interfaces/location-maintenance.ko.md)가
소유한다.

---

## `status()` (suspend 확장)

Location runtime 자체의 상태를 확인한다. Java `getStatus()`를 감싼 suspend 확장 함수다.

```kotlin
val status = locationQuery.status()
```

**옵션.** 이 함수에는 modifier가 없다.

**완료 결과.** Java 레퍼런스의 `getStatus` 완료 결과(`ZLinkLocationRuntimeStatus`)와 같다.

**선택 기준.** Java 레퍼런스의 `getStatus` 항목과 같다.

---

## `listTopology` / `listServiceSummaries` (suspend 확장) / `topology` (Flow)

등록된 node topology나 MeshName별 서비스 요약을 페이지 단위로 조회하거나, `Flow`로 이어 받는다.

```kotlin
val page = locationQuery.listTopology(
    ZLinkLocationTopologyFilter("play", null, ZLinkTopologyState.READY),
)

// 페이지 단위 조회를 감추고 Flow로 순회하는 경우
locationQuery.topology(filter, pageSize = 200).collect { entry -> ... }
```

**옵션.** `listTopology`/`listServiceSummaries`의 `page` 인자는 `ZLinkPageRequest.firstPage()`를
기본값으로 쓴다 — 나머지 filter·page 의미는 Java 레퍼런스의 `listTopology`/`listServiceSummaries`
항목과 같다. `topology(filter, pageSize = 100)`는 `Flow<ZLinkLocationTopologyEntry>`를 반환하며
내부에서 continuation token을 따라 다음 페이지를 자동으로 이어 조회한다.

**완료 결과.** 조회 projection은 bounded page와 Java result type을 유지한다. Raw Spot·Actor
authority row, Store key, provider version과 scan cursor는 추가하지 않는다.

**선택 기준.** 한 페이지 결과만 필요하면 suspend 확장을, 전체 항목을 순회하려면 `Flow` 버전을
쓴다.

---

전체 근거는
[Kotlin Location·Relocation exact interface](../../common/spec/server/languages/kotlin/interfaces/location-maintenance.ko.md)와
[Java 레퍼런스 07. Location authority](../../java/reference/07-location-authority.ko.md)를
참고한다.
