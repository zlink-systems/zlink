# .NET 공식 etcd Location Store 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md) · [Provider SPI](08-authority-relocation.ko.md) ·
[Location 설정과 운영](08-location-maintenance.ko.md)

## 1. 범위

공식 etcd package는 Location Store 구현 class 하나를 제공한다. Relocation Store는 제공하지 않으며,
필요하면 다른 provider(예: [공식 Redis Store](08-location-provider-redis.ko.md))를 함께 등록한다. Public
surface는 Store instance를 만들기 위한 option으로 제한한다. 각 option의 뜻과 조건, 공통 동작과 저장
형식은 [공식 etcd Location Store provider](../../../05-location-relocation/07-location-store-etcd.ko.md)가 소유한다. etcd 전용 Framework
registration helper는 제공하지 않는다.

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

생성자는 [공통 option](../../../05-location-relocation/07-location-store-etcd.ko.md#11-공개-option)의 조건에 어긋나면 etcd에 접속하기 전에
`ArgumentException`으로 거부한다. Store가 소유한 operation은 구현한 generic provider SPI와 정확히 같다.

## 3. 수명

Framework 등록 뒤 Store 수명은 Framework가 소유한다. Dispose가 시작되면 새 operation은
`ObjectDisposedException`으로 실패하며, 진행 중인 operation이 끝난 뒤 Store가 소유한 자원을 해제한다.

## 4. 등록 예제

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
        // 합의로 복제하는 etcd에 Location record를 저장한다.

    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
        {
            ConnectionString = "redis-host:6379",
            KeyPrefix = "zlink:game:relocation"
        }));
        // Relocation Store는 다른 provider로 등록한다.
});
```

Application은 등록 뒤 Store operation을 직접 호출하거나 Store를 교체·dispose하지 않는다.
