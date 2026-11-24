using System.Globalization;
using System.Text;
using System.Text.Json;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Tests;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class LocationStoreFormatMigrationTests
{
    private const string Mesh = "play:서울";
    private const string LegacyPrefix = "zlink:v11:capacity:";
    private static readonly ZLinkStoreKey Format = new("zlink:v12:format");
    private static readonly ZLinkStoreKey Floor = new("zlink:v12:generation-floor");
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromSeconds(30);

    [Fact]
    public async Task ConcurrentStartupConvertsEachRecordOnceAndOmitsZeroCounters()
    {
        var scenario = LocationContentionFixture.ReadScenario("concurrentStartupConversion");
        var expected = scenario.GetProperty("expected");
        var mesh = scenario.GetProperty("mesh").GetString()!;
        var startupCount = scenario.GetProperty("startupCount").GetInt32();
        var records = scenario.GetProperty("records");
        var store = new MigrationStore();
        await SeedAsync(
            store,
            "zlink:v11:object-counter",
            scenario
                .GetProperty("legacyNextObjectGeneration")
                .GetInt64()
                .ToString(CultureInfo.InvariantCulture)
        );
        await SeedAsync(
            store,
            "zlink:v11:authority-owner-counter",
            scenario
                .GetProperty("legacyNextAuthorityOwnerGeneration")
                .GetInt64()
                .ToString(CultureInfo.InvariantCulture)
        );
        const int activeSpotsWithDescriptor = 1;
        foreach (var record in records.EnumerateArray())
        {
            var descriptorHex = record.GetProperty("descriptorRoutingIdHex").GetString();
            if (descriptorHex is not null)
                await SeedDescriptorAsync(store, RoutingId.FromHex(descriptorHex), mesh);
            await SeedAsync(
                store,
                LegacyCapacity(record.GetProperty("legacyRid").GetString()!, mesh).Value,
                Capacity(
                    record.GetProperty("activeActorSlots").GetInt32(),
                    descriptorHex is null ? 0 : activeSpotsWithDescriptor,
                    record.GetProperty("pendingActorSlots").GetInt32()
                )
            );
        }
        store.GateFirstScans(startupCount);

        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var runtimes = Enumerable.Range(0, startupCount).Select(_ => Runtime(store)).ToArray();
        try
        {
            await Task.WhenAll(
                runtimes.Select(
                    (runtime, index) =>
                        runtime
                            .StartAsync(RoutingId.From($"startup-{index}"), deadline.Token)
                            .AsTask()
                )
            );

            await AssertValueAsync(store, Format, expected.GetProperty("format").GetString());
            await AssertValueAsync(
                store,
                Floor,
                expected
                    .GetProperty("generationFloor")
                    .GetInt64()
                    .ToString(CultureInfo.InvariantCulture)
            );
            foreach (var record in records.EnumerateArray())
            {
                var expectedRid = record.GetProperty("expectedRoutingIdHex").GetString()!;
                var active = record.GetProperty("activeActorSlots").GetInt32();
                var pending = record.GetProperty("pendingActorSlots").GetInt32();
                await AssertValueAsync(
                    store,
                    Counter(mesh, expectedRid, "actors", "active"),
                    active == 0 ? null : active.ToString(CultureInfo.InvariantCulture)
                );
                await AssertValueAsync(
                    store,
                    Counter(mesh, expectedRid, "actors", "pending"),
                    pending == 0 ? null : pending.ToString(CultureInfo.InvariantCulture)
                );
                if (record.GetProperty("descriptorRoutingIdHex").GetString() is not null)
                {
                    await AssertValueAsync(
                        store,
                        Counter(mesh, expectedRid, "spot-type\0userSpot\0room", "active"),
                        activeSpotsWithDescriptor.ToString(CultureInfo.InvariantCulture)
                    );
                }
                await AssertValueAsync(store, Counter(mesh, expectedRid, "spots", "pending"), null);
                await AssertValueAsync(
                    store,
                    LegacyCapacity(record.GetProperty("legacyRid").GetString()!, mesh),
                    null
                );
            }
            var remaining = Assert.IsType<ZLinkStoreScanResult.Page>(
                await store.Inner.ScanAsync(new ZLinkStoreScanRequest(LegacyPrefix, null, 1000))
            );
            Assert.Equal(
                expected.GetProperty("remainingLegacyRecords").GetInt32(),
                remaining.Value.Items.Count
            );
            Assert.Equal(records.GetArrayLength(), store.ConvertedRecords);
            Assert.Equal(
                expected.GetProperty("writesBeforeConversion").GetInt32(),
                store.WritesBeforeFormat
            );
        }
        finally
        {
            await Task.WhenAll(runtimes.Select(runtime => runtime.DisposeAsync().AsTask()));
        }
    }

    [Fact]
    public async Task StartupResumesAfterCancellationFollowingOneAppliedRecord()
    {
        var store = new MigrationStore();
        await SeedAsync(store, LegacyCapacity("a").Value, Capacity(1, 0, 0));
        await SeedAsync(store, LegacyCapacity("b").Value, Capacity(3, 0, 0));
        using var cancellation = new CancellationTokenSource();
        store.InterruptAfterRecord = cancellation;

        await using (var interrupted = Runtime(store))
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                interrupted.StartAsync(RoutingId.From("interrupted"), cancellation.Token).AsTask()
            );

        await AssertValueAsync(store, Format, null);
        await AssertValueAsync(store, Floor, "1");
        Assert.Equal(1, store.ConvertedRecords);
        Assert.Equal(0, store.WritesBeforeFormat);
        await using var resumed = Runtime(store);
        await resumed.StartAsync(RoutingId.From("resumed"));

        await AssertValueAsync(store, Format, "converted");
        await AssertValueAsync(store, Counter(RoutingId.From("a"), "actors", "active"), "1");
        await AssertValueAsync(store, Counter(RoutingId.From("b"), "actors", "active"), "3");
        await AssertValueAsync(store, LegacyCapacity("a"), null);
        await AssertValueAsync(store, LegacyCapacity("b"), null);
        Assert.Equal(2, store.ConvertedRecords);
        Assert.Equal(0, store.WritesBeforeFormat);
    }

    [Fact]
    public async Task StartupChecksDestinationsAfterLosingTheAppliedWriteResponse()
    {
        var store = new MigrationStore { LoseResponseAfterRecord = true };
        var legacyKey = LegacyCapacity("node");
        await SeedAsync(store, legacyKey.Value, Capacity(1, 0, 0));
        await using var runtime = Runtime(store);

        await runtime.StartAsync(RoutingId.From("replacement"));

        await AssertValueAsync(store, Format, "converted");
        await AssertValueAsync(store, legacyKey, null);
        await AssertValueAsync(store, Counter(RoutingId.From("node"), "actors", "active"), "1");
        Assert.Equal(1, store.ConvertedRecords);
        Assert.Equal(0, store.WritesBeforeFormat);
    }

    [Fact]
    public async Task StartupRejectsAmbiguousLegacyRid()
    {
        var store = new MigrationStore();
        var binaryRid = RoutingId.From(1u);
        var textRid = RoutingId.From("1");
        Assert.Equal(binaryRid.ToString(), textRid.ToString());
        await SeedDescriptorAsync(store, binaryRid);
        await SeedDescriptorAsync(store, textRid);
        var legacyKey = LegacyCapacity(textRid.ToString());
        var legacyValue = Capacity(1, 0, 0);
        await SeedAsync(store, legacyKey.Value, legacyValue);
        await using var runtime = Runtime(store);

        var error = await Assert.ThrowsAsync<ZLinkConfigurationException>(() =>
            runtime.StartAsync(RoutingId.From("node")).AsTask()
        );
        Assert.Contains("ambiguous", error.Message, StringComparison.OrdinalIgnoreCase);
        await AssertValueAsync(store, legacyKey, legacyValue);
        await AssertValueAsync(store, Format, null);
        await AssertValueAsync(store, Counter(binaryRid, "actors", "active"), null);
        await AssertValueAsync(store, Counter(textRid, "actors", "active"), null);
    }

    [Fact]
    public async Task StartupRejectsRecordOverKeyLimitWithoutSplitting()
    {
        var store = new MigrationStore();
        var legacyKey = LegacyCapacity("node");
        var types = Enumerable
            .Range(0, 1024)
            .ToDictionary(index => $"instance_spot\0type-{index}", _ => 1);
        var legacyValue = JsonSerializer.Serialize(
            new
            {
                active = new
                {
                    actors = 0,
                    spots = 1024,
                    spotTypes = types,
                },
                pending = new
                {
                    actors = 0,
                    spots = 1024,
                    spotTypes = types,
                },
            }
        );
        await SeedAsync(store, legacyKey.Value, legacyValue);
        await using var runtime = Runtime(store);

        var error = await Assert.ThrowsAsync<ZLinkConfigurationException>(() =>
            runtime.StartAsync(RoutingId.From("node")).AsTask()
        );
        Assert.Contains("key limit", error.Message, StringComparison.OrdinalIgnoreCase);
        await AssertValueAsync(store, legacyKey, legacyValue);
        await AssertValueAsync(store, Format, null);
        await AssertValueAsync(store, Counter(RoutingId.From("node"), "spots", "active"), null);
        Assert.Equal(0, store.ConvertedRecords);
    }

    private static ZLinkLocationRuntime Runtime(MigrationStore store) =>
        new(new ZLinkLocationOptions(), new ZLinkProviderLocationRepository(store));

    private static ZLinkStoreKey Counter(RoutingId rid, string dimension, string state) =>
        Counter(Mesh, rid.ToHex(), dimension, state);

    private static ZLinkStoreKey Counter(
        string mesh,
        string ridHex,
        string dimension,
        string state
    ) => new($"zlink:v12:capacity\0{mesh}\0{ridHex}\0{dimension}\0{state}");

    private static ZLinkStoreKey LegacyCapacity(string rid, string mesh = Mesh) =>
        new($"{LegacyPrefix}{Uri.EscapeDataString(mesh)}:{Uri.EscapeDataString(rid)}");

    private static string Capacity(int activeActors, int activeSpots, int pendingActors) =>
        JsonSerializer.Serialize(
            new
            {
                active = new
                {
                    actors = activeActors,
                    spots = activeSpots,
                    spotTypes = new Dictionary<string, int> { ["user_spot\0room"] = activeSpots },
                },
                pending = new
                {
                    actors = pendingActors,
                    spots = 0,
                    spotTypes = new Dictionary<string, int> { ["user_spot\0room"] = 0 },
                },
            }
        );

    private static async Task SeedDescriptorAsync(
        MigrationStore store,
        RoutingId rid,
        string mesh = Mesh
    )
    {
        var repository = new ZLinkProviderLocationRepository(store.Inner);
        var owner = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync(Guid.NewGuid().ToString("n"), LeaseTtl)
            )
            .Token;
        var descriptor = new ZLinkMeshNodeDescriptor(
            mesh,
            rid,
            1,
            1,
            "tcp://127.0.0.1:12345",
            new Dictionary<string, int>(StringComparer.Ordinal) { [mesh] = 100 },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default
        );
        var result = await repository.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim
        );
        Assert.Equal(ZLinkLocationWriteStatus.Stored, result.Status);
    }

    private static async Task SeedAsync(MigrationStore store, string key, string value) =>
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.Inner.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Missing(new(key))],
                    [new ZLinkStoreMutation.Put(new(key), Encoding.UTF8.GetBytes(value), null)]
                )
            )
        );

    private static async Task AssertValueAsync(
        MigrationStore store,
        ZLinkStoreKey key,
        string? expected
    )
    {
        var result = await store.Inner.ReadAsync(key);
        if (expected is null)
            Assert.IsType<ZLinkStoreReadResult.Missing>(result);
        else
            Assert.Equal(
                expected,
                Encoding.UTF8.GetString(
                    Assert.IsType<ZLinkStoreReadResult.Found>(result).Value.Bytes.Span
                )
            );
    }

    private sealed class MigrationStore : IZLinkLocationStore
    {
        private readonly TaskCompletionSource _scanGate = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _scansToGate;
        private int _convertedRecords;
        private int _earlyWrites;
        public ZLinkInMemoryProviderLocationStore Inner { get; } = new();
        public CancellationTokenSource? InterruptAfterRecord { get; set; }
        public bool LoseResponseAfterRecord { get; init; }
        public int ConvertedRecords => Volatile.Read(ref _convertedRecords);
        public int WritesBeforeFormat => Volatile.Read(ref _earlyWrites);

        public void GateFirstScans(int expected) => _scansToGate = expected;

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => Inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var result = await Inner.ScanAsync(request, cancellationToken);
            if (request.Prefix == LegacyPrefix && Volatile.Read(ref _scansToGate) > 0)
            {
                if (Interlocked.Decrement(ref _scansToGate) == 0)
                    _scanGate.TrySetResult();
                await _scanGate.Task.WaitAsync(cancellationToken);
            }
            return result;
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var convertsRecord = request
                .Mutations.OfType<ZLinkStoreMutation.Delete>()
                .Any(mutation =>
                    mutation.Key.Value.StartsWith(LegacyPrefix, StringComparison.Ordinal)
                );
            var writesFormatMetadata =
                request.Mutations is [ZLinkStoreMutation.Put put]
                && (put.Key == Floor || put.Key == Format);
            if (
                !convertsRecord
                && !writesFormatMetadata
                && await Inner.ReadAsync(Format, cancellationToken) is ZLinkStoreReadResult.Missing
            )
                Interlocked.Increment(ref _earlyWrites);

            var result = await Inner.WriteAsync(request, cancellationToken);
            if (result is ZLinkStoreWriteResult.Applied && convertsRecord)
            {
                var converted = Interlocked.Increment(ref _convertedRecords);
                if (InterruptAfterRecord is { IsCancellationRequested: false } interrupt)
                {
                    interrupt.Cancel();
                    cancellationToken.ThrowIfCancellationRequested();
                }
                if (LoseResponseAfterRecord && converted == 1)
                    throw new IOException("The response was lost after committing the batch.");
            }
            return result;
        }
    }
}
