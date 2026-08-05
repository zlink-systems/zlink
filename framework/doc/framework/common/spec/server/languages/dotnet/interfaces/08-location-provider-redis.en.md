# .NET Official Redis Store Public Interface

[.NET exact interface table of contents](README.en.md) · [Provider SPI](08-authority-relocation.ko.md) ·
[Location Configuration And Operations](08-location-maintenance.en.md)

## 1. Scope

The official Redis package provides Location Store and Relocation Store
as separate classes. The public surface is limited to the connection,
key namespace, and operation timeout settings for creating the two Store
instances.

Redis key layout, Lua script, private record encoding, schema marker,
retry, snapshot cursor, connection lease, and change stamp are
implementation details. A Redis-specific Framework registration helper,
or an API where one class implements both Store interfaces together,
isn't provided.

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

Each options sets a non-empty `KeyPrefix` and one of `ConnectionString`
or `ConfigurationOptions`. If both connection settings are provided,
`ConfigurationOptions` is used. `OperationTimeout` must be greater than
0. Violating the range is rejected with `ArgumentException` before
provider I/O.

The operations the Store owns are exactly the same as the generic
provider SPI it implements. A Redis-specific authority, reservation,
capacity, aggregate, relocation phase, or manifest operation isn't
added.

## 3. Lifetime And Connection

After Framework registration, the Store's lifetime is owned by the
framework. Once dispose starts, a new operation fails with
`ObjectDisposedException`, and after the already-started operations
finish, the connection lease the Store acquired is released.

If `ConnectionString` is used and the Redis configuration is the same,
the two Stores share the same multiplexer from the internal pool. Each
Store's `DisposeAsync()` only releases the lease it acquired, and
disposes the multiplexer when releasing the last lease. The normalized
connection configuration, including password, is turned into a hash and
used as the pool key, and isn't exposed in the public API or logs.

`ConfigurationOptions` can include a callback and a custom connection
object. Even if two options instances have the same string, they can't
be judged to have the same behavior, so in this case each Store owns its
own multiplexer.

Location and Relocation Store use different key prefixes when using the
same Redis deployment. Physically separate Redis is also supported.
Public correctness doesn't depend on connection sharing or a cross-store
Redis transaction.

## 4. Registration Example

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:location"
        }));
        // registers the provider that stores small opaque location records.

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation"
        }));
        // registers the immutable relocation blob provider separately.
});
```

After registration, the application doesn't call the two Store
operations directly, or replace/dispose the Store.
