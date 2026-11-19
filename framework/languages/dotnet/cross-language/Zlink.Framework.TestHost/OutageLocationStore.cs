using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;

// The test store reads only canonical fields needed by outage assertions.
internal sealed class OutageLocationStore(
    IZLinkLocationStore inner,
    TestHostOptions options,
    bool source
) : IZLinkLocationStore
{
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
        File.ReadLines(
                Environment.GetEnvironmentVariable("ZLINK_OUTAGE_LEASE_PROFILE")
                    ?? throw new InvalidOperationException(
                        "ZLINK_OUTAGE_LEASE_PROFILE is required for outage tests."
                    )
            )
            .Where(line => !string.IsNullOrWhiteSpace(line) && !line.StartsWith('#'))
            .Select(line => line.Split('=', 2))
            .ToDictionary(fields => fields[0].Trim(), fields => fields[1].Trim())
    );

    private static double ProfileMs(string key) =>
        double.Parse(Profile.Value[key], CultureInfo.InvariantCulture);

    private static double MonotonicMs() => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;

    private readonly ConcurrentDictionary<ZLinkStoreKey, double> _successfulWriteStarts = new();
    private readonly object _writeObservation = new();
    private readonly HashSet<Task> _pendingWrites = [];
    private readonly TaskCompletionSource _recoveryFailure = Signal();
    private double _cutoffMs;
    private double _descriptorConfirmedMs;
    private double _casConfirmedMs;

    private void Timing(string step, double timestamp) =>
        _events?.Append(
            $"outage-timing|event={step}|monotonicMs={timestamp.ToString("R", CultureInfo.InvariantCulture)}"
        );

    private Exception FailRecovery(string message)
    {
        var error = new InvalidOperationException(message);
        _recoveryFailure.TrySetException(error);
        return error;
    }

    private void RequireBeforeCutoff(double timestamp, string step)
    {
        if (timestamp >= _cutoffMs)
            throw FailRecovery(
                $"deadline-exceeded|event={step}|monotonicMs={timestamp:R}|cutoffMs={_cutoffMs:R}"
            );
    }

    private async Task WaitForRecoveryAsync(Task task, CancellationToken cancellationToken)
    {
        var remainingMs = _cutoffMs - MonotonicMs();
        if (remainingMs <= 0)
            throw FailRecovery($"deadline-exceeded|event=confirmation-wait|cutoffMs={_cutoffMs:R}");
        try
        {
            await task.WaitAsync(TimeSpan.FromMilliseconds(remainingMs), cancellationToken);
        }
        catch (TimeoutException)
        {
            throw FailRecovery($"deadline-exceeded|event=confirmation-wait|cutoffMs={_cutoffMs:R}");
        }
    }

    private volatile bool _outage;
    private volatile bool _recovering;
    private ZLinkStoreKey _actorKey;
    private ZLinkStoreKey _meshKey;
    private ZLinkStoreKey _ownerKey;
    private OutageCanonicalStoreReader.Authority? _authorityBefore;
    private OutageCanonicalStoreReader.Authority? _authorityRecovered;
    private long _oldLease;
    private long _newLease;
    private OutageProbeRes? _identity;
    private string? _sourceRid;
    private bool _relocationFenceObserved;

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private void Require(bool condition, string message)
    {
        if (!condition)
            throw FailRecovery(message);
    }

    internal static IZLinkLocationStore Wrap(
        IServiceCollection services,
        IZLinkLocationStore store,
        TestHostOptions options,
        bool source
    )
    {
        if (options.OutageControlPrefix is null)
            return store;
        if (string.IsNullOrWhiteSpace(options.OutageRunId))
            throw new InvalidOperationException(
                "--outage-run-id is required with --outage-control-prefix."
            );
        if (options.OutageOrder is not ("descriptor-first" or "cas-first"))
            throw new InvalidOperationException(
                "--outage-order must be descriptor-first or cas-first."
            );
        var wrapper = new OutageLocationStore(store, options, source);
        services.AddSingleton(wrapper);
        services.AddHostedService(provider => new OutageCommandService(provider, wrapper, options));
        return wrapper;
    }

    internal static void ConfigureProfile(
        Zlink.Framework.Contracts.Locations.ZLinkLocationOptions location,
        TestHostOptions options
    )
    {
        if (options.OutageControlPrefix is null)
            return;
        location.OwnerLeaseTtl = TimeSpan.FromMilliseconds(ProfileMs("ttlMs"));
        location.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(ProfileMs("renewIntervalMs"));
        location.OwnerLeaseFencingMargin = TimeSpan.FromMilliseconds(ProfileMs("fencingMarginMs"));
        location.OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(ProfileMs("renewTimeoutMs"));
        location.StoreFailureGrace = TimeSpan.FromMilliseconds(ProfileMs("storeFailureGraceMs"));
        location.StoreOutageRetention = bool.Parse(Profile.Value["storeOutageRetention"]);
    }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    )
    {
        FailIfOutage();
        return inner.ReadAsync(key, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    )
    {
        FailIfOutage();
        return inner.ScanAsync(request, cancellationToken);
    }

    private void FailIfOutage()
    {
        if (!_outage)
            return;
        _failed.TrySetResult();
        throw new IOException("Injected cross-language Location Store outage.");
    }

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    )
    {
        var startedMs = MonotonicMs();
        FailIfOutage();
        var actor = request
            .Mutations.OfType<ZLinkStoreMutation.Put>()
            .SingleOrDefault(put => put.Key == _actorKey);
        if (_recovering && source)
        {
            if (
                request
                    .Mutations.OfType<ZLinkStoreMutation.Delete>()
                    .Any(delete => delete.Key == _actorKey)
            )
                throw FailRecovery("discarded|actor-authority-delete");
            if (actor is not null && _casConfirmedMs == 0)
            {
                Timing("cas-submit", startedMs);
                if (startedMs + ProfileMs("renewTimeoutMs") > _cutoffMs)
                    throw FailRecovery(
                        $"deadline-exceeded|event=cas-submit|monotonicMs={startedMs:R}|cutoffMs={_cutoffMs:R}"
                    );
            }
        }
        if (!source && actor is not null && !_commitRelease.Task.IsCompleted)
        {
            var before = await inner.ReadAsync(_actorKey, cancellationToken);
            Require(
                before is ZLinkStoreReadResult.Found,
                "Target NewOwner CAS lost source authority."
            );
            var previous = OutageCanonicalStoreReader.ReadAuthority(
                ((ZLinkStoreReadResult.Found)before).Value.Bytes
            );
            var proposed = OutageCanonicalStoreReader.ReadAuthority(actor.Bytes);
            Require(
                previous.ObjectGeneration == proposed.ObjectGeneration,
                "Relocation changed object generation."
            );
            Require(
                previous.AuthorityOwnerGeneration != proposed.AuthorityOwnerGeneration,
                "Target write is not NewOwner."
            );
            _authorityBefore = previous;
            _commitHeld.TrySetResult();
            await _commitRelease.Task.WaitAsync(cancellationToken);
        }
        var result = await WriteObservedAsync(request, startedMs, cancellationToken);
        if (result is not ZLinkStoreWriteResult.Applied)
            return result;
        if (_recovering && source)
        {
            try
            {
                var owner = request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .SingleOrDefault(put => put.Key == _ownerKey);
                if (owner is not null)
                {
                    var lease = OutageCanonicalStoreReader.ReadOwnerLease(owner.Bytes);
                    if (lease != _oldLease && _newLease == 0)
                    {
                        _newLease = lease;
                        Timing("new-claim", MonotonicMs());
                        _claimed.TrySetResult();
                    }
                }
                var descriptor = request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .SingleOrDefault(put => put.Key == _meshKey);
                if (descriptor is not null && _descriptorConfirmedMs == 0)
                {
                    var descriptorOwner = OutageCanonicalStoreReader.ReadDescriptorOwner(
                        descriptor.Bytes
                    );
                    Require(
                        descriptorOwner.LeaseGeneration == _newLease,
                        "Recovery descriptor did not use the recovered lease."
                    );
                    Require(
                        descriptorOwner.OwnerId == _authorityBefore!.OwnerId,
                        "Recovery descriptor changed owner identity."
                    );
                    _descriptorHeld.TrySetResult();
                    await WaitForRecoveryAsync(_descriptorRelease.Task, cancellationToken);
                    _descriptorConfirmedMs = MonotonicMs();
                    Timing("descriptor-confirmed", _descriptorConfirmedMs);
                    RequireBeforeCutoff(_descriptorConfirmedMs, "descriptor-confirmed");
                }
                if (actor is not null && _casConfirmedMs == 0)
                {
                    var meta = OutageCanonicalStoreReader.ReadAuthority(actor.Bytes);
                    Require(_authorityBefore is not null, "Missing steady authority observation.");
                    Require(
                        _authorityBefore!.ObjectGeneration == meta.ObjectGeneration,
                        "Recovery changed object generation."
                    );
                    Require(
                        _authorityBefore!.AuthorityOwnerGeneration == meta.AuthorityOwnerGeneration,
                        "Recovery did not Preserve authority generation."
                    );
                    Require(
                        _authorityBefore!.OwnerId == meta.OwnerId,
                        "Recovery changed owner identity."
                    );
                    Require(
                        meta.OwnerLeaseGeneration == _newLease && _newLease != _oldLease,
                        "Preserve did not use the recovered lease."
                    );
                    _authorityRecovered = meta;
                    _casHeld.TrySetResult();
                    await WaitForRecoveryAsync(_casRelease.Task, cancellationToken);
                    _casConfirmedMs = MonotonicMs();
                    Timing("cas-confirmed", _casConfirmedMs);
                    RequireBeforeCutoff(_casConfirmedMs, "cas-confirmed");
                }
            }
            catch (Exception error) when (!_recoveryFailure.Task.IsCompleted)
            {
                // Observation faults must fail the run with their cause, not as a later deadline.
                throw FailRecovery(
                    $"wrapper-observation|{error.GetType().Name}:{(error.InnerException ?? error).Message}"
                );
            }
        }
        if (source && !_recovering && _newLease != 0 && actor is not null)
        {
            var meta = OutageCanonicalStoreReader.ReadAuthority(actor.Bytes);
            var relocation = OutageCanonicalStoreReader.ReadRelocation(meta.Payload);
            if (relocation is not null)
            {
                var state = relocation;
                Require(
                    state.SourceOwnerId == _authorityBefore!.OwnerId,
                    "Relocation source changed owner identity."
                );
                Require(
                    state.CoordinatorOwnerId == _authorityBefore!.OwnerId,
                    "Relocation coordinator changed owner identity."
                );
                Require(
                    state.SourceOwnerLeaseGeneration == (ulong)_newLease,
                    "Relocation source reused the old lease."
                );
                Require(
                    state.CoordinatorLeaseGeneration == (ulong)_newLease,
                    "Relocation coordinator reused the old lease."
                );
                _relocationFenceObserved = true;
            }
        }
        return result;
    }

    private async Task<ZLinkStoreWriteResult> WriteObservedAsync(
        ZLinkStoreWriteRequest request,
        double startedMs,
        CancellationToken cancellationToken
    )
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
                    _successfulWriteStarts.AddOrUpdate(
                        put.Key,
                        startedMs,
                        (_, previous) => Math.Max(previous, startedMs)
                    );
            return result;
        }
        finally
        {
            lock (_writeObservation)
                _pendingWrites.Remove(settled.Task);
            settled.TrySetResult();
        }
    }

    private IZLinkActorManager? _actors;
    private TestHostEventSink? _events;

    private async Task<ActorRef> RequireSourceAsync(CancellationToken cancellationToken) =>
        await _actors!.FindAsync(options.ActorId ?? "cross-lang-user-spot-actor", cancellationToken)
        ?? throw FailRecovery("Public Find lost the source owner before commit.");

    internal async Task RunCommandAsync(
        string command,
        IServiceProvider services,
        CancellationToken cancellationToken
    )
    {
        var commandTask = RunCommandCoreAsync(command, services, cancellationToken);
        await await Task.WhenAny(commandTask, _recoveryFailure.Task);
    }

    private async Task RunCommandCoreAsync(
        string command,
        IServiceProvider services,
        CancellationToken cancellationToken
    )
    {
        var actorId = options.ActorId ?? "cross-lang-user-spot-actor";
        _actors = services.GetRequiredService<IZLinkActorManager>();
        _events ??= services.GetRequiredService<TestHostEventSink>();
        var client = services.GetRequiredService<IZLinkActorClient>();
        var health = services.GetRequiredService<IZLinkLocationRuntimeQuery>();
        switch (command)
        {
            case "steady":
                _actorKey = OutageCanonicalStoreReader.ActorKey(actorId);
                if (!source)
                {
                    // Public Find runs in the staged command, never inside a Store callback on the runtime path.
                    _sourceRid =
                        options.PeerRid
                        ?? throw new InvalidOperationException(
                            "--peer-rid is required for the outage target."
                        );
                    return;
                }
                await SourceCreated.Task.WaitAsync(cancellationToken);
                _identity = await ProbeAsync(client, "steady", cancellationToken);
                _sourceRid = _identity.NodeRid;
                var row = await inner.ReadAsync(_actorKey, cancellationToken);
                Require(row is ZLinkStoreReadResult.Found, "Steady actor authority is missing.");
                _authorityBefore = OutageCanonicalStoreReader.ReadAuthority(
                    ((ZLinkStoreReadResult.Found)row).Value.Bytes
                );
                Require(
                    OutageCanonicalStoreReader.IsSteadyReady(_authorityBefore.Payload),
                    "Initial actor authority is not steady Ready."
                );
                _oldLease = _authorityBefore.OwnerLeaseGeneration;
                _ownerKey = OutageCanonicalStoreReader.OwnerKey(_authorityBefore.OwnerId);
                _meshKey = OutageCanonicalStoreReader.MeshKey(
                    options.MeshName!,
                    RoutingId.From(_sourceRid)
                );
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append(
                        $"outage-identity|instance={_identity.InstanceId}|creations={_identity.CreationCount}|lease={_oldLease}|object={_authorityBefore.ObjectGeneration}|authority={_authorityBefore.AuthorityOwnerGeneration}"
                    );
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
                await WaitForHealthAsync(health, healthy: false, cancellationToken);
                Require(
                    _successfulWriteStarts.TryGetValue(_ownerKey, out var writeStartMs),
                    "No successful owner lease write was observed."
                );
                // Public status omits the runtime request start; bound it by the Store write start.
                var t0Ms = writeStartMs - ProfileMs("renewTimeoutMs");
                _cutoffMs = t0Ms + ProfileMs("ttlMs") + 2 * ProfileMs("fencingMarginMs");
                Timing("t0", t0Ms);
                _events.Append("outage-observation|cutoffSource=store-request-conservative-bound");
                Timing("cutoff", _cutoffMs);
                return;
            case "expired":
                while (
                    await inner.ReadAsync(_ownerKey, cancellationToken)
                        is not ZLinkStoreReadResult.Missing
                )
                    await Task.Delay(10, cancellationToken);
                return;
            case "recover":
                _recovering = true;
                _outage = false;
                await WaitForRecoveryAsync(_claimed.Task, cancellationToken);
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append($"outage-lease|old={_oldLease}|new={_newLease}");
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
                await WaitForRecoveryAsync(
                    Task.WhenAll(_descriptorHeld.Task, _casHeld.Task),
                    cancellationToken
                );
                var before = OutageActorObservations.Count(actorId);
                try
                {
                    await ProbeAsync(client, "partial", cancellationToken);
                    throw new InvalidOperationException(
                        "A partially confirmed recovery executed an operation."
                    );
                }
                catch (ZLinkFrameworkException error)
                    when (error.Kind == ZLinkFrameworkErrorKind.ShuttingDown) { }
                Require(
                    OutageActorObservations.Count(actorId) == before,
                    "Partial recovery entered the handler."
                );
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append("outage-partial|error=ShuttingDown|handlerDelta=0");
                return;
            case "resumed":
                Require(
                    _descriptorRelease.Task.IsCompleted && _casRelease.Task.IsCompleted,
                    "Both recovery results must be released."
                );
                await WaitForHealthAsync(health, healthy: true, cancellationToken);
                var current = await ObserveResumedAsync(client, actorId, cancellationToken);
                Require(
                    current.InstanceId == _identity!.InstanceId
                        && current.CreationCount == _identity.CreationCount,
                    "Recovery replaced the source instance."
                );
                Require(
                    _authorityRecovered is not null,
                    "Recovery Preserve result was not observed."
                );
                Require(
                    _descriptorConfirmedMs > 0 && _casConfirmedMs > 0,
                    "Both recovery confirmations must be observed."
                );
                RequireBeforeCutoff(_descriptorConfirmedMs, "descriptor-confirmed");
                RequireBeforeCutoff(_casConfirmedMs, "cas-confirmed");
                _recovering = false;
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append(
                        $"outage-resumed|instance={current.InstanceId}|creations={current.CreationCount}"
                    );
                return;
            case "relocation":
                Relocate.TrySetResult();
                return;
            case "staged":
                Require(!source, "Staged observation belongs to the target.");
                await _commitHeld.Task.WaitAsync(cancellationToken);
                var owner = await RequireSourceAsync(cancellationToken);
                Require(
                    owner.NodeRid.ToString() == _sourceRid,
                    "Public Find published the target before NewOwner commit."
                );
                Require(
                    OutageActorObservations.Count(actorId) == 0,
                    "Staged target executed an actor handler."
                );
                var flow = services.GetRequiredService<TestHostMessageFlowListener>();
                Require(
                    flow.HasRelocationDetail(actorId, "target_restore_begin"),
                    "Staged target restore was not observed in message flow."
                );
                // AuthorityPublished is private. Find, handler count and restore flow are
                // the public observations of the held pre-commit stage.
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append(
                        $"outage-staged|find={_sourceRid}|handlerCount=0|fence=unobserved|AuthorityPublished=unavailable|fenceObservation=public-routing-and-handler"
                    );
                return;
            case "commit":
                Require(_commitHeld.Task.IsCompleted, "No staged NewOwner CAS was observed.");
                _commitRelease.TrySetResult();
                return;
            case "completed":
                if (source)
                {
                    await JoinCompleted.Task.WaitAsync(cancellationToken);
                    Require(
                        _relocationFenceObserved,
                        "Relocation source/coordinator lease was not observed."
                    );
                    services
                        .GetRequiredService<TestHostEventSink>()
                        .Append(
                            $"outage-fence|sourceLease={_newLease}|coordinatorLease={_newLease}"
                        );
                }
                else
                {
                    var found = await RequireSourceAsync(cancellationToken);
                    Require(
                        found.NodeRid.ToString() != _sourceRid,
                        "Public Find did not publish the target."
                    );
                    for (var index = 0; index < 2; index++)
                    {
                        var marker = $"outage-completed-{index}";
                        var response = await client
                            .RequestToActor(actorId, new UserSpotProbeReq(marker))
                            .Timeout(TimeSpan.FromSeconds(5))
                            .Async<UserSpotProbeRes>(cancellationToken);
                        Require(
                            response.NodeRid == found.NodeRid.ToString()
                                && response.Marker == marker,
                            "Post-commit operation did not run on target."
                        );
                        // Source host probes also reach this target; count this request only.
                        Require(
                            OutageActorObservations.Count(actorId, marker) == 1,
                            "Post-commit request did not execute exactly once on target."
                        );
                    }
                }
                return;
            default:
                throw new InvalidOperationException($"Unknown outage command '{command}'.");
        }
    }

    private Task<OutageProbeRes> ProbeAsync(
        IZLinkActorClient client,
        string marker,
        CancellationToken cancellationToken
    ) =>
        client
            .RequestToActor(
                options.ActorId ?? "cross-lang-user-spot-actor",
                new OutageProbeReq(marker)
            )
            .Timeout(TimeSpan.FromSeconds(1))
            .Async<OutageProbeRes>(cancellationToken)
            .AsTask();

    private static async Task WaitForHealthAsync(
        IZLinkLocationRuntimeQuery query,
        bool healthy,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            var status = await query.GetStatusAsync(cancellationToken);
            if (
                healthy
                    ? status.StoreHealthy && status.OwnerLeaseHealthy
                    : !status.OwnerLeaseHealthy
            )
                return;
            await Task.Delay(10, cancellationToken);
        }
    }

    private async Task<OutageProbeRes> ObserveResumedAsync(
        IZLinkActorClient client,
        string actorId,
        CancellationToken cancellationToken
    )
    {
        // Public health does not expose per-actor recovery admission. Observe it
        // through successful dispatch; every closed-admission result must run no handler.
        while (true)
        {
            var before = OutageActorObservations.Count(actorId);
            try
            {
                return await ProbeAsync(client, "resumed", cancellationToken);
            }
            catch (ZLinkFrameworkException error)
                when (error.Kind == ZLinkFrameworkErrorKind.ShuttingDown)
            {
                Require(
                    OutageActorObservations.Count(actorId) == before,
                    "Closed recovery admission entered the handler."
                );
            }
            await Task.Delay(10, cancellationToken);
        }
    }
}

internal sealed class OutageCommandService(
    IServiceProvider services,
    OutageLocationStore store,
    TestHostOptions options
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
            var fields = (await File.ReadAllTextAsync(file, stoppingToken))
                .TrimEnd('\r', '\n')
                .Split('\t');
            if (
                fields.Length != 3
                || fields[0] != options.OutageRunId
                || !long.TryParse(fields[1], out var sequence)
                || sequence <= previous
            )
            {
                await Task.Delay(10, stoppingToken);
                continue;
            }
            if (sequence != previous + 1)
                throw new InvalidOperationException("Outage command sequence skipped a phase.");
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
            deadline.CancelAfter(TimeSpan.FromSeconds(60));
            try
            {
                await store.RunCommandAsync(fields[2], services, deadline.Token);
            }
            catch (Exception error)
            {
                var message =
                    error.Message.StartsWith("deadline-exceeded|", StringComparison.Ordinal)
                    || error.Message.StartsWith("discarded|", StringComparison.Ordinal)
                        ? error.Message
                        : $"{error.GetType().Name}:{error.Message}";
                services
                    .GetRequiredService<TestHostEventSink>()
                    .Append(
                        $"outage-error|run={fields[0]}|seq={sequence}|phase={fields[2]}|error={message}"
                    );
                throw;
            }
            services
                .GetRequiredService<TestHostEventSink>()
                .Append($"outage-ack|run={fields[0]}|seq={sequence}|phase={fields[2]}");
            previous = sequence;
        }
    }
}

internal static class OutageActorObservations
{
    private static readonly ConcurrentDictionary<string, int> Creations = new();
    private static readonly ConcurrentDictionary<string, int> Handlers = new();
    private static readonly ConcurrentDictionary<(string ActorId, string Marker), int> Requests =
        new();

    internal static int Created(string actorId) =>
        Creations.AddOrUpdate(actorId, 1, (_, count) => count + 1);

    internal static int Count(string actorId) => Handlers.GetValueOrDefault(actorId);

    internal static int Count(string actorId, string marker) =>
        Requests.GetValueOrDefault((actorId, marker));

    internal static int Enter(string actorId, string marker)
    {
        Requests.AddOrUpdate((actorId, marker), 1, (_, count) => count + 1);
        return Handlers.AddOrUpdate(actorId, 1, (_, count) => count + 1);
    }
}

internal sealed record OutageProbeReq(string Marker);

internal sealed record OutageProbeRes(
    string ActorId,
    string NodeRid,
    string InstanceId,
    int CreationCount,
    int HandlerCount,
    string Marker
);

internal sealed class OutageProbeHandler
    : IZLinkEntrySpotActorRequestHandler<
        RelocationEntrySpot,
        RelocationActor,
        OutageProbeReq,
        OutageProbeRes
    >
{
    public ValueTask<OutageProbeRes> HandleAsync(
        RelocationEntrySpot spot,
        RelocationActor actor,
        IZLinkMessageContext context,
        OutageProbeReq request,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(
            new OutageProbeRes(
                actor.ActorId,
                spot.Context.NodeRid.ToString(),
                actor.InstanceId,
                actor.CreationCount,
                OutageActorObservations.Enter(actor.ActorId, request.Marker),
                request.Marker
            )
        );
    }
}
