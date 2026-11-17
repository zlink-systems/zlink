using System.Collections.Concurrent;
using System.Reflection;
using System.Diagnostics;
using System.Globalization;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.LocationProvider;

// Only the test host observes opaque records. All key and record decoding stays
// in the production repository; this decorator changes delivery, not authority.
internal sealed class OutageLocationStore(
    IZLinkLocationStore inner,
    TestHostOptions options,
    bool source
) : IZLinkLocationStore
{
    private static readonly Type Repository = typeof(IZLinkActorManager).Assembly.GetType(
        "Zlink.Framework.Runtime.Locations.ZLinkProviderLocationRepository", true
    )!;
    private const BindingFlags Static = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    private const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    private static readonly Func<byte[], object?> ReadRelocation = BindSpanDecoder(
        "Zlink.Framework.Runtime.Locations.ZLinkCanonicalRelocationAuthorityStateCodec", "TryRead"
    );
    private delegate bool SpanPredicate(ReadOnlySpan<byte> payload);
    private static readonly SpanPredicate IsSteadyReady = Repository.Assembly.GetType(
        "Zlink.Framework.Runtime.Actors.ZLinkActorAuthorityPayloadCodec", true
    )!.GetMethod("IsSteadyReady", Static)!.CreateDelegate<SpanPredicate>();
    private readonly TaskCompletionSource _failed = Signal();
    private readonly TaskCompletionSource _claimed = Signal();
    private readonly TaskCompletionSource _descriptorHeld = Signal();
    private readonly TaskCompletionSource _casHeld = Signal();
    private readonly TaskCompletionSource _descriptorRelease = Signal();
    private readonly TaskCompletionSource _casRelease = Signal();
    private readonly TaskCompletionSource _commitRelease = Signal();
    private readonly TaskCompletionSource _commitHeld = Signal();
    internal readonly TaskCompletionSource SourceCreated = Signal();
    internal readonly TaskCompletionSource Relocate = Signal();
    internal readonly TaskCompletionSource JoinCompleted = Signal();
    private static readonly Lazy<Dictionary<string, string>> Profile = new(() =>
        File.ReadLines(Environment.GetEnvironmentVariable("ZLINK_OUTAGE_LEASE_PROFILE")
            ?? throw new InvalidOperationException("ZLINK_OUTAGE_LEASE_PROFILE is required for outage tests."))
            .Where(line => !string.IsNullOrWhiteSpace(line) && !line.StartsWith('#'))
            .Select(line => line.Split('=', 2))
            .ToDictionary(fields => fields[0].Trim(), fields => fields[1].Trim()));
    private static double ProfileMs(string key) => double.Parse(Profile.Value[key], CultureInfo.InvariantCulture);
    private static double MonotonicMs() => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;
    private readonly ConcurrentDictionary<ZLinkStoreKey, double> _successfulWriteStarts = new();
    private readonly object _writeObservation = new();
    private readonly HashSet<Task> _pendingWrites = [];
    private readonly TaskCompletionSource _recoveryFailure = Signal();
    private double _cutoffMs;
    private double _descriptorConfirmedMs;
    private double _casConfirmedMs;
    private void Timing(string step, double timestamp) =>
        _events?.Append($"outage-timing|event={step}|monotonicMs={timestamp.ToString("R", CultureInfo.InvariantCulture)}");
    private Exception FailRecovery(string message)
    {
        var error = new InvalidOperationException(message);
        _recoveryFailure.TrySetException(error);
        return error;
    }
    private void RequireBeforeCutoff(double timestamp, string step)
    {
        if (timestamp >= _cutoffMs)
            throw FailRecovery($"deadline-exceeded|event={step}|monotonicMs={timestamp:R}|cutoffMs={_cutoffMs:R}");
    }
    private async Task WaitForRecoveryAsync(Task task, CancellationToken cancellationToken)
    {
        var remainingMs = _cutoffMs - MonotonicMs();
        if (remainingMs <= 0) throw FailRecovery($"deadline-exceeded|event=confirmation-wait|cutoffMs={_cutoffMs:R}");
        try { await task.WaitAsync(TimeSpan.FromMilliseconds(remainingMs), cancellationToken); }
        catch (TimeoutException) { throw FailRecovery($"deadline-exceeded|event=confirmation-wait|cutoffMs={_cutoffMs:R}"); }
    }
    private volatile bool _outage;
    private volatile bool _recovering;
    private ZLinkStoreKey _actorKey;
    private ZLinkStoreKey _meshKey;
    private ZLinkStoreKey _ownerKey;
    private object? _authorityBefore;
    private object? _authorityRecovered;
    private long _oldLease;
    private long _newLease;
    private OutageProbeRes? _identity;
    private string? _sourceRid;
    private bool _relocationFenceObserved;

    private static TaskCompletionSource Signal() => new(TaskCreationOptions.RunContinuationsAsynchronously);
    private static object Call(string name, params object?[] arguments) =>
        Repository.GetMethod(name, Static)!.Invoke(null, arguments)!;
    private static object Property(object value, string name) => value.GetType().GetProperty(name, Instance)!.GetValue(value)!;
    private static T Property<T>(object value, string name) => (T)Property(value, name);
    private static Func<byte[], object?> BindSpanDecoder(string typeName, string methodName)
    {
        var method = Repository.Assembly.GetType(typeName, true)!.GetMethod(methodName, Static)!;
        var result = method.GetParameters()[1].ParameterType.GetElementType()!;
        return (Func<byte[], object?>)typeof(OutageLocationStore)
            .GetMethod(nameof(BindTypedDecoder), Static)!.MakeGenericMethod(result).Invoke(null, [method])!;
    }

    private delegate bool SpanDecoder<T>(ReadOnlySpan<byte> payload, out T decoded);
    private static Func<byte[], object?> BindTypedDecoder<T>(MethodInfo method) where T : class
    {
        // A delegate avoids boxing ReadOnlySpan while retaining the existing codec.
        var decode = method.CreateDelegate<SpanDecoder<T>>();
        return bytes => decode(bytes, out var decoded) ? decoded : null;
    }
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    internal static IZLinkLocationStore Wrap(
        IServiceCollection services, IZLinkLocationStore store, TestHostOptions options, bool source
    )
    {
        if (options.OutageControlPrefix is null) return store;
        if (string.IsNullOrWhiteSpace(options.OutageRunId))
            throw new InvalidOperationException("--outage-run-id is required with --outage-control-prefix.");
        if (options.OutageOrder is not ("descriptor-first" or "cas-first"))
            throw new InvalidOperationException("--outage-order must be descriptor-first or cas-first.");
        var wrapper = new OutageLocationStore(store, options, source);
        services.AddSingleton(wrapper);
        services.AddHostedService(provider => new OutageCommandService(provider, wrapper, options));
        return wrapper;
    }

    internal static void ConfigureProfile(Zlink.Framework.Contracts.Locations.ZLinkLocationOptions location, TestHostOptions options)
    {
        if (options.OutageControlPrefix is null) return;
        location.OwnerLeaseTtl = TimeSpan.FromMilliseconds(ProfileMs("ttlMs"));
        location.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(ProfileMs("renewIntervalMs"));
        location.OwnerLeaseFencingMargin = TimeSpan.FromMilliseconds(ProfileMs("fencingMarginMs"));
        location.OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(ProfileMs("renewTimeoutMs"));
        location.StoreFailureGrace = TimeSpan.FromMilliseconds(ProfileMs("storeFailureGraceMs"));
        location.StoreOutageRetention = bool.Parse(Profile.Value["storeOutageRetention"]);
    }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(ZLinkStoreKey key, CancellationToken cancellationToken = default)
    {
        FailIfOutage();
        return inner.ReadAsync(key, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(ZLinkStoreScanRequest request, CancellationToken cancellationToken = default)
    {
        FailIfOutage();
        return inner.ScanAsync(request, cancellationToken);
    }

    private void FailIfOutage()
    {
        if (!_outage) return;
        _failed.TrySetResult();
        throw new IOException("Injected cross-language Location Store outage.");
    }

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request, CancellationToken cancellationToken = default
    )
    {
        var startedMs = MonotonicMs();
        FailIfOutage();
        var actor = request.Mutations.OfType<ZLinkStoreMutation.Put>().SingleOrDefault(put => put.Key == _actorKey);
        if (_recovering && source)
        {
            if (request.Mutations.OfType<ZLinkStoreMutation.Delete>().Any(delete => delete.Key == _actorKey))
                throw FailRecovery("discarded|actor-authority-delete");
            if (actor is not null && _casConfirmedMs == 0)
            {
                Timing("cas-submit", startedMs);
                if (startedMs + ProfileMs("renewTimeoutMs") > _cutoffMs)
                    throw FailRecovery($"deadline-exceeded|event=cas-submit|monotonicMs={startedMs:R}|cutoffMs={_cutoffMs:R}");
            }
        }
        if (!source && actor is not null && !_commitRelease.Task.IsCompleted)
        {
            var before = await inner.ReadAsync(_actorKey, cancellationToken);
            Require(before is ZLinkStoreReadResult.Found, "Target NewOwner CAS lost source authority.");
            var previous = Call("DecodeAuthorityMeta", ((ZLinkStoreReadResult.Found)before).Value.Bytes);
            var proposed = Call("DecodeAuthorityMeta", actor.Bytes);
            Require(Property<ulong>(previous, "ObjectGeneration") == Property<ulong>(proposed, "ObjectGeneration"), "Relocation changed object generation.");
            Require(Property<ulong>(previous, "AuthorityOwnerGeneration") != Property<ulong>(proposed, "AuthorityOwnerGeneration"), "Target write is not NewOwner.");
            _sourceRid = ((await RequireSourceAsync(cancellationToken)).NodeRid).ToString();
            _authorityBefore = previous;
            _commitHeld.TrySetResult();
            await _commitRelease.Task.WaitAsync(cancellationToken);
        }
        var result = await WriteObservedAsync(request, startedMs, cancellationToken);
        if (result is not ZLinkStoreWriteResult.Applied) return result;
        if (_recovering && source)
        {
            var owner = request.Mutations.OfType<ZLinkStoreMutation.Put>().SingleOrDefault(put => put.Key == _ownerKey);
            if (owner is not null)
            {
                var lease = Property<long>(Call("DecodeOwner", owner.Bytes), "LeaseGeneration");
                if (lease != _oldLease && _newLease == 0)
                {
                    _newLease = lease;
                    Timing("new-claim", MonotonicMs());
                    _claimed.TrySetResult();
                }
            }
            var descriptor = request.Mutations.OfType<ZLinkStoreMutation.Put>().SingleOrDefault(put => put.Key == _meshKey);
            if (descriptor is not null && _descriptorConfirmedMs == 0)
            {
                var descriptorType = Repository.Assembly.GetType("Zlink.Framework.Runtime.Locations.ZLinkMeshNodeDescriptor", true)!;
                var record = Repository.GetMethod("DecodeDescriptor", Static)!.MakeGenericMethod(descriptorType).Invoke(null, [descriptor.Bytes])!;
                Require(Property<long>(Property(record, "Descriptor"), "LeaseGeneration") == _newLease, "Recovery descriptor did not use the recovered lease.");
                Require(Property<string>(Property(record, "Descriptor"), "OwnerId") == Property<string>(_authorityBefore!, "OwnerId"), "Recovery descriptor changed owner identity.");
                _descriptorHeld.TrySetResult();
                await WaitForRecoveryAsync(_descriptorRelease.Task, cancellationToken);
                _descriptorConfirmedMs = MonotonicMs();
                Timing("descriptor-confirmed", _descriptorConfirmedMs);
                RequireBeforeCutoff(_descriptorConfirmedMs, "descriptor-confirmed");
            }
            if (actor is not null && _casConfirmedMs == 0)
            {
                var meta = Call("DecodeAuthorityMeta", actor.Bytes);
                Require(_authorityBefore is not null, "Missing steady authority observation.");
                Require(Property<ulong>(_authorityBefore!, "ObjectGeneration") == Property<ulong>(meta, "ObjectGeneration"), "Recovery changed object generation.");
                Require(Property<ulong>(_authorityBefore!, "AuthorityOwnerGeneration") == Property<ulong>(meta, "AuthorityOwnerGeneration"), "Recovery did not Preserve authority generation.");
                Require(Property<string>(_authorityBefore!, "OwnerId") == Property<string>(meta, "OwnerId"), "Recovery changed owner identity.");
                Require(Property<long>(meta, "OwnerLeaseGeneration") == _newLease && _newLease != _oldLease, "Preserve did not use the recovered lease.");
                _authorityRecovered = meta;
                _casHeld.TrySetResult();
                await WaitForRecoveryAsync(_casRelease.Task, cancellationToken);
                _casConfirmedMs = MonotonicMs();
                Timing("cas-confirmed", _casConfirmedMs);
                RequireBeforeCutoff(_casConfirmedMs, "cas-confirmed");
            }
        }
        if (source && !_recovering && _newLease != 0 && actor is not null)
        {
            var meta = Call("DecodeAuthorityMeta", actor.Bytes);
            var relocation = ReadRelocation(Property<byte[]>(meta, "Payload"));
            if (relocation is not null)
            {
                var state = Property(relocation, "State");
                Require(Property<string>(state, "SourceOwnerId") == Property<string>(_authorityBefore!, "OwnerId"), "Relocation source changed owner identity.");
                Require(Property<string>(state, "CoordinatorOwnerId") == Property<string>(_authorityBefore!, "OwnerId"), "Relocation coordinator changed owner identity.");
                Require(Property<ulong>(state, "SourceOwnerLeaseGeneration") == (ulong)_newLease, "Relocation source reused the old lease.");
                Require(Property<ulong>(state, "CoordinatorLeaseGeneration") == (ulong)_newLease, "Relocation coordinator reused the old lease.");
                _relocationFenceObserved = true;
            }
        }
        return result;
    }

    private async Task<ZLinkStoreWriteResult> WriteObservedAsync(
        ZLinkStoreWriteRequest request, double startedMs, CancellationToken cancellationToken)
    {
        var settled = Signal();
        Task<ZLinkStoreWriteResult> write;
        lock (_writeObservation)
        {
            FailIfOutage();
            write = inner.WriteAsync(request, cancellationToken).AsTask();
            _pendingWrites.Add(settled.Task);
        }
        try
        {
            var result = await write;
            if (source && !_recovering && result is ZLinkStoreWriteResult.Applied)
                foreach (var put in request.Mutations.OfType<ZLinkStoreMutation.Put>())
                    _successfulWriteStarts.AddOrUpdate(put.Key, startedMs, (_, previous) => Math.Max(previous, startedMs));
            return result;
        }
        finally
        {
            lock (_writeObservation) _pendingWrites.Remove(settled.Task);
            settled.TrySetResult();
        }
    }

    private IZLinkActorManager? _actors;
    private TestHostEventSink? _events;
    private async Task<ActorRef> RequireSourceAsync(CancellationToken cancellationToken) =>
        await _actors!.FindAsync(options.ActorId ?? "cross-lang-user-spot-actor", cancellationToken)
            ?? throw new InvalidOperationException("Public Find lost the source owner before commit.");

    internal async Task RunCommandAsync(string command, IServiceProvider services, CancellationToken cancellationToken)
    {
        var commandTask = RunCommandCoreAsync(command, services, cancellationToken);
        await await Task.WhenAny(commandTask, _recoveryFailure.Task);
    }

    private async Task RunCommandCoreAsync(string command, IServiceProvider services, CancellationToken cancellationToken)
    {
        var actorId = options.ActorId ?? "cross-lang-user-spot-actor";
        _actors = services.GetRequiredService<IZLinkActorManager>();
        _events ??= services.GetRequiredService<TestHostEventSink>();
        var client = services.GetRequiredService<IZLinkActorClient>();
        var runtime = services.GetRequiredService<IZLinkSpotManager>();
        switch (command)
        {
            case "steady":
                var codec = Repository.Assembly.GetType("Zlink.Framework.Runtime.Locations.ZLinkAuthorityKeyCodec", true)!;
                var actorKey = codec.GetMethod("EncodeActor", Static)!.Invoke(null, [actorId]);
                _actorKey = (ZLinkStoreKey)Call("AuthorityMetaKey", actorKey);
                if (!source) return;
                await SourceCreated.Task.WaitAsync(cancellationToken);
                _identity = await ProbeAsync(client, "steady", cancellationToken);
                _sourceRid = _identity.NodeRid;
                var row = await inner.ReadAsync(_actorKey, cancellationToken);
                Require(row is ZLinkStoreReadResult.Found, "Steady actor authority is missing.");
                _authorityBefore = Call("DecodeAuthorityMeta", ((ZLinkStoreReadResult.Found)row).Value.Bytes);
                Require(IsSteadyReady(Property<byte[]>(_authorityBefore, "Payload")), "Initial actor authority is not steady Ready.");
                _oldLease = Property<long>(_authorityBefore, "OwnerLeaseGeneration");
                _ownerKey = (ZLinkStoreKey)Call("OwnerKey", Property<string>(_authorityBefore, "OwnerId"));
                _meshKey = (ZLinkStoreKey)Call("MeshKey", options.MeshName!, RoutingId.From(_sourceRid));
                services.GetRequiredService<TestHostEventSink>().Append($"outage-identity|instance={_identity.InstanceId}|creations={_identity.CreationCount}|lease={_oldLease}|object={Property<ulong>(_authorityBefore, "ObjectGeneration")}|authority={Property<ulong>(_authorityBefore, "AuthorityOwnerGeneration")}");
                return;
            case "outage":
                Require(source, "Only the source may inject outage.");
                Task[] pending;
                lock (_writeObservation)
                {
                    _outage = true;
                    pending = _pendingWrites.ToArray();
                }
                await _failed.Task.WaitAsync(cancellationToken);
                await Task.WhenAll(pending).WaitAsync(cancellationToken);
                Require(_successfulWriteStarts.TryGetValue(_ownerKey, out var t0Ms), "No successful owner lease write was observed.");
                _cutoffMs = t0Ms + ProfileMs("ttlMs") + 2 * ProfileMs("fencingMarginMs");
                Timing("t0", t0Ms);
                Timing("cutoff", _cutoffMs);
                return;
            case "expired":
                while (await inner.ReadAsync(_ownerKey, cancellationToken) is not ZLinkStoreReadResult.Missing)
                    await Task.Delay(10, cancellationToken);
                return;
            case "recover":
                _recovering = true;
                _outage = false;
                await WaitForRecoveryAsync(_claimed.Task, cancellationToken);
                services.GetRequiredService<TestHostEventSink>().Append($"outage-lease|old={_oldLease}|new={_newLease}");
                return;
            case "release-descriptor":
                await WaitForRecoveryAsync(_descriptorHeld.Task, cancellationToken);
                _descriptorRelease.TrySetResult();
                return;
            case "release-cas":
                await WaitForRecoveryAsync(_casHeld.Task, cancellationToken);
                _casRelease.TrySetResult();
                return;
            case "partial":
                await WaitForRecoveryAsync(Task.WhenAll(_descriptorHeld.Task, _casHeld.Task), cancellationToken);
                var before = OutageActorObservations.Count(actorId);
                try
                {
                    await ProbeAsync(client, "partial", cancellationToken);
                    throw new InvalidOperationException("A partially confirmed recovery executed an operation.");
                }
                catch (ZLinkFrameworkException error) when (error.Kind == ZLinkFrameworkErrorKind.ShuttingDown) { }
                Require(OutageActorObservations.Count(actorId) == before, "Partial recovery entered the handler.");
                services.GetRequiredService<TestHostEventSink>().Append("outage-partial|error=ShuttingDown|handlerDelta=0");
                return;
            case "resumed":
                Require(_descriptorRelease.Task.IsCompleted && _casRelease.Task.IsCompleted, "Both recovery results must be released.");
                var locationRuntime = runtime.GetType().GetField("_locationRuntime", Instance)!.GetValue(runtime)!;
                while (Property(locationRuntime.GetType().GetMethod("GetHealthSnapshot", Instance)!.Invoke(locationRuntime, null)!, "StoreOutage") is not null)
                    await Task.Delay(10, cancellationToken);
                var current = await ProbeAsync(client, "resumed", cancellationToken);
                Require(current.InstanceId == _identity!.InstanceId && current.CreationCount == _identity.CreationCount, "Recovery replaced the source instance.");
                Require(_authorityRecovered is not null, "Recovery Preserve result was not observed.");
                Require(_descriptorConfirmedMs > 0 && _casConfirmedMs > 0, "Both recovery confirmations must be observed.");
                RequireBeforeCutoff(_descriptorConfirmedMs, "descriptor-confirmed");
                RequireBeforeCutoff(_casConfirmedMs, "cas-confirmed");
                _recovering = false;
                services.GetRequiredService<TestHostEventSink>().Append($"outage-resumed|instance={current.InstanceId}|creations={current.CreationCount}");
                return;
            case "relocation":
                Relocate.TrySetResult();
                return;
            case "staged":
                Require(!source, "Staged observation belongs to the target.");
                await _commitHeld.Task.WaitAsync(cancellationToken);
                var owner = await RequireSourceAsync(cancellationToken);
                Require(owner.NodeRid.ToString() == _sourceRid, "Public Find published the target before NewOwner commit.");
                Require(OutageActorObservations.Count(actorId) == 0, "Staged target executed an actor handler.");
                RequireStagedFenceAbsent(runtime, actorId, Property<ulong>(_authorityBefore!, "ObjectGeneration"));
                services.GetRequiredService<TestHostEventSink>().Append($"outage-staged|find={_sourceRid}|handlerCount=0|fence=absent");
                return;
            case "commit":
                Require(_commitHeld.Task.IsCompleted, "No staged NewOwner CAS was observed.");
                _commitRelease.TrySetResult();
                return;
            case "completed":
                if (source)
                {
                    await JoinCompleted.Task.WaitAsync(cancellationToken);
                    Require(_relocationFenceObserved, "Relocation source/coordinator lease was not observed.");
                    services.GetRequiredService<TestHostEventSink>().Append($"outage-fence|sourceLease={_newLease}|coordinatorLease={_newLease}");
                }
                else
                {
                    var found = await RequireSourceAsync(cancellationToken);
                    Require(found.NodeRid.ToString() != _sourceRid, "Public Find did not publish the target.");
                    for (var index = 0; index < 2; index++)
                    {
                        var response = await client.RequestToActor(actorId, new UserSpotProbeReq($"outage-completed-{index}"))
                            .Timeout(TimeSpan.FromSeconds(5)).Async<UserSpotProbeRes>(cancellationToken);
                        Require(response.NodeRid == found.NodeRid.ToString(), "Post-commit operation did not run on target.");
                    }
                }
                return;
            default:
                throw new InvalidOperationException($"Unknown outage command '{command}'.");
        }
    }

    private Task<OutageProbeRes> ProbeAsync(IZLinkActorClient client, string marker, CancellationToken cancellationToken) =>
        client.RequestToActor(options.ActorId ?? "cross-lang-user-spot-actor", new OutageProbeReq(marker))
            .Timeout(TimeSpan.FromSeconds(1)).Async<OutageProbeRes>(cancellationToken).AsTask();

    private void RequireStagedFenceAbsent(object runtime, string actorId, ulong generation)
    {
        var nodeRuntime = runtime.GetType().GetMethod("GetMeshNodeRuntime", Instance)!.Invoke(runtime, [options.MeshName]);
        var node = Property(nodeRuntime!, "Node");
        var referenceType = Repository.Assembly.GetType("Zlink.Framework.Runtime.Backend.Contracts.ZLinkBackendActorRef", true)!;
        var rid = Property<RoutingId>(node, "RoutingId");
        var reference = Activator.CreateInstance(referenceType, Instance, null, [rid, actorId, generation], null);
        var parameters = new object?[] { reference, 0UL, 0UL };
        var visible = (bool)node.GetType().GetMethod("TryGetLocalActorAuthority", Instance)!.Invoke(node, parameters)!;
        Require(!visible, "Staged target exposed a local owner fence.");
    }
}

internal sealed class OutageCommandService(
    IServiceProvider services, OutageLocationStore store, TestHostOptions options
) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        long previous = 0;
        while (!stoppingToken.IsCancellationRequested)
        {
            var file = options.OutageControlPrefix + ".command";
            if (!File.Exists(file))
            {
                await Task.Delay(10, stoppingToken);
                continue;
            }
            var fields = (await File.ReadAllTextAsync(file, stoppingToken)).TrimEnd('\r', '\n').Split('\t');
            if (fields.Length != 3 || fields[0] != options.OutageRunId || !long.TryParse(fields[1], out var sequence) || sequence <= previous)
            {
                await Task.Delay(10, stoppingToken);
                continue;
            }
            if (sequence != previous + 1) throw new InvalidOperationException("Outage command sequence skipped a phase.");
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
            deadline.CancelAfter(TimeSpan.FromSeconds(60));
            try
            {
                await store.RunCommandAsync(fields[2], services, deadline.Token);
            }
            catch (Exception error)
            {
                var message = error.Message.StartsWith("deadline-exceeded|", StringComparison.Ordinal)
                    || error.Message.StartsWith("discarded|", StringComparison.Ordinal)
                    ? error.Message : $"{error.GetType().Name}:{error.Message}";
                services.GetRequiredService<TestHostEventSink>().Append($"outage-error|run={fields[0]}|seq={sequence}|phase={fields[2]}|error={message}");
                throw;
            }
            services.GetRequiredService<TestHostEventSink>().Append($"outage-ack|run={fields[0]}|seq={sequence}|phase={fields[2]}");
            previous = sequence;
        }
    }
}

internal static class OutageActorObservations
{
    private static readonly ConcurrentDictionary<string, int> Creations = new();
    private static readonly ConcurrentDictionary<string, int> Handlers = new();
    internal static int Created(string actorId) => Creations.AddOrUpdate(actorId, 1, (_, count) => count + 1);
    internal static int Count(string actorId) => Handlers.GetValueOrDefault(actorId);
    internal static int Enter(string actorId) => Handlers.AddOrUpdate(actorId, 1, (_, count) => count + 1);
}

internal sealed record OutageProbeReq(string Marker);
internal sealed record OutageProbeRes(string ActorId, string NodeRid, string InstanceId, int CreationCount, int HandlerCount, string Marker);

internal sealed class OutageProbeHandler
    : IZLinkEntrySpotActorRequestHandler<RelocationEntrySpot, RelocationActor, OutageProbeReq, OutageProbeRes>
{
    public ValueTask<OutageProbeRes> HandleAsync(RelocationEntrySpot spot, RelocationActor actor, IZLinkMessageContext context, OutageProbeReq request, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new OutageProbeRes(actor.ActorId, spot.Context.NodeRid.ToString(), actor.InstanceId, actor.CreationCount, OutageActorObservations.Enter(actor.ActorId), request.Marker));
    }
}
