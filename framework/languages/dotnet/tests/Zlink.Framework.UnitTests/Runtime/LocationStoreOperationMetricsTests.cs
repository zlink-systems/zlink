using System.Diagnostics.Metrics;
using System.Text.Json;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class LocationStoreOperationMetricsTests
{
    [Fact]
    public async Task Failed_Owner_Claim_Is_CompareExchange_Not_LeaseRenew()
    {
        var samples = new List<string>();
        using var listener = Listen(samples);
        var provider = new FailingProvider { FailReads = false };
        var runtime = new ZLinkLocationRuntime(
            new ZLinkLocationOptions(),
            new ZLinkProviderLocationRepository(provider)
        );
        Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        Assert.Equal(["compare_exchange"], samples);
        AssertClosed(samples);
    }

    [Theory]
    [InlineData("read")]
    [InlineData("lease_renew")]
    [InlineData("release")]
    public async Task Owner_Store_Failures_Use_Their_Operation(string operation)
    {
        var samples = new List<string>();
        using var listener = Listen(samples);
        var provider = new FailingProvider { Failure = null };
        var repository = new ZLinkProviderLocationRepository(provider);
        var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), repository);
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        provider.Failure = new IOException("backend unavailable");
        if (operation == "lease_renew")
            Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
        else if (operation == "read")
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await ZLinkLocationStoreRead.ExecuteAsync(
                    null,
                    "owner",
                    default,
                    token => repository.ReadOwnerLeaseAsync(runtime.OwnerId, token)
                )
            );
        else
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.RemoveDescriptorAsync(
                    new ZLinkMeshNodeDescriptorKey("mesh", Systems.Zlink.RoutingId.From("node"))
                )
            );
        Assert.Equal([operation], samples);
        AssertClosed(samples);
    }

    [Theory]
    [InlineData("put", "relocation_put")]
    [InlineData("confirmation", "relocation_put")]
    [InlineData("read", "relocation_get")]
    [InlineData("renew", "relocation_put")]
    [InlineData("delete", "relocation_delete")]
    public async Task Relocation_Backend_Failures_Use_Their_Operation(
        string request,
        string operation
    )
    {
        var samples = new List<string>();
        using var listener = Listen(samples);
        var repository = new ZLinkProviderRelocationRepository(new FailingBlobStore());
        await Assert.ThrowsAsync<IOException>(() => BlobRequest(repository, request));
        Assert.Equal([operation], samples);
        AssertClosed(samples);
    }

    [Theory]
    [InlineData("put")]
    [InlineData("read")]
    [InlineData("renew")]
    [InlineData("delete")]
    public async Task Relocation_Validation_Does_Not_Count_As_Backend_Failure(string request)
    {
        var samples = new List<string>();
        using var listener = Listen(samples);
        var repository = new ZLinkProviderRelocationRepository(
            new FailingBlobStore { Failure = new ArgumentException("invalid input") }
        );
        await Assert.ThrowsAsync<ArgumentException>(() => BlobRequest(repository, request));
        Assert.Empty(samples);
    }

    [Fact]
    public async Task Owner_Input_And_Codec_Failures_Do_Not_Count()
    {
        foreach (
            var failure in new Exception[]
            {
                new ArgumentException("input"),
                new System.Text.Json.JsonException("codec"),
            }
        )
        {
            var samples = new List<string>();
            using var listener = Listen(samples);
            var store = new CallerFailureStore(failure);
            var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), store);
            Assert.False(await runtime.RenewOwnerLeaseOnceAsync());
            Assert.Empty(samples);
        }
    }

    [Fact]
    public async Task Normal_Claim_Conflict_Does_Not_Count_But_Confirmation_Backend_Failure_Is_Read()
    {
        var samples = new List<string>();
        using var listener = Listen(samples);
        var store = new ConflictingClaimStore();
        var runtime = new ZLinkLocationRuntime(new ZLinkLocationOptions(), store);
        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(async () =>
            await runtime.RenewOwnerLeaseOnceAsync()
        );
        Assert.Empty(samples);
        store.FailRead = true;
        await Assert.ThrowsAsync<ZLinkOwnerLeaseClaimRejectedException>(async () =>
            await runtime.RenewOwnerLeaseOnceAsync()
        );
        Assert.Equal(["read"], samples);
        AssertClosed(samples);
    }

    private sealed class ConflictingClaimStore : ZLinkLocationStoreTestDouble
    {
        public bool FailRead { get; set; }

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                new ZLinkOwnerLeaseClaimResult.Conflict()
            );

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) =>
            FailRead
                ? throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "backend unavailable"
                )
                : ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                    new ZLinkOwnerLeaseReadResult.Missing()
                );
    }

    private static async Task BlobRequest(
        ZLinkProviderRelocationRepository repository,
        string request
    )
    {
        switch (request)
        {
            case "put":
                await repository.PutRelocationAtAsync(
                    "reference",
                    new byte[] { 1 },
                    TimeSpan.FromMinutes(1)
                );
                break;
            case "confirmation":
                await repository.PutRelocationAsync(new byte[] { 1 }, TimeSpan.FromMinutes(1));
                break;
            case "read":
                await repository.GetRelocationAsync("reference");
                break;
            case "renew":
                await repository.RenewRelocationAsync("reference", TimeSpan.FromMinutes(1));
                break;
            case "delete":
                await repository.DeleteRelocationAsync("reference");
                break;
        }
    }

    private sealed class CallerFailureStore(Exception failure) : ZLinkLocationStoreTestDouble
    {
        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => throw failure;

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                new ZLinkOwnerLeaseReadResult.Missing()
            );
    }

    private sealed class FailingBlobStore : IZLinkRelocationStore
    {
        public Exception Failure { get; init; } = new IOException("backend unavailable");

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default
        ) => throw Failure;

        public ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default
        ) => throw Failure;

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default
        ) => throw Failure;

        public ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default
        ) => throw Failure;
    }

    private static IDisposable Listen(List<string> samples)
    {
        var listener = new MeterListener();
        var instruments = new List<Instrument>();
        listener.InstrumentPublished = (instrument, current) =>
        {
            if (
                instrument.Meter.Name == "zlink.framework"
                && instrument.Name == "zlink.location.store.errors"
            )
            {
                instruments.Add(instrument);
                current.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>(
            (instrument, value, tags, state) =>
            {
                foreach (var tag in tags)
                    if (tag.Key == "operation")
                        samples.Add((string)tag.Value!);
            }
        );
        listener.Start();
        return new ListenerScope(listener, instruments);
    }

    private sealed class ListenerScope(MeterListener listener, List<Instrument> instruments)
        : IDisposable
    {
        public void Dispose()
        {
            foreach (var instrument in instruments)
                listener.DisableMeasurementEvents(instrument);
            listener.Dispose();
        }
    }

    private static void AssertClosed(IEnumerable<string> samples)
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        const string relative = "framework/runtime/protocol/fixtures/runtime-metrics-v1.json";
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, relative)))
            directory = directory.Parent;
        Assert.NotNull(directory);
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(Path.Combine(directory.FullName, relative))
        );
        var allowed = fixture
            .RootElement.GetProperty("metrics")
            .EnumerateArray()
            .Single(row => row.GetProperty("name").GetString() == "zlink.location.store.errors")
            .GetProperty("labelValues")
            .GetProperty("operation")
            .EnumerateArray()
            .Select(value => value.GetString())
            .ToArray();
        Assert.All(samples, sample => Assert.Contains(sample, allowed));
    }

    private sealed class FailingProvider : IZLinkLocationStore
    {
        private readonly ZLinkInMemoryProviderLocationStore inner = new();
        public bool FailReads { get; init; } = true;
        public Exception? Failure { get; set; } = new IOException("backend unavailable");

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) =>
            Failure is not null && FailReads
                ? throw Failure
                : inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => Failure is not null ? throw Failure : inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
