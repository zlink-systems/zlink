using System.Net;
using System.Net.Sockets;
using System.Globalization;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class ActorManagerProductionTests
{
    [Fact]
    public async Task CapacityRaceReleasesLosingReservationAndRunsOnlyFinalFactory()
    {
        TestActorFactory.Reset();
        var inner = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(inner);
        var capacityRace = new CapacityRaceLocationStore(inner);
        var suffix = Guid.NewGuid().ToString("N");
        var firstEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var secondEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var sourceEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";

        await using var firstProvider = BuildServer(
            capacityRace,
            firstEndpoint,
            "z-high",
            placementWeight: 300,
            actorLimit: 1);
        await using var secondProvider = BuildServer(
            capacityRace,
            secondEndpoint,
            "a-low",
            placementWeight: 100,
            actorLimit: 1);
        await using var sourceProvider = BuildClient(
            capacityRace,
            sourceEndpoint);
        var first = firstProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var second = secondProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var source = sourceProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var firstHost = FrameworkHost(firstProvider);
        var secondHost = FrameworkHost(secondProvider);
        var sourceHost = FrameworkHost(sourceProvider);
        await firstHost.StartAsync(CancellationToken.None);
        await secondHost.StartAsync(CancellationToken.None);
        await sourceHost.StartAsync(CancellationToken.None);
        try
        {
            var firstRid = first.GetSpotNodeRuntime("objects").Node.RoutingId;
            var secondRid = second.GetSpotNodeRuntime("objects").Node.RoutingId;
            await PublishServerDescriptorAsync(
                repository,
                first,
                firstRid,
                firstEndpoint);
            await PublishServerDescriptorAsync(
                repository,
                second,
                secondRid,
                secondEndpoint);
            await WaitUntilAsync(() =>
                source.GetSpotNodeRuntime("objects").Node.MeshStatus()
                    .AdmittedPeerCount == 2);

            using var timeout = new CancellationTokenSource(
                TimeSpan.FromSeconds(10));
            var result = await new ZLinkActorManagerService(source)
                .GetOrCreate($"actor-{suffix}", "player")
                .Async(timeout.Token);

            Assert.IsType<ZLinkActorCreateResult.Created>(result);
            Assert.Equal(2, capacityRace.ActorReserveAttempts);
            Assert.Equal(
                [firstRid, secondRid],
                capacityRace.ActorReserveTargets);
            Assert.Equal(1, TestActorFactory.CreateCount);
            Assert.Equal(1, TestEntrySpot.CreateCount);
            Assert.Equal(
                (1L, 0L),
                await ReadActorCapacityUsageAsync(
                    inner,
                    new ZLinkMeshNodeDescriptorKey("objects", firstRid),
                    first.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration));
            Assert.Equal(
                (0L, 1L),
                await ReadActorCapacityUsageAsync(
                    inner,
                    new ZLinkMeshNodeDescriptorKey("objects", secondRid),
                    second.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration));

            Assert.True(await capacityRace.ReleaseCompetingCapacityAsync());
            Assert.Equal(
                (0L, 0L),
                await ReadActorCapacityUsageAsync(
                    inner,
                    new ZLinkMeshNodeDescriptorKey("objects", firstRid),
                    first.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration));
        }
        finally
        {
            await source.StopAsync(CancellationToken.None);
            await second.StopAsync(CancellationToken.None);
            await first.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ReservationWriteConflictRetriesWithinCreationDeadline()
    {
        TestActorFactory.Reset();
        var inner = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(inner);
        var (store, reservationRace) =
            ReservationConflictLocationStore.Create(inner, conflictsBeforeSuccess: 1);
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        await using var provider = BuildServer(store, endpoint);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var host = FrameworkHost(provider);
        await host.StartAsync(CancellationToken.None);
        try
        {
            var rid = runtime.GetSpotNodeRuntime("objects").Node.RoutingId;
            await PublishServerDescriptorAsync(repository, runtime, rid, endpoint);
            var actors = provider.GetRequiredService<IZLinkActorManager>();
            var actorId = $"reservation-race-{suffix}";

            var created = Assert.IsType<ZLinkActorCreateResult.Created>(
                await actors.GetOrCreate(actorId, "player")
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async());
            var existing = Assert.IsType<ZLinkActorCreateResult.Existing>(
                await actors.GetOrCreate(actorId, "player")
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async());

            Assert.Equal(created.Actor, existing.Actor);
            Assert.Equal(2, reservationRace.ActorReserveAttempts);
            Assert.Equal(1, TestActorFactory.CreateCount);
            Assert.Equal(1, TestEntrySpot.CreateCount);
            Assert.Equal(
                (0L, 1L),
                await ReadActorCapacityUsageAsync(
                    inner,
                    new ZLinkMeshNodeDescriptorKey("objects", rid),
                    runtime.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task PersistentReservationConflictHonorsCancellationAndDeadline(
        bool callerCancels)
    {
        TestActorFactory.Reset();
        var inner = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(inner);
        var (store, _) = ReservationConflictLocationStore.Create(
            inner,
            conflictsBeforeSuccess: int.MaxValue);
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        await using var provider = BuildServer(store, endpoint);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var host = FrameworkHost(provider);
        await host.StartAsync(CancellationToken.None);
        try
        {
            var rid = runtime.GetSpotNodeRuntime("objects").Node.RoutingId;
            await PublishServerDescriptorAsync(repository, runtime, rid, endpoint);
            using var cancellation = callerCancels
                ? new CancellationTokenSource(TimeSpan.FromMilliseconds(50))
                : new CancellationTokenSource();

            async Task SubmitAsync() =>
                _ = await provider.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate("reservation-cancelled", "player")
                    .Timeout(
                        callerCancels
                            ? TimeSpan.FromSeconds(5)
                            : TimeSpan.FromMilliseconds(50))
                    .Async(cancellation.Token);

            if (callerCancels)
            {
                await Assert.ThrowsAnyAsync<OperationCanceledException>(SubmitAsync);
            }
            else
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(SubmitAsync);
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
            }

            Assert.Equal(0, TestActorFactory.CreateCount);
            Assert.Equal(0, TestEntrySpot.CreateCount);
            Assert.Equal(
                (0L, 0L),
                await ReadActorCapacityUsageAsync(
                    inner,
                    new ZLinkMeshNodeDescriptorKey("objects", rid),
                    runtime.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SameProcessServerExecutesLocalProductionActorTarget()
    {
        TestActorFactory.Reset();
        var store = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(store);
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        await using var provider = BuildServer(store, endpoint);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var host = FrameworkHost(provider);
        Assert.NotNull(provider.GetService<TestActorFactory>());
        await host.StartAsync(CancellationToken.None);
        try
        {
            var rid = runtime.GetSpotNodeRuntime("objects").Node.RoutingId;
            await PublishServerDescriptorAsync(
                repository,
                runtime,
                rid,
                endpoint);

            using var timeout = new CancellationTokenSource(
                TimeSpan.FromSeconds(10));
            var result = Assert.IsType<ZLinkActorCreateResult.Created>(
                await provider.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate($"local-{suffix}", "player")
                    .Async(timeout.Token));

            Assert.Equal(rid, result.Actor.NodeRid);
            Assert.Equal(1, TestActorFactory.CreateCount);
            Assert.Equal(1, TestEntrySpot.CreateCount);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static IHostedService FrameworkHost(ServiceProvider provider) =>
        provider.GetServices<IHostedService>().Single(service =>
            service.GetType().Name == "ZLinkFrameworkHostedService");

    private static ServiceProvider BuildServer(
        IZLinkLocationStore store,
        string endpoint,
        string? routingIdPrefix = null,
        int placementWeight = 100,
        int actorLimit = 0)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            var node = options.AddRouteMesh("objects")
                .Listen(endpoint);
            if (routingIdPrefix is not null)
                node.SetRoutingIdPrefix(routingIdPrefix);
            node.SetPlacementWeight(placementWeight);
            node.SetActorLimit(actorLimit);
            node.Channel("objects").Server();
            node.Objects().Server()
                .AddEntrySpot<TestEntrySpot>()
                .AddActorFactory<TestActor, TestActorFactory>(
                    "player", factory => factory.DisableRelocation());
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider BuildClient(
        IZLinkLocationStore store,
        string endpoint)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            var node = options.AddRouteMesh("objects")
                .Listen(endpoint);
            node.Channel("objects").Client();
            node.Objects().Client();
        });
        return services.BuildServiceProvider();
    }

    private static int FindFreeTcpPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private static async Task PublishServerDescriptorAsync(
        IZLinkLocationRepository store,
        ZLinkFrameworkRuntime runtime,
        RoutingId rid,
        string endpoint)
    {
        var node = runtime.GetSpotNodeRuntime("objects");
        ZLinkMeshNodeDescriptor? published = null;
        await WaitUntilAsync(async () =>
        {
            published = (await store.ListMeshNodesAsync("objects", default))
                .Items
                .SingleOrDefault(descriptor => descriptor.Rid == rid);
            return published is
            {
                State: ZLinkFrameworkRuntimeState.Serving
            } && published.Endpoint == endpoint;
        });
        var generation = node.Node.MeshStatus().LifecycleGeneration;
        Assert.Equal(generation, published!.LifecycleGeneration);
        Assert.False(string.IsNullOrEmpty(published.EntrySpotId));
        Assert.True(published.PlacementWeight > 0);
    }

    private static async ValueTask<(long Pending, long Active)>
        ReadActorCapacityUsageAsync(
            IZLinkLocationStore store,
            ZLinkMeshNodeDescriptorKey descriptor,
            ulong lifecycleGeneration)
    {
        var key = CapacityKey(descriptor, lifecycleGeneration);
        var read = await store.ReadAsync(key);
        if (read is ZLinkStoreReadResult.Missing)
            return (0L, 0L);

        var found = Assert.IsType<ZLinkStoreReadResult.Found>(read);
        using var document = JsonDocument.Parse(found.Value.Bytes);
        var root = document.RootElement;
        return (
            root.GetProperty("actorsPending").GetInt64(),
            root.GetProperty("actorsActive").GetInt64());
    }

    private static ZLinkStoreKey CapacityKey(
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong lifecycleGeneration) =>
        new("zlink:v11:capacity:"
            + EncodeSegment(descriptor.MeshName)
            + EncodeSegment(descriptor.Rid.ToHex())
            + lifecycleGeneration.ToString(CultureInfo.InvariantCulture));

    private static string EncodeSegment(string value) =>
        System.Text.Encoding.UTF8.GetByteCount(value).ToString(
            CultureInfo.InvariantCulture) + ":" + value + ":";

    private static bool TryGetActorReservationTransaction(
        ZLinkStoreWriteRequest request,
        out ZLinkStoreMutation.Put capacity,
        out ZLinkStoreCondition capacityCondition,
        out RoutingId targetRid)
    {
        capacity = null!;
        capacityCondition = null!;
        targetRid = default;

        var reservation = request.Mutations
            .OfType<ZLinkStoreMutation.Put>()
            .SingleOrDefault(mutation => mutation.Key.Value.StartsWith(
                "zlink:v11:creation-reservation:",
                StringComparison.Ordinal));
        if (reservation is null)
            return false;

        capacity = request.Mutations
            .OfType<ZLinkStoreMutation.Put>()
            .Single(mutation => mutation.Key.Value.StartsWith(
                "zlink:v11:capacity:",
                StringComparison.Ordinal));
        using (var capacityDocument = JsonDocument.Parse(capacity.Bytes))
        {
            if (capacityDocument.RootElement
                    .GetProperty("actorsPending")
                    .GetInt32() == 0)
                return false;
        }

        var capacityKey = capacity.Key;
        capacityCondition = request.Conditions.Single(condition =>
            condition switch
            {
                ZLinkStoreCondition.Missing missing =>
                    missing.Key == capacityKey,
                ZLinkStoreCondition.Version version =>
                    version.Key == capacityKey,
                _ => false
            });
        using var reservationDocument = JsonDocument.Parse(reservation.Bytes);
        targetRid = RoutingId.FromHex(
            reservationDocument.RootElement
                .GetProperty("targetDescriptor")
                .GetProperty("rid")
                .GetString()!);
        return true;
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    private static async Task WaitUntilAsync(
        Func<Task<bool>> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!await condition().ConfigureAwait(false))
            await Task.Delay(10, timeout.Token);
    }

    private sealed class TestActor(
        string actorId,
        IZLinkActorContext context) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class TestActorFactory : IZLinkActorFactory<TestActor>
    {
        private static int _createCount;

        public static int CreateCount => Volatile.Read(ref _createCount);

        public static void Reset()
        {
            Volatile.Write(ref _createCount, 0);
            TestEntrySpot.Reset();
        }

        public ValueTask<TestActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref _createCount);
            return ValueTask.FromResult(new TestActor(context.ActorId, context));
        }
    }

    private sealed class TestEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<TestActor>
    {
        private static int _createCount;

        public IZLinkEntrySpotContext Context { get; } = context;

        public static int CreateCount => Volatile.Read(ref _createCount);

        public static void Reset() => Volatile.Write(ref _createCount, 0);

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            TestActor actor,
            ZLinkMessage createRequest,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref _createCount);
            return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
        }

        public ValueTask OnJoinedActorAsync(
            TestActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            TestActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class CapacityRaceLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _actorReserveAttempts;
        private readonly object _gate = new();
        private readonly List<RoutingId> _actorReserveTargets = [];
        private ZLinkStoreKey? _competingCapacityKey;

        public int ActorReserveAttempts => Volatile.Read(
            ref _actorReserveAttempts);

        public IReadOnlyList<RoutingId> ActorReserveTargets
        {
            get
            {
                lock (_gate)
                    return _actorReserveTargets.ToArray();
            }
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            if (!TryGetActorReservationTransaction(
                    request,
                    out var capacity,
                    out var capacityCondition,
                    out var targetRid))
                return await inner.WriteAsync(request, cancellationToken);

            var attempt = Interlocked.Increment(ref _actorReserveAttempts);
            lock (_gate)
                _actorReserveTargets.Add(targetRid);
            if (attempt != 1)
                return await inner.WriteAsync(request, cancellationToken);

            var competing = await inner.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [capacityCondition],
                    [capacity]),
                cancellationToken);
            var applied = Assert.IsType<ZLinkStoreWriteResult.Applied>(competing);
            lock (_gate)
                _competingCapacityKey = capacity.Key;
            return new ZLinkStoreWriteResult.Conflict(applied.StoreNow);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);

        public async ValueTask<bool> ReleaseCompetingCapacityAsync()
        {
            ZLinkStoreKey key;
            lock (_gate)
                key = _competingCapacityKey
                    ?? throw new InvalidOperationException(
                        "The competing capacity claim was not created.");
            var read = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(key));
            var release = await inner.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, read.Value.Version)],
                    [new ZLinkStoreMutation.Delete(key)]));
            return release is ZLinkStoreWriteResult.Applied;
        }
    }

    private sealed class ReservationConflictLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _actorReserveAttempts;
        private int _conflictsBeforeSuccess;

        public int ActorReserveAttempts => Volatile.Read(
            ref _actorReserveAttempts);

        public static (
            IZLinkLocationStore Store,
            ReservationConflictLocationStore Control) Create(
            IZLinkLocationStore inner,
            int conflictsBeforeSuccess)
        {
            var proxy = new ReservationConflictLocationStore(inner);
            proxy._conflictsBeforeSuccess = conflictsBeforeSuccess;
            return (proxy, proxy);
        }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            if (TryGetActorReservationTransaction(
                    request,
                    out _,
                    out _,
                    out _)
                && Interlocked.Increment(ref _actorReserveAttempts)
                <= _conflictsBeforeSuccess)
                return ValueTask.FromResult<ZLinkStoreWriteResult>(
                    new ZLinkStoreWriteResult.Conflict(DateTimeOffset.UtcNow));

            return inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }
}
