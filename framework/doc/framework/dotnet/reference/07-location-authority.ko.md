# 07. Location authority

[레퍼런스 목차](README.ko.md)

이 category는 Location·Relocation Store 등록, `ZLinkLocationOptions` 조정, `IZLinkLocationReadiness`와
`IZLinkLocationRuntimeQuery`가 제공하는 진입점을 다룬다. 정확한 signature는
[Location 설정과 운영 exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md)와
[공식 Redis Store exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-provider-redis.ko.md)가
소유한다.

---

## Location·Relocation Store 등록 (구성 시점)

분산 discovery, Instance Spot cold activation 또는 Actor·Spot relocation을 쓰는 host가 Store
구현을 root에 등록한다.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:location",
        })); // 작은 opaque location record를 저장하는 provider를 등록한다

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation",
        })); // immutable relocation payload를 별도 capability로 등록한다
});
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.AddLocationStore(IZLinkLocationStore)` | 없으면 분산 discovery·relocation 불가 | exact read, conditional atomic batch, bounded snapshot scan을 제공하는 Store 하나 |
| `.AddRelocationStore(IZLinkRelocationStore)` | `RecreateOnRelocation`/`PreserveStateWith` factory나 Instance Spot factory가 하나라도 있으면 필수 | Framework가 발급한 reference에 immutable relocation payload를 저장하는 Store 하나 |
| `ZLinkRedisLocationOptions.KeyPrefix` / `ZLinkRedisRelocationOptions.KeyPrefix` | 코드 initializer는 빈 문자열이지만 유효한 구성에는 반드시 비어 있지 않은 값을 지정해야 한다(둘이 같은 Redis를 쓰면 서로 달라야 함) | Redis key namespace |
| `.ConnectionString` 또는 `.ConfigurationOptions` | 필수(둘 중 하나) | Redis 연결 설정. 둘 다 지정하면 `ConfigurationOptions`를 사용 |

**완료 결과.** 반환값 없이 동기로 등록된다. 각 역할은 정확히 하나만 등록한다 — 같은 역할을 두 번
등록하거나 필수 Store가 없으면 host startup 검증에서 `ZLinkConfigurationException`으로 드러난다.
`OperationTimeout`이 0 이하면 provider I/O 전에 `ArgumentException`으로 거부한다.

**선택 기준.** Manual peer만 쓰고 분산 location 기능이 필요 없는 node는 이 항목을 생략하고 시작할
수 있다. 공식 Redis provider 외에 같은 `IZLinkLocationStore`/`IZLinkRelocationStore`를 구현하는
다른 provider도 등록할 수 있다. 등록 뒤에는 application이 Store operation을 직접 호출하거나
Store를 교체·dispose하지 않는다.

---

## `ConfigureLocations()` (구성 시점)

Owner lease, polling과 relocation 동시성 상한을 조정한다.

```csharp
services.AddZLinkFramework(options =>
{
    ZLinkLocationOptions locations = options.ConfigureLocations();
    locations.OwnerLeaseTtl = TimeSpan.FromSeconds(20);
    locations.MaxConcurrentRelocationCaptures = 16;
});
```

**옵션.** 자주 조정하는 값은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `OwnerLeaseRenewInterval` / `OwnerLeaseTtl` / `OwnerLeaseFencingMargin` / `OwnerLeaseRenewTimeout` | 5초 / 15초 / 5초 / 3초 | Owner lease 갱신 주기와 유효기간. `OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout < OwnerLeaseTtl - OwnerLeaseFencingMargin`을 만족해야 한다 |
| `PollingInterval` | 1초 | Store 상태 확인 주기 |
| `StoreFailureGrace` | 30초 | Store 장애를 감내하는 유예 시간 |
| `RouteCacheMaxAge` / `MessageFollowDuration` | 15초 / 30초 | 0이면 기능을 끈다. 둘 다 양수면 cache age가 Message Follow duration보다 최소 5초 작아야 한다 |
| `MaxActiveOutboundRelocations` / `MaxActiveInboundRelocations` | 64 / 64 | 동시 진행 가능한 relocation unit 상한 |
| `MaxConcurrentRelocationCaptures` / `MaxConcurrentRelocationRestores` | 8 / 8 | 동시 실행 가능한 Capture·Restore callback 상한 |
| `MaxRelocationPayloadInFlightBytes` | 268,435,456 | process 전체 encoded relocation payload in-flight 상한 |

**완료 결과.** 동기 설정이다. Lease·polling 값이 0 이하이거나 위 부등식을 어기면 host startup
검증에서 드러난다. 실행 중 값 변경은 새 relocation admission에만 적용된다.

**선택 기준.** 기본값이 배포 환경(네트워크 지연, Store 응답 시간)에 맞지 않을 때만 조정한다.

---

## `IsPeerReadyAsync`

특정 MeshName·role(선택적으로 특정 node)의 peer가 준비됐는지 확인한다.

```csharp
bool ready = await locationReadiness.IsPeerReadyAsync(
    "play",
    ZLinkLocationRole.Spot,
    nodeRid: null,
    ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `nodeRid` | `null`(role 전체 기준) | 특정 node로 좁혀서 확인 |

**완료 결과.** `bool`을 반환한다. 별도 실패 kind 없이 준비 여부만 알려준다.

**선택 기준.** 특정 역할의 peer가 준비될 때까지 기다리는 startup 순서 제어나 헬스체크에 쓴다.

---

## `GetStatusAsync`

Location runtime 자체의 상태(Store 연결, owner lease 갱신)를 확인한다.

```csharp
ZLinkLocationRuntimeStatus status = await locationQuery.GetStatusAsync(ct);
bool healthy = status.StoreHealthy && status.OwnerLeaseHealthy;
```

**옵션.** 이 진입점에는 modifier가 없다 — `CancellationToken`만 받는다.

**완료 결과.** `ZLinkLocationRuntimeStatus`를 반환한다. `StoreHealthy`와 `OwnerLeaseHealthy`가
각각 Store 연결과 owner lease 갱신 상태를 나타내며, `LastRefreshAt`/`OwnerLeaseRenewedAt`으로
마지막 갱신 시각을 확인한다.

**선택 기준.** Location 인프라 자체의 건강 상태를 진단할 때 쓴다. 특정 peer 준비 여부는
`IsPeerReadyAsync`를 쓴다.

---

## `ListTopologyAsync` / `ListServiceSummariesAsync`

등록된 node topology나 MeshName별 서비스 요약을 페이지 단위로 조회한다.

```csharp
ZLinkLocationPage<ZLinkLocationTopologyEntry> page = await locationQuery.ListTopologyAsync(
    new ZLinkLocationTopologyFilter(MeshName: "play", State: ZLinkLocationTopologyState.Ready),
    new ZLinkPageRequest(PageSize: 200),
    ct);
```

**옵션.** 두 호출 모두 다음 modifier를 받는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| filter(`ZLinkLocationTopologyFilter`/`ZLinkLocationServiceSummaryFilter`) | 전체(모든 필드 `null`) | MeshName·NodeRid·State로 결과를 좁힌다 |
| `page.PageSize` | 100 | 1..1000 범위 |
| `page.ContinuationToken` | `null`(첫 페이지) | 이전 응답이 반환한 opaque token. Application이 직접 해석하거나 다른 query에 재사용하지 않는다 |

**완료 결과.** `ZLinkLocationPage<T>`를 반환한다. `ContinuationToken`이 `null`이면 마지막
페이지다. Store key·version, owner lease generation, descriptor payload 같은 내부 정보는
반환하지 않는다.

**선택 기준.** 운영 도구에서 등록된 node나 서비스 현황을 사람이 볼 수 있는 형태로 조회할 때 쓴다.
단일 MeshName·ChannelName의 실시간 가용성 판단에는 topology-discovery category의 상태 조회
항목을 쓴다.

---

전체 근거는
[Location 설정과 운영 exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md)와
[공식 Redis Store exact interface](../../common/spec/server/languages/dotnet/interfaces/08-location-provider-redis.ko.md)를
참고한다.
