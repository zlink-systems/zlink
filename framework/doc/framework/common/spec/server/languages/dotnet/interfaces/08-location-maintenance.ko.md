# .NET Location 설정과 운영 공개 인터페이스

[.NET exact interface 목차](README.ko.md) · [Location runtime](../../../../21-location-runtime.ko.md) ·
[Provider SPI](08-authority-relocation.ko.md) · [Host monitoring](10-topology-monitoring.ko.md)

## 1. 범위

이 문서는 application이 사용하는 Location 설정, readiness와 운영 query만 정의한다. Store provider가
구현하는 기술적 primitive는 [provider SPI](08-authority-relocation.ko.md)가 소유한다.

Authority key·version, owner token, descriptor record, capacity fence, reservation, aggregate와 relocation
reference는 Framework 내부 정보이므로 이 application contract에 선언하지 않는다.

## 2. Location option

```csharp
public sealed class ZLinkLocationOptions
{
    public TimeSpan OwnerLeaseRenewInterval { get; set; }
        = TimeSpan.FromSeconds(5);
    public TimeSpan OwnerLeaseTtl { get; set; }
        = TimeSpan.FromSeconds(15);
    public TimeSpan PollingInterval { get; set; }
        = TimeSpan.FromSeconds(1);
    public TimeSpan StoreFailureGrace { get; set; }
        = TimeSpan.FromSeconds(30);
    public TimeSpan OwnerLeaseFencingMargin { get; set; }
        = TimeSpan.FromSeconds(5);
    public TimeSpan OwnerLeaseRenewTimeout { get; set; }
        = TimeSpan.FromSeconds(3);
    public TimeSpan RouteCacheMaxAge { get; set; }
        = TimeSpan.FromSeconds(15);
    public TimeSpan MessageFollowDuration { get; set; }
        = TimeSpan.FromSeconds(30);
    public int MaxActiveOutboundRelocations { get; set; } = 64;
    public int MaxActiveInboundRelocations { get; set; } = 64;
    public int MaxConcurrentRelocationCaptures { get; set; } = 8;
    public int MaxConcurrentRelocationRestores { get; set; } = 8;
    public long MaxRelocationPayloadInFlightBytes { get; set; }
        = 268_435_456;
}
```

Root의 `AddLocationStore(...)`, `AddRelocationStore(...)`와 `ConfigureLocations()` signature는
[Topology configuration](03-configuration-topology.ko.md#2-등록-인터페이스)가 소유한다. Store는 역할별로
정확히 하나 등록한다. 같은 역할을 두 번 등록하면 socket bind 전에 `ZLinkConfigurationException`으로
startup을 실패한다.

Lease와 polling option은 0보다 커야 한다. 모든 Location host는 다음 관계를 만족해야 한다.

```text
OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout
    < OwnerLeaseTtl - OwnerLeaseFencingMargin
```

`RouteCacheMaxAge`와 `MessageFollowDuration`은 0 이상이다. 둘 다 양수이면 cache age가 Message Follow
duration보다 최소 5초 작아야 한다. 0은 해당 기능을 끈다.

다섯 relocation 제한 option은 모두 양수다. Active outbound·inbound unit 기본 상한은 각각 64개,
Capture·Restore callback 기본 상한은 각각 8개, process 전체 encoded payload in-flight 기본 상한은
268,435,456 bytes다. 실행 중 변경은 새 relocation admission에만 적용한다.

## 3. Readiness와 운영 query

```csharp
public sealed record ZLinkLocationRuntimeStatus(
    bool StoreHealthy,
    bool OwnerLeaseHealthy,
    DateTimeOffset? LastRefreshAt,
    DateTimeOffset? OwnerLeaseRenewedAt);

public enum ZLinkLocationTopologyState
{
    Discovered = 1,
    Connecting = 2,
    Ready = 3,
    Lost = 4,
    Error = 5,
    Stopped = 6
}

public sealed record ZLinkLocationTopologyFilter(
    string? MeshName = null,
    RoutingId? NodeRid = null,
    ZLinkLocationTopologyState? State = null);

public sealed record ZLinkLocationTopologyEntry(
    string MeshName,
    RoutingId NodeRid,
    string Endpoint,
    bool Draining,
    ZLinkLocationTopologyState State,
    DateTimeOffset UpdatedAt);

public sealed record ZLinkLocationServiceSummaryFilter(
    string? MeshName = null);

public sealed record ZLinkLocationServiceSummary(
    string MeshName,
    uint TotalCount,
    uint ReadyCount,
    uint ErrorCount,
    uint StoppedCount,
    DateTimeOffset LastUpdatedAt);

public readonly record struct ZLinkPageRequest(
    int PageSize = 100,
    string? ContinuationToken = null);

public sealed record ZLinkLocationPage<T>(
    IReadOnlyList<T> Items,
    string? ContinuationToken);

public interface IZLinkLocationReadiness
{
    ValueTask<bool> IsPeerReadyAsync(
        string meshName,
        ZLinkLocationRole role,
        RoutingId? nodeRid = null,
        CancellationToken cancellationToken = default);
}

public interface IZLinkLocationRuntimeQuery
{
    ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>>
        ListServiceSummariesAsync(
            ZLinkLocationServiceSummaryFilter filter,
            ZLinkPageRequest page = default,
            CancellationToken cancellationToken = default);
}

public enum ZLinkLocationRole : ushort
{
    Invalid = 0,
    Spot = 2,
    Router = 3,
    Dealer = 4,
    Pub = 5,
    Sub = 6
}
```

운영 query는 사람이 이해할 수 있는 health·topology·service summary만 반환한다. Store key·version,
owner lease generation, descriptor payload와 protocol envelope는 반환하지 않는다. `NodeRid`는 실제 transport
routing identity이므로 public `RoutingId`로 유지한다.

Page size는 `1..1000`이고 continuation token은 해당 query가 발급한 opaque value다. Application은 token을
해석하거나 다른 query에 사용하지 않는다.

## 4. Host maintenance

Host maintenance는 `IZLinkFrameworkRuntime.RelocateAsync(...)`와 `ShutdownAsync(...)`가 소유한다.
`RelocateAsync(...)`는 가능한 workload를 다른 owner로 이전하고 `Relocated` 상태에서 완료한다.
`PlannedMaintenance`는 source와 같은 application version으로만 이전하며 target version을 받지 않는다.
`RollingUpdate`는 source보다 큰 target version을 필수로 받고 그 version과 정확히 일치하는 node로만
이전한다. 두 mode 모두 version, maintenance wave, capability, capacity, placement weight 순서로
candidate를 제한하고 선택한다. 요청 조건을 만족하는 target이 없으면 deadline까지 기다린 뒤
`Blocked/TargetUnavailable`로 완료한다.
Application은 결과를 확인한 뒤 `ShutdownAsync(...)`로 host를 종료할 수 있다. `ShutdownAsync(...)`를
`Serving`에서 바로 호출하면 새 relocation을 시작하지 않고 bounded cleanup 뒤 host를 종료한다.
정확한 signature와 결과는
[Host monitoring](10-topology-monitoring.ko.md)이 소유한다.
