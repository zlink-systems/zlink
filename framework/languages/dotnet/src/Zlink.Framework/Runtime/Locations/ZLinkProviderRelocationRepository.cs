namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkProviderRelocationRepository(IZLinkRelocationStore provider)
    : IZLinkRelocationRepository
{
    public async ValueTask<ZLinkRelocationStored> PutRelocationAsync(
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        // Reconcile an uncertain write at the same reference. Only a confirmed
        // conflict allocates another reference.
        var reference = new ZLinkBlobReference(Guid.NewGuid().ToString("N"));
        try
        {
            return await PutAtCoreAsync(reference, payload, retention, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError("relocation_put");
            throw;
        }
    }

    public async ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
        string reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(reference);
        try
        {
            return await PutAtCoreAsync(
                    new ZLinkBlobReference(reference),
                    payload,
                    retention,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError("relocation_put");
            throw;
        }
    }

    private async ValueTask<ZLinkRelocationStored> PutAtCoreAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ZLinkBlobPutResult result;
            try
            {
                result = await provider
                    .PutAsync(reference, payload, retention, cancellationToken)
                    .AsTask()
                    .WaitAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception failure)
                when (failure
                        is not OutOfMemoryException
                            and not StackOverflowException
                            and not AccessViolationException
                )
            {
                cancellationToken.ThrowIfCancellationRequested();
                var read = await provider
                    .ReadAsync(reference, cancellationToken)
                    .AsTask()
                    .WaitAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (read is ZLinkBlobReadResult.Found found)
                {
                    if (found.Bytes.Span.SequenceEqual(payload.Span))
                        return Stored(reference, payload.Span, found.ExpiresAt, found.StoreNow);
                    result = new ZLinkBlobPutResult.Conflict(found.StoreNow);
                }
                else
                    continue;
            }

            if (result is ZLinkBlobPutResult.Conflict)
            {
                reference = new ZLinkBlobReference(Guid.NewGuid().ToString("N"));
                continue;
            }
            var (expiresAt, storeNow) = result switch
            {
                ZLinkBlobPutResult.Stored stored => (stored.ExpiresAt, stored.StoreNow),
                ZLinkBlobPutResult.AlreadyStored stored => (stored.ExpiresAt, stored.StoreNow),
                _ => throw new InvalidOperationException(),
            };
            return Stored(reference, payload.Span, expiresAt, storeNow);
        }
    }

    public async ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default
    )
    {
        ZLinkBlobReadResult result;
        try
        {
            result = await provider
                .ReadAsync(new ZLinkBlobReference(reference), cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError("relocation_get");
            throw;
        }
        return result switch
        {
            ZLinkBlobReadResult.Found found => new ZLinkRelocationReadResult.Found(found.Bytes),
            ZLinkBlobReadResult.Missing => new ZLinkRelocationReadResult.Missing(),
            _ => throw new InvalidOperationException(),
        };
    }

    public async ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
        string reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default
    )
    {
        ZLinkBlobRenewResult result;
        try
        {
            result = await provider
                .RenewAsync(new ZLinkBlobReference(reference), retention, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError("relocation_put");
            throw;
        }
        return result switch
        {
            ZLinkBlobRenewResult.Renewed renewed => new ZLinkRelocationRenewResult.Renewed(
                renewed.ExpiresAt,
                renewed.StoreNow
            ),
            ZLinkBlobRenewResult.Missing => new ZLinkRelocationRenewResult.Missing(),
            _ => throw new InvalidOperationException(),
        };
    }

    public async ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default
    )
    {
        try
        {
            await provider
                .DeleteAsync(new ZLinkBlobReference(reference), cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError("relocation_delete");
            throw;
        }
        return ZLinkRelocationDeleteResult.Deleted;
    }

    private static uint ComputeCrc32C(ReadOnlySpan<byte> payload)
    {
        var crc = uint.MaxValue;
        foreach (var value in payload)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
            {
                crc = (crc >> 1) ^ (0x82f63b78U & (uint)-(int)(crc & 1));
            }
        }
        return ~crc;
    }

    private static ZLinkRelocationStored Stored(
        ZLinkBlobReference reference,
        ReadOnlySpan<byte> payload,
        DateTimeOffset expiresAt,
        DateTimeOffset storeNow
    ) => new(reference.Value, ComputeCrc32C(payload), expiresAt, storeNow);
}
