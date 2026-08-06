# Java Location object query types

이 문서는 `ZLinkLocationRuntimeQuery`의 object location query가 반환하는 Java public type을 고정한다.
이 타입은 application이 운영 상태를 확인하는 데만 사용하며 messaging target이나 placement 조건으로
사용하지 않는다.

```java
public enum ZLinkLocationObjectState {
    CREATING, READY, UNAVAILABLE
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

`objectKind`는 필수이며 `stableType`과 `meshName`은 선택 값이다. `ObjectGeneration`은 양수이고,
`globalId`와 `stableType`은 비어 있지 않다. query는 unbounded list를 제공하지 않으며 continuation token은
opaque 값이다.
