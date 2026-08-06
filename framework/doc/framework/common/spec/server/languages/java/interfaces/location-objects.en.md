# Java Location object query types

This document fixes the Java public types returned by the object location operations on
`ZLinkLocationRuntimeQuery`. They are for operational inspection only, not for messaging targets or placement
conditions.

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

`objectKind` is required; `stableType` and `meshName` are optional. `ObjectGeneration` is positive, and
`globalId` and `stableType` are non-blank. The query does not provide an unbounded list, and its continuation
token is opaque.
