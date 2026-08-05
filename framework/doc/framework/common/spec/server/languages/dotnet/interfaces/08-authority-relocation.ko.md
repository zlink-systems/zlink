# .NET Location·Relocation provider 공개 인터페이스

[.NET exact interface 목차](README.ko.md) · [Location runtime](../../../../21-location-runtime.ko.md) ·
[Location Store provider](../../../../22-location-store-redis.ko.md) ·
[Relocation Store provider](../../../../23-relocation-store-redis.ko.md)

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

Reference는 Framework가 put 전에 발급하는 opaque UTF-8 `1..4096` bytes다. 같은 reference와 같은 bytes를
재시도하면 `AlreadyStored`, 다른 bytes면 `Conflict`다. 삭제되거나 만료된 reference도 다른 content에
재사용하지 않는다.

Application state를 나눈 data chunk는 최대 64 MiB다. Framework는 각 chunk 앞에 23-byte immutable
envelope를 붙인다. 따라서 `IZLinkRelocationStore.PutAsync(...)`가 받는 encoded blob은 최대
`64 MiB + 23 bytes`다. Framework는 최대 256 GiB logical stream을 최대 4,096개의 data chunk와 immutable
root manifest로 구성한다. Payload checksum과 root·chunk 관계는 Framework가 계산하고 검증한다.

Read result bytes는 consumer가 사용하는 동안 변경하지 않는다. Renew와 delete는 idempotent하며 delete는
reference가 없어도 성공한 no-op이다.

## 4. 취소와 결과 재조정

호출 전에 cancellation이 요청되면 provider는 I/O나 commit을 시작하지 않는다. 호출이 시작된 뒤
cancellation, timeout 또는 transport failure가 발생하면 commit 여부가 불확실할 수 있다.

Framework는 Location Store의 exact read와 version 또는 Relocation Store의 caller-issued reference로 결과를
재조정한다. `Conflict`, `Missing`, `Expired`와 `AlreadyStored`는 닫힌 정상 결과다.
`ArgumentException`과 `OperationCanceledException`이 아닌 Store 호출 예외는 Framework가 provider failure로
분류한다.

## 5. 수명

등록이 성공하면 Store instance의 수명은 Framework가 소유한다. Store가 `IAsyncDisposable` 또는
`IDisposable`을 구현하면 Framework는 dependent runtime을 먼저 종료한 뒤 정확히 한 번 dispose한다.

두 Store가 connection을 공유할 때 각 Store의 dispose가 해제할 connection lease는 provider가 관리한다.
Application은 등록 뒤 Store operation을 직접 호출하거나 instance를 교체·dispose하지 않는다.
