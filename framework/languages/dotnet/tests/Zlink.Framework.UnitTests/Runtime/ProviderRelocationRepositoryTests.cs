using System.Diagnostics;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ProviderRelocationRepositoryTests
{
    [Fact]
    public async Task LostPutResponse_ReconcilesTheFrameworkIssuedReference()
    {
        var provider = new AmbiguousRelocationStore(commitBeforeFailure: true);
        var repository = new ZLinkProviderRelocationRepository(provider);
        var payload = new byte[] { 1, 2, 3, 4 };

        var first = await repository.PutRelocationAsync(
            payload,
            TimeSpan.FromMinutes(5));
        var second = await repository.PutRelocationAsync(
            payload,
            TimeSpan.FromMinutes(5));

        Assert.Equal(provider.PutReferences[0], first.Reference);
        Assert.Equal(provider.PutReferences[0], provider.ReadReferences.Single());
        Assert.NotEqual(first.Reference, second.Reference);
        Assert.Equal(first.ChecksumCrc32c, second.ChecksumCrc32c);
        Assert.Equal(2, provider.PutCalls);
        Assert.Equal(2, provider.StoredCount);
    }

    [Fact]
    public async Task FailedPutWithoutStoredBytes_DoesNotReportSuccess()
    {
        var provider = new AmbiguousRelocationStore(commitBeforeFailure: false);
        var repository = new ZLinkProviderRelocationRepository(provider);

        var failure = await Assert.ThrowsAsync<IOException>(
            () => repository.PutRelocationAsync(
                    new byte[] { 9 },
                    TimeSpan.FromMinutes(5))
                .AsTask());

        Assert.Equal("put response lost", failure.Message);
        Assert.Equal(1, provider.PutCalls);
        Assert.Equal(0, provider.StoredCount);
    }

    [Fact]
    public async Task LostPutResponse_WithHangingProviderRead_IsBounded()
    {
        var provider = new AmbiguousRelocationStore(
            commitBeforeFailure: true,
            hangRead: true);
        var repository = new ZLinkProviderRelocationRepository(provider);
        var started = Stopwatch.GetTimestamp();

        var failure = await Assert.ThrowsAsync<IOException>(
            () => repository.PutRelocationAsync(
                    new byte[] { 7, 8 },
                    TimeSpan.FromMinutes(5))
                .AsTask());

        Assert.Equal("put response lost", failure.Message);
        Assert.InRange(
            Stopwatch.GetElapsedTime(started),
            TimeSpan.FromSeconds(4.5),
            TimeSpan.FromSeconds(7));
    }

    [Fact]
    public async Task DeterministicReference_AllowsExactRetryAndRejectsChange()
    {
        var provider = new AmbiguousRelocationStore(
            commitBeforeFailure: false,
            failNextPut: false);
        var repository = new ZLinkProviderRelocationRepository(provider);
        const string reference = "completion-receipt";

        var first = await repository.PutRelocationAtAsync(
            reference,
            new byte[] { 1, 2 },
            TimeSpan.FromHours(24));
        var retry = await repository.PutRelocationAtAsync(
            reference,
            new byte[] { 1, 2 },
            TimeSpan.FromHours(24));

        Assert.Equal(reference, first.Reference);
        Assert.Equal(reference, retry.Reference);
        await Assert.ThrowsAsync<InvalidDataException>(
            () => repository.PutRelocationAtAsync(
                    reference,
                    new byte[] { 2, 1 },
                    TimeSpan.FromHours(24))
                .AsTask());
    }

    private sealed class AmbiguousRelocationStore(
        bool commitBeforeFailure,
        bool hangRead = false,
        bool failNextPut = true) : IZLinkRelocationStore
    {
        private readonly Dictionary<string, StoredBlob> _stored =
            new(StringComparer.Ordinal);
        private readonly List<string> _putReferences = [];
        private readonly List<string> _readReferences = [];
        private bool _failNextPut = failNextPut;

        internal int PutCalls { get; private set; }
        internal int StoredCount => _stored.Count;
        internal IReadOnlyList<string> PutReferences => _putReferences;
        internal IReadOnlyList<string> ReadReferences => _readReferences;

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            PutCalls++;
            _putReferences.Add(reference.Value);
            var now = DateTimeOffset.UtcNow;
            var expiresAt = now + retention;
            if (_failNextPut)
            {
                _failNextPut = false;
                if (commitBeforeFailure)
                {
                    _stored.Add(
                        reference.Value,
                        new StoredBlob(payload.ToArray(), expiresAt, now));
                }

                throw new IOException("put response lost");
            }

            if (_stored.TryGetValue(reference.Value, out var current))
            {
                return ValueTask.FromResult<ZLinkBlobPutResult>(
                    current.Bytes.AsSpan().SequenceEqual(payload.Span)
                        ? new ZLinkBlobPutResult.AlreadyStored(
                            expiresAt,
                            now)
                        : new ZLinkBlobPutResult.Conflict(now));
            }

            _stored.Add(
                reference.Value,
                new StoredBlob(payload.ToArray(), expiresAt, now));
            return ValueTask.FromResult<ZLinkBlobPutResult>(
                new ZLinkBlobPutResult.Stored(expiresAt, now));
        }

        public async ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            _readReferences.Add(reference.Value);
            if (hangRead)
                await Task.Delay(
                    Timeout.InfiniteTimeSpan,
                    cancellationToken);
            return _stored.TryGetValue(reference.Value, out var stored)
                ? new ZLinkBlobReadResult.Found(
                    stored.Bytes,
                    stored.ExpiresAt,
                    stored.StoreNow)
                : new ZLinkBlobReadResult.Missing(
                    DateTimeOffset.UtcNow);
        }

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        private sealed record StoredBlob(
            byte[] Bytes,
            DateTimeOffset ExpiresAt,
            DateTimeOffset StoreNow);
    }
}
