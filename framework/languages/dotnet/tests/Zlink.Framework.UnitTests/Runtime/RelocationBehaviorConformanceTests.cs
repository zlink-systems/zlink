using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RelocationBehaviorConformanceTests
{
    [Fact]
    public async Task ActorJoin_target_callback_push_converges_after_completed_without_waiting_for_discovery_poll()
    {
        var trace = new RelocationBehaviorTrace
        {
            SendTargetLifecyclePush = true
        };
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-push-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-push-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        var stream = new RelocationBehaviorStream(
            RoutingId.From($"behavior-session-{Guid.NewGuid():N}"));
        var session = new ZLinkSessionContext(
            source.Runtime,
            stream,
            new RelocationBehaviorSessionHandlers(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        _ = await session.Actors.BindAsync(created.Actor);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            pollingInterval: TimeSpan.FromSeconds(3));
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetLifecycleStarted");
        trace.ReleaseTargetLifecycle.TrySetResult();
        await trace.WaitAsync("publicJoinCompleted");

        try
        {
            await stream.FirstWrite.Task.WaitAsync(TimeSpan.FromSeconds(1));
            Assert.Equal(1, stream.WriteCount);
        }
        finally
        {
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_runtime_preserves_observable_order_and_exact_callback_counts()
    {
        using var fixture = LoadBehaviorFixture();
        Assert.Equal(
            "zlink.framework.relocation-behavior",
            fixture.RootElement.GetProperty("fixture").GetString());

        var trace = new RelocationBehaviorTrace();
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);
        trace.TargetNodeRid = target.LocalNodeRid;

        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        var join = client.RequestToActor(
                actorId,
                new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");

        var saved = client.RequestToActor(actorId, new BehaviorWork("saved"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        Assert.False(saved.IsCompleted);
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));

        await trace.WaitAsync("targetLifecycleStarted");
        var temporary = client.RequestToActor(
                actorId,
                new BehaviorWork("temporary"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        try
        {
        await Task.Delay(50);
        Assert.DoesNotContain(
            trace.Events,
            value => value is "sourceMembershipLeaveSubmitted"
                or "publicJoinCompleted"
                or "savedWorkAdmitted"
                or "temporaryWorkAdmitted");

        trace.ReleaseTargetLifecycle.TrySetResult();
        await trace.WaitAsync("sourceMembershipLeaveSubmitted");
        var completionLeftCanonicalAuthority = false;
        try
        {
            await trace.WaitAsync("publicJoinCompleted");
            var completedAuthority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(locationStore)
                    .ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)));
            completionLeftCanonicalAuthority =
                ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    completedAuthority.Snapshot.Payload.Span,
                    out _);
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);
            _ = await saved.WaitAsync(TimeSpan.FromSeconds(15));
            _ = await temporary.WaitAsync(TimeSpan.FromSeconds(15));
            await trace.WaitForTargetAuthorityAsync();
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);

            var direct = await client.RequestToActor(
                    actorId,
                    new BehaviorWork("direct"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>();
            Assert.Equal("direct", direct.Marker);
        }
        finally
        {
            trace.ReleaseSourceLeave.TrySetResult();
        }

        await WaitUntilAsync(async () =>
        {
            var current = await actorManager.FindAsync(actorId);
            return current is { } actor && actor.NodeRid == target.LocalNodeRid;
        });

        Assert.Equal(
            ["saved", "temporary", "direct"],
            trace.DeliveredMarkers);
        Assert.Equal(trace.SourceObjectGeneration, trace.TargetObjectGeneration);
        Assert.True(trace.TargetOwnerGeneration > trace.SourceOwnerGeneration);
        Assert.False(
            completionLeftCanonicalAuthority,
            "An unbound ActorJoin must normalize the completed relocation "
            + "before the public join completion can outlive its target process.");
        AssertObservedBehavior(fixture.RootElement, trace.Events);
        }
        finally
        {
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    private static void AssertObservedBehavior(
        JsonElement fixture,
        IReadOnlyList<string> events)
    {
        string[] observedRequired =
        [
            "relocationRequested",
            "sourceStateCaptured",
            "targetStateRestored",
            "targetLifecycleCompleted",
            "sourceMembershipLeaveSubmitted",
            "publicJoinCompleted",
            "savedWorkAdmitted",
            "temporaryWorkAdmitted"
        ];
        var actorJoin = fixture.GetProperty("profiles")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == "actorJoin");
        var required = fixture.GetProperty("commonLifecycle")
            .GetProperty("requiredEvents")
            .EnumerateArray()
            .Select(static item => item.GetString()!)
            .Concat(actorJoin.GetProperty("requiredEvents")
                .EnumerateArray()
                .Select(static item => item.GetString()!))
            .ToArray();
        Assert.All(observedRequired, name => Assert.Contains(name, required));
        Assert.All(
            observedRequired,
            name => Assert.Equal(1, events.Count(value => value == name)));

        var order = fixture.GetProperty("commonLifecycle")
            .GetProperty("requiredOrder")
            .EnumerateArray()
            .Concat(actorJoin.GetProperty("additionalOrder").EnumerateArray())
            .Select(edge => edge.EnumerateArray()
                .Select(static item => item.GetString()!)
                .ToArray())
            .ToArray();
        foreach (var before in observedRequired)
        {
            foreach (var after in observedRequired)
            {
                if (!IsRequiredBefore(before, after, order)) continue;
                Assert.True(
                    events.IndexOf(before) < events.IndexOf(after),
                    $"Expected {before} before {after}: {string.Join(',', events)}");
            }
        }

        var counts = actorJoin.GetProperty("callbackCounts");
        Assert.Equal(
            counts.GetProperty("targetMembershipJoin").GetInt32(),
            events.Count(value => value == "targetMembershipJoinCallback"));
        Assert.Equal(
            counts.GetProperty("sourceMembershipLeave").GetInt32(),
            events.Count(value => value == "sourceMembershipLeaveSubmitted"));
        var terminal = actorJoin.GetProperty("terminalEvent").GetString()!;
        Assert.Equal(1, events.Count(value => value == terminal));
    }

    private static bool IsRequiredBefore(
        string before,
        string after,
        IReadOnlyList<string[]> order)
    {
        if (string.Equals(before, after, StringComparison.Ordinal)) return false;
        var pending = new Queue<string>();
        var visited = new HashSet<string>(StringComparer.Ordinal) { before };
        pending.Enqueue(before);
        while (pending.TryDequeue(out var current))
        {
            foreach (var edge in order.Where(edge => edge[0] == current))
            {
                if (edge[1] == after) return true;
                if (visited.Add(edge[1])) pending.Enqueue(edge[1]);
            }
        }
        return false;
    }

    private static JsonDocument LoadBehaviorFixture()
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "relocation-behavior-v1.json");
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private static async Task WaitUntilAsync(Func<bool> predicate) =>
        await WaitUntilAsync(() => ValueTask.FromResult(predicate()));

    private static async Task WaitUntilAsync(Func<ValueTask<bool>> predicate)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            if (await predicate()) return;
            await Task.Delay(10);
        }
        Assert.True(await predicate(), "The runtime did not reach the expected behavior state.");
    }
}

internal sealed class RelocationBehaviorTrace
{
    private readonly object _gate = new();
    private readonly List<string> _events = [];
    private readonly List<string> _deliveredMarkers = [];
    private readonly ConcurrentDictionary<string, TaskCompletionSource> _signals =
        new(StringComparer.Ordinal);

    internal string? ActorId { get; set; }
    internal RoutingId SourceNodeRid { get; set; }
    internal RoutingId TargetNodeRid { get; set; }
    internal ulong SourceObjectGeneration { get; set; }
    internal ulong TargetObjectGeneration { get; private set; }
    internal ulong SourceOwnerGeneration { get; private set; }
    internal ulong TargetOwnerGeneration { get; private set; }
    internal int TargetLifecycleAttemptCount;
    internal bool SendTargetLifecyclePush { get; init; }
    internal TaskCompletionSource ReleaseJoinHandler { get; } = Signal();
    internal TaskCompletionSource ReleaseTargetLifecycle { get; } = Signal();
    internal TaskCompletionSource ReleaseSourceLeave { get; } = Signal();
    private TaskCompletionSource TargetAuthorityObserved { get; } = Signal();

    internal IReadOnlyList<string> Events
    {
        get { lock (_gate) return _events.ToArray(); }
    }

    internal IReadOnlyList<string> DeliveredMarkers
    {
        get { lock (_gate) return _deliveredMarkers.ToArray(); }
    }

    internal void Record(string name)
    {
        lock (_gate) _events.Add(name);
        _signals.GetOrAdd(name, static _ => Signal()).TrySetResult();
    }

    internal void RecordDelivery(string marker, string node)
    {
        Assert.Equal("target", node);
        lock (_gate) _deliveredMarkers.Add(marker);
        Record(marker switch
        {
            "saved" => "savedWorkAdmitted",
            "temporary" => "temporaryWorkAdmitted",
            "direct" => "directWorkAdmitted",
            _ => throw new InvalidOperationException($"Unknown marker '{marker}'.")
        });
    }

    internal async Task WaitAsync(string name) =>
        await _signals.GetOrAdd(name, static _ => Signal()).Task
            .WaitAsync(TimeSpan.FromSeconds(15));

    internal async Task WaitForTargetAuthorityAsync() =>
        await TargetAuthorityObserved.Task.WaitAsync(TimeSpan.FromSeconds(15));

    internal void ObserveReadyActorAuthority(ZLinkAuthoritySnapshot snapshot)
    {
        if (!ZLinkActorAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.State != ZLinkActorAuthorityState.Ready
            || authority.ActorId != ActorId)
            return;
        if (authority.NodeRid == SourceNodeRid)
        {
            SourceOwnerGeneration = snapshot.AuthorityOwnerGeneration;
            return;
        }
        if (authority.NodeRid != TargetNodeRid) return;
        TargetObjectGeneration = snapshot.ObjectGeneration;
        TargetOwnerGeneration = snapshot.AuthorityOwnerGeneration;
        TargetAuthorityObserved.TrySetResult();
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class RecordingLocationStore(
    IZLinkLocationStore inner,
    RelocationBehaviorTrace trace) : IZLinkLocationStore
{
    private readonly ZLinkProviderLocationRepository _reader = new(inner);

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default) =>
        inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        var result = await inner.WriteAsync(request, cancellationToken);
        if (result is ZLinkStoreWriteResult.Applied && trace.ActorId is { } actorId)
        {
            var read = await _reader.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                cancellationToken);
            if (read is ZLinkAuthorityReadResult.Found found)
                trace.ObserveReadyActorAuthority(found.Snapshot);
        }
        return result;
    }

    internal async ValueTask ObserveActorAuthorityAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        var read = await _reader.ReadAuthorityAsync(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
            cancellationToken);
        if (read is ZLinkAuthorityReadResult.Found found)
            trace.ObserveReadyActorAuthority(found.Snapshot);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);
}

internal sealed class SynchronizedRelocationStore :
    IZLinkRelocationRepository,
    IZLinkRelocationStore
{
    private readonly object _gate = new();
    private readonly InMemoryRelocationStore _inner = new();

    public ValueTask<ZLinkBlobPutResult> PutAsync(ZLinkBlobReference reference, ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutAsync(reference, payload, retention, cancellationToken); }
    public ValueTask<ZLinkBlobReadResult> ReadAsync(ZLinkBlobReference reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.ReadAsync(reference, cancellationToken); }
    public ValueTask<ZLinkBlobRenewResult> RenewAsync(ZLinkBlobReference reference, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.RenewAsync(reference, retention, cancellationToken); }
    public ValueTask DeleteAsync(ZLinkBlobReference reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.DeleteAsync(reference, cancellationToken); }
    public ValueTask<ZLinkRelocationStored> PutRelocationAsync(ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutRelocationAsync(payload, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(string reference, ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutRelocationAtAsync(reference, payload, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(string reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.GetRelocationAsync(reference, cancellationToken); }
    public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(string reference, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.RenewRelocationAsync(reference, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(string reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.DeleteRelocationAsync(reference, cancellationToken); }
}

internal sealed class RelocationBehaviorHost : IAsyncDisposable
{
    internal const string MeshName = "relocation-behavior";
    internal const string ActorType = "relocation-behavior-actor";
    internal const string SpotType = "relocation-behavior-spot";

    private readonly ServiceProvider _provider;
    private readonly IHostedService _hosted;

    private RelocationBehaviorHost(ServiceProvider provider, IHostedService hosted)
    {
        _provider = provider;
        _hosted = hosted;
        Runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
    }

    internal IServiceProvider Services => _provider;
    internal ZLinkFrameworkRuntime Runtime { get; }
    internal RoutingId LocalNodeRid => Runtime.GetMeshNodeRuntime(MeshName).Node.RoutingId;

    internal static async Task<RelocationBehaviorHost> StartAsync(
        string node,
        RelocationBehaviorTrace trace,
        IZLinkLocationStore locationStore,
        IZLinkRelocationStore relocationStore,
        bool registerTargetSpot,
        TimeSpan? pollingInterval = null)
    {
        var services = new ServiceCollection();
        services.AddSingleton(trace);
        services.AddSingleton(new BehaviorNode(node));
        services.AddTransient<BeginBehaviorJoinHandler>();
        services.AddTransient<EntryBehaviorWorkHandler>();
        services.AddTransient<TargetBehaviorWorkHandler>();
        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddLocationStore(locationStore);
            options.AddRelocationStore(relocationStore);
            options.ConfigureLocations().PollingInterval =
                pollingInterval ?? TimeSpan.FromMilliseconds(10);
            var objects = options.AddRouteMesh(MeshName)
                .Listen(ReserveTcpEndpoint())
                .SetRoutingIdPrefix($"behavior-{node}")
                .SetActorLimit(100)
                .SetSpotLimit(100)
                .Objects()
                .Server()
                .AddEntrySpot<BehaviorEntrySpot>()
                .AddActorFactory<BehaviorActor, BehaviorActorFactory>(
                    ActorType,
                    factory => factory.PreserveStateWith<BehaviorActorRelocationAdapter>());
            if (registerTargetSpot)
                objects.AddSpotFactory<BehaviorTargetSpot>(
                    SpotType,
                    factory => factory.DisableRelocation());
        });
        var provider = services.BuildServiceProvider();
        var hosted = provider.GetServices<IHostedService>().Single(
            static service => service is ZLinkFrameworkHostedService);
        try
        {
            await hosted.StartAsync(CancellationToken.None);
            return new RelocationBehaviorHost(provider, hosted);
        }
        catch
        {
            await provider.DisposeAsync();
            throw;
        }
    }

    public async ValueTask DisposeAsync()
    {
        await _hosted.StopAsync(CancellationToken.None);
        await _provider.DisposeAsync();
    }

    private static string ReserveTcpEndpoint()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return $"tcp://127.0.0.1:{port}";
    }
}

internal sealed record BehaviorNode(string Name);
internal sealed record BehaviorCreate(int State);
internal sealed record BeginBehaviorJoin(string TargetSpotId);
internal sealed record BehaviorWork(string Marker);
internal sealed record BehaviorAck(string Marker);
internal sealed record BehaviorLifecyclePush(string Marker);

internal sealed class BehaviorActor(
    string actorId,
    IZLinkActorContext context,
    RelocationBehaviorTrace trace) : IZLinkActor
{
    internal int State { get; set; }
    public string ActorId { get; } = actorId;
    public IZLinkActorContext Context { get; } = context;

    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion);
        trace.Record("publicJoinCompleted");
        return ValueTask.CompletedTask;
    }
}

internal sealed class BehaviorActorFactory(RelocationBehaviorTrace trace)
    : IZLinkActorFactory<BehaviorActor>
{
    public ValueTask<BehaviorActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(new BehaviorActor(context.ActorId, context, trace));
}

internal sealed class BehaviorActorRelocationAdapter(
    RelocationBehaviorTrace trace,
    BehaviorNode node) : IZLinkActorRelocationAdapter<BehaviorActor>
{
    public ValueTask<byte[]> CaptureAsync(
        BehaviorActor actor,
        CancellationToken cancellationToken)
    {
        Assert.Equal("source", node.Name);
        trace.Record("sourceStateCaptured");
        return ValueTask.FromResult(BitConverter.GetBytes(actor.State));
    }

    public ValueTask RestoreAsync(
        BehaviorActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        Assert.Equal("target", node.Name);
        actor.State = BitConverter.ToInt32(payload.Span);
        trace.Record("targetStateRestored");
        return ValueTask.CompletedTask;
    }
}

internal sealed class BehaviorEntrySpot(
    IZLinkEntrySpotContext context,
    RelocationBehaviorTrace trace,
    BehaviorNode node) : IZLinkEntrySpot<BehaviorActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<BeginBehaviorJoinHandler, BehaviorActor>(
            nameof(BeginBehaviorJoin));
        Context.Handlers.AddActorPacket<EntryBehaviorWorkHandler, BehaviorActor>(
            nameof(BehaviorWork));
    }

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        BehaviorActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        actor.State = request.Decode<BehaviorCreate>().State;
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request, CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public ValueTask OnJoinedActorAsync(BehaviorActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnLeaveActorAsync(BehaviorActor actor, CancellationToken cancellationToken)
    {
        Assert.Equal("source", node.Name);
        trace.Record("sourceMembershipLeaveSubmitted");
        await trace.ReleaseSourceLeave.Task.WaitAsync(cancellationToken);
    }
}

internal sealed class BehaviorTargetSpot(
    IZLinkSpotContext context,
    RelocationBehaviorTrace trace) : IZLinkSpot<BehaviorActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<TargetBehaviorWorkHandler, BehaviorActor>(
            nameof(BehaviorWork));

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(ZLinkMessage request, CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request, CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public async ValueTask OnJoinedActorAsync(BehaviorActor actor, CancellationToken cancellationToken)
    {
        _ = Interlocked.Increment(ref trace.TargetLifecycleAttemptCount);
        trace.Record("targetMembershipJoinCallback");
        trace.Record("targetLifecycleStarted");
        await trace.ReleaseTargetLifecycle.Task.WaitAsync(cancellationToken);
        trace.Record("targetLifecycleCompleted");
        if (trace.SendTargetLifecyclePush)
            await actor.Context.BoundSession
                .Send(new BehaviorLifecyclePush("target-ready"))
                .Async(cancellationToken);
    }

    public ValueTask OnLeaveActorAsync(BehaviorActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class BeginBehaviorJoinHandler(RelocationBehaviorTrace trace)
    : IZLinkEntrySpotActorRequestHandler<BehaviorEntrySpot, BehaviorActor, BeginBehaviorJoin, BehaviorAck>
{
    public async ValueTask<BehaviorAck> HandleAsync(
        BehaviorEntrySpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        BeginBehaviorJoin request,
        CancellationToken cancellationToken)
    {
        trace.Record("relocationRequested");
        actor.Context.JoinSpot(request.TargetSpotId, request)
            .Timeout(TimeSpan.FromSeconds(15))
            .Defer();
        await trace.ReleaseJoinHandler.Task.WaitAsync(cancellationToken);
        return new BehaviorAck("join");
    }
}

internal sealed class EntryBehaviorWorkHandler(RelocationBehaviorTrace trace, BehaviorNode node)
    : IZLinkEntrySpotActorRequestHandler<BehaviorEntrySpot, BehaviorActor, BehaviorWork, BehaviorAck>
{
    public ValueTask<BehaviorAck> HandleAsync(BehaviorEntrySpot spot, BehaviorActor actor, IZLinkMessageContext context, BehaviorWork request, CancellationToken cancellationToken)
    {
        trace.RecordDelivery(request.Marker, node.Name);
        return ValueTask.FromResult(new BehaviorAck(request.Marker));
    }
}

internal sealed class TargetBehaviorWorkHandler(RelocationBehaviorTrace trace, BehaviorNode node)
    : IZLinkSpotActorRequestHandler<BehaviorTargetSpot, BehaviorActor, BehaviorWork, BehaviorAck>
{
    public ValueTask<BehaviorAck> HandleAsync(BehaviorTargetSpot spot, BehaviorActor actor, IZLinkMessageContext context, BehaviorWork request, CancellationToken cancellationToken)
    {
        trace.RecordDelivery(request.Marker, node.Name);
        return ValueTask.FromResult(new BehaviorAck(request.Marker));
    }
}

internal sealed class RelocationBehaviorSessionHandlers
    : IZLinkSessionHandlerRegistry
{
    public void AddHandler<THandler>() where THandler : class
    {
    }

    public void AddHandler<THandler>(string packetName) where THandler : class
    {
    }

    public ValueTask<bool> TryHandleAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(false);
}

internal sealed class RelocationBehaviorStream(RoutingId routingId) : IZLinkStream
{
    private int _writeCount;

    internal int WriteCount => Volatile.Read(ref _writeCount);

    internal TaskCompletionSource FirstWrite { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public string SessionId { get; } = routingId.ToHex();

    public RoutingId? RoutingId { get; } = routingId;

    public string? LocalAddr => null;

    public string? RemoteAddr => null;

    public bool Write(
        ZLinkMessage payload,
        SendFlags flags = SendFlags.None)
    {
        Interlocked.Increment(ref _writeCount);
        FirstWrite.TrySetResult();
        return true;
    }

    public ValueTask CloseAsync() => ValueTask.CompletedTask;
}

internal static class RelocationBehaviorListExtensions
{
    internal static int IndexOf(this IReadOnlyList<string> values, string value)
    {
        for (var index = 0; index < values.Count; index++)
            if (string.Equals(values[index], value, StringComparison.Ordinal))
                return index;
        return -1;
    }
}
