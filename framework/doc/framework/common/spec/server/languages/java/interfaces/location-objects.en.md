# Java Location object query types

This document fixes the Java public types returned by the object location operations on
`ZLinkLocationRuntimeQuery`. They are for operational inspection only, not for messaging targets or placement
conditions.

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

`objectKind` is required; `stableType` and `meshName` are optional. `objectGeneration` is positive, and
`globalId` and an entry's `stableType` are non-blank. A filter's `stableType`, when specified, is non-blank.
The query does not provide an unbounded list. Page size is `1..1000`, the encoded page is at most 4 MiB,
and the continuation token is an opaque value issued by the query.

Direct lookup by Actor ID and Spot ID each queries one current object location. Missing returns an empty
`Optional`; Creating returns a `CREATING` entry; Ready returns a `READY` entry; and an unavailable current
owner after commit returns an `UNAVAILABLE` entry. `findSpotLocation(...)` treats User Spot and Instance
Spot under the same Spot-ID lookup contract. A Store query failure fails the whole operation with
`ZLinkFrameworkErrorKind.UNAVAILABLE` and does not return a partial page.
