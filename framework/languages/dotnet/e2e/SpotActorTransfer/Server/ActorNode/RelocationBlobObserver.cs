using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using Zlink.Framework.LocationProvider;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.ActorNode;

internal sealed class RelocationBlobObserver
{
    private readonly ConcurrentQueue<RelocationBlobMeasurement> _measurements = new();

    public void Record(
        string operation,
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        long startedUnixTimeMilliseconds,
        long completedUnixTimeMilliseconds,
        long startedTimestamp,
        int activeOperationCount)
    {
        _measurements.Enqueue(new RelocationBlobMeasurement(
            operation,
            payload.Length,
            Convert.ToHexString(SHA256.HashData(payload.Span))
                .ToLowerInvariant(),
            Convert.ToHexString(
                    SHA256.HashData(
                        System.Text.Encoding.UTF8.GetBytes(reference.Value)))
                .ToLowerInvariant(),
            startedUnixTimeMilliseconds,
            completedUnixTimeMilliseconds,
            Stopwatch.GetElapsedTime(startedTimestamp)
                .TotalMilliseconds,
            activeOperationCount));
        Console.WriteLine(
            $"relocation_blob operation={operation} bytes={payload.Length}"
            + $" active={activeOperationCount}");
    }

    public RelocationBlobMeasurement[] Snapshot() => _measurements.ToArray();

    public void Reset()
    {
        while (_measurements.TryDequeue(out _))
        {
        }
    }
}

/// <summary>
/// Records only opaque blob size and checksum. It does not parse Framework
/// references, envelopes, or provider keys.
/// </summary>
internal sealed class ObservedRelocationStore(
    IZLinkRelocationStore inner,
    RelocationBlobObserver observer) :
    IZLinkRelocationStore,
    IAsyncDisposable
{
    private int _activeOperations;

    public async ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        var startedAt =
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var startedTimestamp = Stopwatch.GetTimestamp();
        var active = Interlocked.Increment(ref _activeOperations);
        try
        {
            return await inner.PutAsync(
                    reference,
                    payload,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            observer.Record(
                "put",
                reference,
                payload,
                startedAt,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                startedTimestamp,
                active);
            Interlocked.Decrement(ref _activeOperations);
        }
    }

    public async ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default)
    {
        var startedAt =
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var startedTimestamp = Stopwatch.GetTimestamp();
        var active = Interlocked.Increment(ref _activeOperations);
        ZLinkBlobReadResult? result = null;
        try
        {
            result = await inner.ReadAsync(reference, cancellationToken)
                .ConfigureAwait(false);
            return result;
        }
        finally
        {
            if (result is ZLinkBlobReadResult.Found found)
                observer.Record(
                    "read",
                    reference,
                    found.Bytes,
                    startedAt,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                    startedTimestamp,
                    active);
            Interlocked.Decrement(ref _activeOperations);
        }
    }

    public ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default) =>
        inner.RenewAsync(reference, retention, cancellationToken);

    public ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default) =>
        inner.DeleteAsync(reference, cancellationToken);

    public async ValueTask DisposeAsync()
    {
        switch (inner)
        {
            case IAsyncDisposable asyncDisposable:
                await asyncDisposable.DisposeAsync().ConfigureAwait(false);
                break;
            case IDisposable disposable:
                disposable.Dispose();
                break;
        }
    }
}
