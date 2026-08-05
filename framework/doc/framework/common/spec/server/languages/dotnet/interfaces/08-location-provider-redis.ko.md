# .NET 공식 Redis Store 공개 인터페이스

[.NET exact interface 목차](README.ko.md) · [Provider SPI](08-authority-relocation.ko.md) ·
[Location 설정과 운영](08-location-maintenance.ko.md)

## 1. 범위

공식 Redis package는 Location Store와 Relocation Store를 별도 class로 제공한다. Public surface는 두 Store
instance를 만들기 위한 connection, key namespace와 operation timeout 설정으로 제한한다.

Redis key layout, Lua script, private record encoding, schema marker, retry, snapshot cursor, connection lease와
change stamp는 implementation detail이다. Redis 전용 Framework registration helper나 하나의 class가 두 Store
interface를 함께 구현하는 API는 제공하지 않는다.

## 2. Public Contract

```csharp
namespace Zlink.Framework.Locations.Redis;

public sealed class ZLinkRedisLocationOptions
{
    public string? ConnectionString { get; set; }
    public StackExchange.Redis.ConfigurationOptions? ConfigurationOptions
        { get; set; }
    public string KeyPrefix { get; set; } = string.Empty;
    public TimeSpan OperationTimeout { get; set; }
        = TimeSpan.FromSeconds(5);
}

public sealed class ZLinkRedisLocationStore :
    Zlink.Framework.LocationProvider.IZLinkLocationStore,
    IAsyncDisposable
{
    public ZLinkRedisLocationStore(ZLinkRedisLocationOptions options);
    public ZLinkRedisLocationStore(
        Action<ZLinkRedisLocationOptions> configure);
    public ValueTask DisposeAsync();
}

public sealed class ZLinkRedisRelocationOptions
{
    public string? ConnectionString { get; set; }
    public StackExchange.Redis.ConfigurationOptions? ConfigurationOptions
        { get; set; }
    public string KeyPrefix { get; set; } = string.Empty;
    public TimeSpan OperationTimeout { get; set; }
        = TimeSpan.FromSeconds(5);
}

public sealed class ZLinkRedisRelocationStore :
    Zlink.Framework.LocationProvider.IZLinkRelocationStore,
    IAsyncDisposable
{
    public ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions options);
    public ZLinkRedisRelocationStore(
        Action<ZLinkRedisRelocationOptions> configure);
    public ValueTask DisposeAsync();
}
```

각 options에는 비어 있지 않은 `KeyPrefix`와 `ConnectionString` 또는 `ConfigurationOptions` 중 하나를
설정한다. 두 connection 설정을 모두 제공하면 `ConfigurationOptions`를 사용한다. `OperationTimeout`은
0보다 커야 한다. 범위를 위반하면 provider I/O 전에 `ArgumentException`으로 거부한다.

Store가 소유한 operation은 구현한 generic provider SPI와 정확히 같다. Redis 전용 authority, reservation,
capacity, aggregate, relocation phase와 manifest operation은 추가하지 않는다.

## 3. 수명과 connection

Framework 등록 뒤 Store 수명은 Framework가 소유한다. Dispose가 시작되면 새 operation은
`ObjectDisposedException`으로 실패하고 이미 시작한 operation이 끝난 뒤 Store가 획득한 connection lease를
해제한다.

`ConnectionString`을 사용하고 Redis 설정이 같으면 두 Store는 내부 pool에서 같은 multiplexer를 공유한다.
각 Store의 `DisposeAsync()`는 자신이 획득한 lease만 해제하며 마지막 lease를 해제할 때 multiplexer를
dispose한다. Password를 포함한 normalized connection 설정은 hash로 바꿔 pool key로 사용하며 public
API나 log에 노출하지 않는다.

`ConfigurationOptions`는 callback과 사용자 지정 connection object를 포함할 수 있다. 두 options instance의
문자열이 같아도 같은 동작이라고 판단할 수 없으므로 이 경우 Store마다 multiplexer를 소유한다.

Location과 Relocation Store는 같은 Redis deployment를 사용할 때 서로 다른 key prefix를 사용한다.
물리적으로 분리된 Redis도 지원한다. Public correctness는 connection 공유나 cross-store Redis transaction에
의존하지 않는다.

## 4. 등록 예제

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:location"
        }));
        // 작은 opaque location record를 저장하는 provider를 등록한다.

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation"
        }));
        // Immutable relocation blob provider를 별도로 등록한다.
});
```

Application은 등록 뒤 두 Store operation을 직접 호출하거나 Store를 교체·dispose하지 않는다.
