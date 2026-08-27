# Java Location object query types

이 문서는 `ZLinkLocationRuntimeQuery`의 object location query가 반환하는 Java public type을 고정한다.
이 타입은 application이 운영 상태를 확인하는 데만 사용하며 messaging target이나 placement 조건으로
사용하지 않는다.

```java
public enum ZLinkLocationObjectState {
 CREATING, READY, UNAVAILABLE
}

public enum ZLinkPlacementObjectKind {
 ACTOR, USER_SPOT, INSTANCE_SPOT
}

public record ZLinkLocationObjectEntry(
 String globalId,
 long objectGeneration,
 String meshName,
 systems.zlink.contracts.core.RoutingId nodeRid,
 ZLinkLocationObjectState state,
 String stableType) {}

public record ZLinkLocationObjectFilter(
 ZLinkPlacementObjectKind objectKind,
 String stableType,
 String meshName) {}
```

`objectKind`는 필수이며 `stableType`과 `meshName`은 선택 값이다. `objectGeneration`은 양수이고,
`globalId`와 entry의 `stableType`은 비어 있지 않다. Filter의 `stableType`을 지정하면 비어 있지 않아야 한다.
Query는 unbounded list를 제공하지 않는다. Page size는 `1..1000`, encoded page는 최대 4 MiB이며
continuation token은 query가 발급한 opaque 값이다.

Actor ID와 Spot ID의 직접 lookup은 각각 현재 object location 하나를 조회한다. Missing이면 빈
`Optional`, Creating이면 `CREATING`, Ready이면 `READY`, commit 뒤 current owner를 사용할 수 없으면
`UNAVAILABLE` entry를 반환한다. `findSpotLocation(...)`은 User Spot과 Instance Spot을 같은 Spot ID 조회
계약으로 다룬다. Store 조회가 실패하면 operation 전체가 `ZLinkFrameworkErrorKind.UNAVAILABLE`로
실패하며 page의 일부 결과를 반환하지 않는다.
