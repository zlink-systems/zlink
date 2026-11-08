# .NET Official etcd Location Store Public Interface

[.NET language interface index](README.en.md) · [Provider SPI](08-authority-relocation.en.md) ·
[Location configuration and operation](08-location-maintenance.en.md)

## 1. Scope

The official etcd package provides one Location Store implementation class. It doesn't provide a
Relocation Store; if one is needed, register another provider alongside it (for example the
[official Redis Store](08-location-provider-redis.en.md)). The public surface is limited to the options
needed to create the Store instance. The meaning and conditions of each option, the common behavior,
and the storage format are owned by the
[official etcd Location Store provider](../../../05-location-relocation/07-location-store-etcd.en.md). No etcd-specific Framework registration
helper is provided.

## 2. Public Contract

```csharp
namespace Zlink.Framework.Locations.Etcd;

public sealed class ZLinkEtcdClientIdentity
{
    public required string CertificatePem { get; init; }
    public required string KeyPem { get; init; }
}

public sealed class ZLinkEtcdTlsOptions
{
    public string? TrustedCaPem { get; set; }
    public ZLinkEtcdClientIdentity? ClientIdentity { get; set; }
}

public sealed class ZLinkEtcdCredentials
{
    public required string User { get; init; }
    public required string Password { get; init; }
}

public sealed class ZLinkEtcdLocationOptions
{
    public IList<string> Endpoints { get; } = new List<string>();
    public ZLinkEtcdTlsOptions? Tls { get; set; }
    public ZLinkEtcdCredentials? Credentials { get; set; }
    public string Namespace { get; set; } = string.Empty;
    public TimeSpan OperationTimeout { get; set; }
        = TimeSpan.FromSeconds(5);
}

public sealed class ZLinkEtcdLocationStore :
    Zlink.Framework.LocationProvider.IZLinkLocationStore,
    IAsyncDisposable
{
    public ZLinkEtcdLocationStore(ZLinkEtcdLocationOptions options);
    public ZLinkEtcdLocationStore(
        Action<ZLinkEtcdLocationOptions> configure);
    public ValueTask DisposeAsync();
}
```

The constructor rejects violations of the [common option](../../../05-location-relocation/07-location-store-etcd.en.md#11-public-options) conditions with
`ArgumentException` before connecting to etcd. The operations the Store owns are exactly the generic
provider SPI it implements.

## 3. Lifetime

After Framework registration, the Framework owns the Store's lifetime. Once dispose starts, new
operations fail with `ObjectDisposedException`, and after operations in progress finish, the Store
releases the resources it owns.

## 4. Registration Example

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkEtcdLocationStore(o =>
        {
            o.Endpoints.Add("https://etcd-1:2379");
            o.Endpoints.Add("https://etcd-2:2379");
            o.Endpoints.Add("https://etcd-3:2379");
            o.Tls = new ZLinkEtcdTlsOptions { TrustedCaPem = caPem };
            o.Namespace = "zlink-game";
        }));
        // Stores Location records in etcd, replicated by consensus.

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation"
        }));
        // The Relocation Store is registered with another provider.
});
```

After registration, the application doesn't call Store operations directly or replace or dispose the
Store.
