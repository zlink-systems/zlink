# .NET Location·Relocation provider 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md) · [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md) ·
[Location Store provider](../../../05-location-relocation/02-location-store-redis.ko.md) ·
[Relocation Store provider](../../../05-location-relocation/03-relocation-store-redis.ko.md)

## 1. 범위

이 문서는 외부 provider 작성자가 구현하는 최소 Store SPI의 정확한 C# 선언을 고정한다. Provider는 opaque
key·value의 conditional atomic batch와 Framework가 발급한 reference에 immutable blob을 저장하는 operation만
구현한다.

Authority, owner lease, descriptor, reservation, capacity, aggregate와 relocation phase는 Framework private
record다. 이 문서에는 해당 domain별 public method·result·DTO를 선언하지 않는다.

Primitive type과 두 Store interface는 별도 `Zlink.Framework.Provider.Abstractions` package가 제공한다.

## 2. Location Store

```csharp
namespace Zlink.Framework.LocationProvider;

public readonly record struct ZLinkStoreKey(string Value);
public readonly record struct ZLinkStoreVersion(string Value);
public readonly record struct ZLinkStoreScanCursor(string Value);

public sealed record ZLinkStoreValue(
    ReadOnlyMemory<byte> Bytes,
    ZLinkStoreVersion Version,
    DateTimeOffset? ExpiresAt,
    DateTimeOffset StoreNow);

public abstract record ZLinkStoreReadResult
{
    private protected ZLinkStoreReadResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkStoreReadResult;

    public sealed record Found(ZLinkStoreValue Value)
        : ZLinkStoreReadResult;
}

public abstract record ZLinkStoreCondition
{
    private protected ZLinkStoreCondition() { }

    public sealed record Missing(ZLinkStoreKey Key)
        : ZLinkStoreCondition;

    public sealed record Version(
        ZLinkStoreKey Key,
        ZLinkStoreVersion Expected)
        : ZLinkStoreCondition;
}

public abstract record ZLinkStoreMutation
{
    private protected ZLinkStoreMutation() { }

    public sealed record Put(
        ZLinkStoreKey Key,
        ReadOnlyMemory<byte> Bytes,
        TimeSpan? Retention)
        : ZLinkStoreMutation;

    public sealed record Delete(ZLinkStoreKey Key)
        : ZLinkStoreMutation;
}

public sealed record ZLinkStoreWriteRequest(
    IReadOnlyList<ZLinkStoreCondition> Conditions,
    IReadOnlyList<ZLinkStoreMutation> Mutations);

public abstract record ZLinkStoreWriteResult
{
    private protected ZLinkStoreWriteResult() { }

    public sealed record Applied(
        IReadOnlyDictionary<ZLinkStoreKey, ZLinkStoreVersion> PutVersions,
        DateTimeOffset StoreNow)
        : ZLinkStoreWriteResult;

    public sealed record Conflict(DateTimeOffset StoreNow)
        : ZLinkStoreWriteResult;
}

public sealed record ZLinkStoreScanRequest(
    string Prefix,
    ZLinkStoreScanCursor? Cursor,
    int Limit);

public sealed record ZLinkStoreScanPage(
    IReadOnlyList<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>> Items,
    ZLinkStoreScanCursor? NextCursor,
    DateTimeOffset StoreNow);

public abstract record ZLinkStoreScanResult
{
    private protected ZLinkStoreScanResult() { }

    public sealed record Page(ZLinkStoreScanPage Value)
        : ZLinkStoreScanResult;

    public sealed record Expired : ZLinkStoreScanResult;
}

/// <summary>
/// Stores opaque Framework records and applies a bounded conditional batch
/// as one atomic commit.
/// </summary>
public interface IZLinkLocationStore
{
    ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default);
}
```

Key는 opaque UTF-8 `1..1024` bytes이고 version은 provider가 발급하는 opaque UTF-8 `1..4096` bytes다.
Value bytes는 최대 1 MiB다. `ExpiresAt == null`은 durable value이며 TTL은 같은 결과에 포함된
`StoreNow`를 기준으로 판단한다.

`WriteAsync(...)`는 condition을 모두 검사하고 참일 때만 mutation 전체를 하나의 commit으로 적용한다.
Condition과 mutation의 unique key 합계는 최대 2,048개이고 encoded request는 최대 4 MiB다. 동일 key의
condition 중복과 mutation 중복은 `ArgumentException`이다. Condition 하나라도 거짓이면 `Conflict`이며
mutation과 version 증가는 0이다.

Scan limit은 `1..1000`이고 page encoded 크기는 최대 4 MiB다. 첫 page에서 만든 snapshot을 같은 cursor의
후속 page가 사용한다. Cursor는 opaque UTF-8 `1..4096` bytes다. Snapshot을 유지할 수 없으면 `Expired`이며
Framework는 부분 결과를 버리고 처음부터 다시 읽는다.

## 3. Relocation Store

```csharp
namespace Zlink.Framework.LocationProvider;

public readonly record struct ZLinkBlobReference(string Value);

public abstract record ZLinkBlobPutResult
{
    private protected ZLinkBlobPutResult() { }

    public sealed record Stored(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;

    public sealed record AlreadyStored(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;

    public sealed record Conflict(DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;
}

public abstract record ZLinkBlobReadResult
{
    private protected ZLinkBlobReadResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkBlobReadResult;

    public sealed record Found(
        ReadOnlyMemory<byte> Bytes,
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobReadResult;
}

public abstract record ZLinkBlobRenewResult
{
    private protected ZLinkBlobRenewResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;

    public sealed record Renewed(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;
}

/// <summary>
/// Stores immutable relocation payload under a reference issued by Framework.
/// It does not interpret relocation phases, manifests, or participants.
/// </summary>
public interface IZLinkRelocationStore
{
    ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);
}
```

Reference와 put 재시도는 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다. .NET 결과 이름은 `AlreadyStored`와 `Conflict`다.

Store의 payload 범위는 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다.

Chunk와 envelope 한도는 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다.

Read 소유권, renew와 delete는 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다.

## 4. 취소와 결과 재조정

Store 취소와 결과 불확실성은 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다.

결과 재조정은 [Relocation Store Redis](../../../05-location-relocation/03-relocation-store-redis.ko.md)가 정한다. .NET은 provider 예외를 `ArgumentException`, `OperationCanceledException` 또는 provider failure로 표현한다.

## 5. 수명

등록이 성공하면 Store instance의 수명은 Framework가 소유한다. Store가 `IAsyncDisposable` 또는
`IDisposable`을 구현하면 Framework는 dependent runtime을 먼저 종료한 뒤 정확히 한 번 dispose한다.

두 Store가 connection을 공유할 때 각 Store의 dispose가 해제할 connection lease는 provider가 관리한다.
Application은 등록 뒤 Store operation을 직접 호출하거나 instance를 교체·dispose하지 않는다.
