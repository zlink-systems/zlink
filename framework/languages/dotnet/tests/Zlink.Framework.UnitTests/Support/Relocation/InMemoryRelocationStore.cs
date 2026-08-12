using System.Security.Cryptography;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Canonical in-memory <see cref="IZLinkRelocationRepository"/> test double.
/// References are content-addressed (SHA-256 hex) so repeated puts of the
/// same payload stay idempotent, matching provider store semantics.
/// </summary>
internal sealed class InMemoryRelocationStore :
    IZLinkRelocationRepository,
    IZLinkRelocationStore
{
    private readonly Dictionary<string, DateTimeOffset> _expirations =
        new(StringComparer.Ordinal);

    internal Dictionary<string, byte[]> Payloads { get; } =
        new(StringComparer.Ordinal);

    internal bool Contains(string reference) => Payloads.ContainsKey(reference);

    internal void Remove(string reference)
    {
        Payloads.Remove(reference);
        _expirations.Remove(reference);
    }

    public ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var now = DateTimeOffset.UtcNow;
        var expiresAt = now + retention;
        if (Payloads.TryGetValue(reference.Value, out var current))
        {
            if (!current.AsSpan().SequenceEqual(payload.Span))
                return ValueTask.FromResult<ZLinkBlobPutResult>(
                    new ZLinkBlobPutResult.Conflict(now));
            _expirations[reference.Value] = expiresAt;
            return ValueTask.FromResult<ZLinkBlobPutResult>(
                new ZLinkBlobPutResult.AlreadyStored(expiresAt, now));
        }
        Payloads.Add(reference.Value, payload.ToArray());
        _expirations.Add(reference.Value, expiresAt);
        return ValueTask.FromResult<ZLinkBlobPutResult>(
            new ZLinkBlobPutResult.Stored(expiresAt, now));
    }

    public ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var now = DateTimeOffset.UtcNow;
        return ValueTask.FromResult<ZLinkBlobReadResult>(
            Payloads.TryGetValue(reference.Value, out var payload)
                ? new ZLinkBlobReadResult.Found(
                    payload,
                    _expirations.GetValueOrDefault(reference.Value, now),
                    now)
                : new ZLinkBlobReadResult.Missing(now));
    }

    public ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var now = DateTimeOffset.UtcNow;
        if (!Payloads.ContainsKey(reference.Value))
            return ValueTask.FromResult<ZLinkBlobRenewResult>(
                new ZLinkBlobRenewResult.Missing(now));
        var expiresAt = now + retention;
        _expirations[reference.Value] = expiresAt;
        return ValueTask.FromResult<ZLinkBlobRenewResult>(
            new ZLinkBlobRenewResult.Renewed(expiresAt, now));
    }

    public ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Remove(reference.Value);
        return ValueTask.CompletedTask;
    }

    public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var bytes = payload.ToArray();
        var reference = Convert.ToHexString(SHA256.HashData(bytes));
        return ValueTask.FromResult(Store(reference, bytes, retention));
    }

    public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
        string reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var bytes = payload.ToArray();
        if (Payloads.TryGetValue(reference, out var current)
            && !current.AsSpan().SequenceEqual(bytes))
            throw new InvalidDataException("Relocation reference collision.");
        return ValueTask.FromResult(Store(reference, bytes, retention));
    }

    public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult<ZLinkRelocationReadResult>(
            Payloads.TryGetValue(reference, out var payload)
                ? new ZLinkRelocationReadResult.Found(payload)
                : new ZLinkRelocationReadResult.Missing());

    public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
        string reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        var now = DateTimeOffset.UtcNow;
        return ValueTask.FromResult<ZLinkRelocationRenewResult>(
            Payloads.ContainsKey(reference)
                ? new ZLinkRelocationRenewResult.Renewed(now + retention, now)
                : new ZLinkRelocationRenewResult.Missing());
    }

    public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var removed = Payloads.Remove(reference);
        _expirations.Remove(reference);
        return ValueTask.FromResult(
            removed
                ? ZLinkRelocationDeleteResult.Deleted
                : ZLinkRelocationDeleteResult.Missing);
    }

    private ZLinkRelocationStored Store(
        string reference, byte[] bytes, TimeSpan retention)
    {
        Payloads[reference] = bytes;
        var now = DateTimeOffset.UtcNow;
        _expirations[reference] = now + retention;
        return new ZLinkRelocationStored(
            reference,
            ZLinkCrc32C.Compute(bytes),
            now + retention,
            now);
    }
}
