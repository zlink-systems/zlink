using System.Net;
using System.Net.Sockets;
using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
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
        var inner = new ZLinkInMemoryLocationStore();
        var (store, capacityRace) = CapacityRaceLocationStore.Create(inner);
        var suffix = Guid.NewGuid().ToString("N");
        var firstEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var secondEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var sourceEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";

        await using var firstProvider = BuildServer(
            store,
            firstEndpoint,
            "z-high",
            placementWeight: 300,
            actorLimit: 1);
        await using var secondProvider = BuildServer(
            store,
            secondEndpoint,
            "a-low",
            placementWeight: 100,
            actorLimit: 1);
        await using var sourceProvider = BuildClient(
            store,
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
                inner,
                first,
                firstRid,
                firstEndpoint);
            await PublishServerDescriptorAsync(
                inner,
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
                inner.GetPlacementCapacityUsage(
                    new ZLinkMeshNodeDescriptorKey("objects", firstRid),
                    first.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration,
                    ZLinkPlacementObjectKind.Actor,
                    "player"));
            Assert.Equal(
                (0L, 1L),
                inner.GetPlacementCapacityUsage(
                    new ZLinkMeshNodeDescriptorKey("objects", secondRid),
                    second.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration,
                    ZLinkPlacementObjectKind.Actor,
                    "player"));

            Assert.IsType<ZLinkObjectAbortResult.Aborted>(
                await capacityRace.ReleaseCompetingReservationAsync());
            Assert.Equal(
                (0L, 0L),
                inner.GetPlacementCapacityUsage(
                    new ZLinkMeshNodeDescriptorKey("objects", firstRid),
                    first.GetSpotNodeRuntime("objects").Node.MeshStatus()
                        .LifecycleGeneration,
                    ZLinkPlacementObjectKind.Actor,
                    "player"));
        }
        finally
        {
            await source.StopAsync(CancellationToken.None);
            await second.StopAsync(CancellationToken.None);
            await first.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ReservationConflictRefreshesDescriptorWithinCreationDeadline()
    {
        TestActorFactory.Reset();
        var inner = new ZLinkInMemoryLocationStore();
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
            await PublishServerDescriptorAsync(inner, runtime, rid, endpoint);
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
            Assert.Equal(3, reservationRace.ActorReserveAttempts);
            Assert.Equal(1, TestActorFactory.CreateCount);
            Assert.Equal(1, TestEntrySpot.CreateCount);
            var descriptor = Assert.Single(
                (await inner.ListMeshNodesAsync("objects", default)).Items);
            Assert.Equal(1, descriptor.Capacity.Actors.Active);
            Assert.Equal(0, descriptor.Capacity.Actors.Reserved);
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
        var inner = new ZLinkInMemoryLocationStore();
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
            await PublishServerDescriptorAsync(inner, runtime, rid, endpoint);
            using var cancellation = callerCancels
                ? new CancellationTokenSource(TimeSpan.FromMilliseconds(50))
                : new CancellationTokenSource();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await provider.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate("reservation-cancelled", "player")
                    .Timeout(
                        callerCancels
                            ? TimeSpan.FromSeconds(5)
                            : TimeSpan.FromMilliseconds(50))
                    .Async(cancellation.Token));

            Assert.Equal(0, TestActorFactory.CreateCount);
            Assert.Equal(0, TestEntrySpot.CreateCount);
            var descriptor = Assert.Single(
                (await inner.ListMeshNodesAsync("objects", default)).Items);
            Assert.Equal(0, descriptor.Capacity.Actors.Active);
            Assert.Equal(0, descriptor.Capacity.Actors.Reserved);
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
        var store = new ZLinkInMemoryLocationStore();
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
                store,
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
        IZLinkLocationRepository store,
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
        IZLinkLocationRepository store,
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

    private class CapacityRaceLocationStore : DispatchProxy
    {
        private IZLinkLocationRepository _inner = null!;
        private int _actorReserveAttempts;
        private readonly object _gate = new();
        private readonly List<RoutingId> _actorReserveTargets = [];
        private ZLinkObjectReservation? _competingReservation;

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

        public static (
            IZLinkLocationRepository Store,
            CapacityRaceLocationStore Control) Create(
            IZLinkLocationRepository inner)
        {
            var contract = DispatchProxy.Create<
                IZLinkLocationRepository,
                CapacityRaceLocationStore>();
            var proxy = (CapacityRaceLocationStore)(object)contract;
            proxy._inner = inner;
            return (contract, proxy);
        }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name == nameof(IZLinkLocationRepository.ReserveAsync)
                && args is { Length: > 0 }
                && args[0] is ZLinkObjectReservationRequest
                {
                    ObjectKind: ZLinkPlacementObjectKind.Actor
                } request)
            {
                var attempt = Interlocked.Increment(ref _actorReserveAttempts);
                lock (_gate)
                    _actorReserveTargets.Add(request.TargetDescriptor.Rid);
                if (attempt == 1)
                    return ReserveAfterCompetingReservationAsync(
                        request,
                        args.Length > 1 && args[1] is CancellationToken token
                            ? token
                            : CancellationToken.None);
            }

            try
            {
                return targetMethod.Invoke(_inner, args);
            }
            catch (TargetInvocationException exception)
                when (exception.InnerException is not null)
            {
                throw exception.InnerException;
            }
        }

        public async ValueTask<ZLinkObjectAbortResult>
            ReleaseCompetingReservationAsync()
        {
            ZLinkObjectReservation reservation;
            lock (_gate)
                reservation = _competingReservation
                    ?? throw new InvalidOperationException(
                        "The competing reservation was not created.");
            return await _inner.AbortAsync(reservation);
        }

        private async ValueTask<ZLinkObjectReserveResult>
            ReserveAfterCompetingReservationAsync(
                ZLinkObjectReservationRequest request,
                CancellationToken cancellationToken)
        {
            var competing = await _inner.ReserveAsync(
                request with
                {
                    Key = new ZLinkAuthorityKey(
                        $"{request.Key.Value}:capacity-race"),
                    CreationIntentReference =
                        $"{request.CreationIntentReference}:capacity-race"
                },
                cancellationToken);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                competing);
            lock (_gate)
                _competingReservation = reserved.Reservation;
            return await _inner.ReserveAsync(request, cancellationToken);
        }
    }

    private class ReservationConflictLocationStore : DispatchProxy
    {
        private IZLinkLocationRepository _inner = null!;
        private int _actorReserveAttempts;
        private int _conflictsBeforeSuccess;

        public int ActorReserveAttempts => Volatile.Read(
            ref _actorReserveAttempts);

        public static (
            IZLinkLocationRepository Store,
            ReservationConflictLocationStore Control) Create(
            IZLinkLocationRepository inner,
            int conflictsBeforeSuccess)
        {
            var contract = DispatchProxy.Create<
                IZLinkLocationRepository,
                ReservationConflictLocationStore>();
            var proxy = (ReservationConflictLocationStore)(object)contract;
            proxy._inner = inner;
            proxy._conflictsBeforeSuccess = conflictsBeforeSuccess;
            return (contract, proxy);
        }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name == nameof(IZLinkLocationRepository.ReserveAsync)
                && args is { Length: > 0 }
                && args[0] is ZLinkObjectReservationRequest
                {
                    ObjectKind: ZLinkPlacementObjectKind.Actor
                }
                && Interlocked.Increment(ref _actorReserveAttempts)
                <= _conflictsBeforeSuccess)
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    new ZLinkObjectReserveResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(
                            DateTimeOffset.UtcNow)));

            try
            {
                return targetMethod.Invoke(_inner, args);
            }
            catch (TargetInvocationException exception)
                when (exception.InnerException is not null)
            {
                throw exception.InnerException;
            }
        }
    }
}
