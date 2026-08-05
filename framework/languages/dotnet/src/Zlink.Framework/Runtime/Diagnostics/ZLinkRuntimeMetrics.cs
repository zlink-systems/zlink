using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkRuntimeMetrics
{
    private static readonly Meter Meter = new(ZLinkMeters.Framework);

    private static readonly UpDownCounter<long> StreamConnectionsActive =
        Meter.CreateUpDownCounter<long>("zlink.stream.connections.active", "{connection}");
    private static readonly Counter<long> StreamConnectionsOpened =
        Meter.CreateCounter<long>("zlink.stream.connections.opened", "{connection}");
    private static readonly Counter<long> StreamConnectionsClosed =
        Meter.CreateCounter<long>("zlink.stream.connections.closed", "{connection}");
    private static readonly UpDownCounter<long> SpotCount =
        Meter.CreateUpDownCounter<long>("zlink.spot.count", "{spot}");
    private static readonly UpDownCounter<long> ActorCount =
        Meter.CreateUpDownCounter<long>("zlink.actor.count", "{actor}");

    private static readonly ConcurrentDictionary<
        object,
        Func<IReadOnlyList<ZLinkRuntimeMetricMeshSnapshot>>> MeshSnapshotProviders = new();
    private static readonly ObservableGauge<long> MeshPeersConfigured =
        Meter.CreateObservableGauge(
            "zlink.mesh_node.peers.configured",
            () => ObserveMeshPeerCount(static snapshot => snapshot.ConfiguredPeers),
            "{peer}");
    private static readonly ObservableGauge<long> MeshPeersConnected =
        Meter.CreateObservableGauge(
            "zlink.mesh_node.peers.connected",
            () => ObserveMeshPeerCount(static snapshot => snapshot.ConnectedPeers),
            "{peer}");
    private static readonly ObservableGauge<long> MeshPeersReady =
        Meter.CreateObservableGauge(
            "zlink.mesh_node.peers.ready",
            () => ObserveMeshPeerCount(static snapshot => snapshot.ReadyPeers),
            "{peer}");
    private static readonly ObservableGauge<long> MeshChannelsReadyMembers =
        Meter.CreateObservableGauge(
            "zlink.mesh_node.channels.ready_members",
            ObserveReadyMembers,
            "{member}");
    private static readonly Counter<long> MeshChannelSelectionFailures =
        Meter.CreateCounter<long>(
            "zlink.mesh_node.channel.selection_failures",
            "{failure}");
    private static readonly UpDownCounter<long> MeshRequestsInflight =
        Meter.CreateUpDownCounter<long>(
            "zlink.mesh_node.requests.inflight",
            "{request}");
    private static readonly Histogram<double> MeshRequestDuration =
        Meter.CreateHistogram<double>("zlink.mesh_node.request.duration", "s");
    private static readonly Counter<long> MeshRequestTimeouts =
        Meter.CreateCounter<long>("zlink.mesh_node.request.timeouts", "{request}");
    private static readonly Counter<long> MeshMessagesDropped =
        Meter.CreateCounter<long>("zlink.mesh_node.messages.dropped", "{message}");

    private static readonly ObservableGauge<long> ObjectCapacityActive =
        Meter.CreateObservableGauge(
            "zlink.object.capacity.active",
            () => ObservePopulationCapacity(static capacity => capacity.Active),
            "{object}");
    private static readonly ObservableGauge<long> ObjectCapacityReserved =
        Meter.CreateObservableGauge(
            "zlink.object.capacity.reserved",
            () => ObservePopulationCapacity(static capacity => capacity.Reserved),
            "{object}");
    private static readonly ObservableGauge<long> ObjectCapacityLimit =
        Meter.CreateObservableGauge(
            "zlink.object.capacity.limit",
            () => ObservePopulationCapacity(static capacity => capacity.Limit),
            "{object}");
    private static readonly ObservableGauge<long> SpotTypeCapacityActive =
        Meter.CreateObservableGauge(
            "zlink.spot.type.capacity.active",
            () => ObserveSpotTypeCapacity(static capacity => capacity.Active),
            "{spot}");
    private static readonly ObservableGauge<long> SpotTypeCapacityReserved =
        Meter.CreateObservableGauge(
            "zlink.spot.type.capacity.reserved",
            () => ObserveSpotTypeCapacity(static capacity => capacity.Reserved),
            "{spot}");
    private static readonly ObservableGauge<long> SpotTypeCapacityLimit =
        Meter.CreateObservableGauge(
            "zlink.spot.type.capacity.limit",
            () => ObserveSpotTypeCapacity(static capacity => capacity.Limit),
            "{spot}");
    private static readonly ObservableGauge<long> ObjectActivationActive =
        Meter.CreateObservableGauge(
            "zlink.object.activation.active",
            () => ObserveActivation(static activation => activation.Active),
            "{activation}");
    private static readonly ObservableGauge<long> ObjectActivationLimit =
        Meter.CreateObservableGauge(
            "zlink.object.activation.limit",
            () => ObserveActivation(static activation => activation.Limit),
            "{activation}");
    private static readonly Counter<long> RelocationStarted =
        Meter.CreateCounter<long>("zlink.relocation.started", "{relocation}");
    private static readonly Counter<long> RelocationCompleted =
        Meter.CreateCounter<long>("zlink.relocation.completed", "{relocation}");
    private static readonly Histogram<double> RelocationDuration =
        Meter.CreateHistogram<double>("zlink.relocation.duration", "s");
    private static readonly Histogram<long> RelocationBytes =
        Meter.CreateHistogram<long>("zlink.relocation.bytes", "By");
    private static readonly Histogram<double> RelocationInterruption =
        Meter.CreateHistogram<double>(
            "zlink.relocation.interruption",
            "s");

    private static readonly Counter<long> InstanceSpotActivations =
        Meter.CreateCounter<long>("zlink.instance_spot.activations", "{activation}");
    private static readonly Histogram<double> InstanceSpotActivationDuration =
        Meter.CreateHistogram<double>("zlink.instance_spot.activation.duration", "s");
    private static readonly ObservableGauge<long> InstanceSpotPendingMessages =
        Meter.CreateObservableGauge(
            "zlink.instance_spot.pending.messages",
            () => ObserveInstanceSpot(static instance => checked((long)instance.PendingMessages)),
            "{message}");
    private static readonly ObservableGauge<long> InstanceSpotPendingBytes =
        Meter.CreateObservableGauge(
            "zlink.instance_spot.pending.bytes",
            () => ObserveInstanceSpot(static instance => checked((long)instance.PendingBytes)),
            "By");
    private static readonly Counter<long> InstanceSpotClaimConflicts =
        Meter.CreateCounter<long>("zlink.instance_spot.claim.conflicts", "{claim}");
    private static readonly Counter<long> InstanceSpotTakeovers =
        Meter.CreateCounter<long>("zlink.instance_spot.takeovers", "{takeover}");

    private static readonly Counter<long> LocationStoreErrors =
        Meter.CreateCounter<long>("zlink.location.store.errors", "{error}");
    private static readonly Counter<long> LocationOwnerLeaseRenewFailures =
        Meter.CreateCounter<long>("zlink.location.owner_lease.renew.failures", "{failure}");
    private static readonly Histogram<double> LocationOwnerLeaseRenewLateness =
        Meter.CreateHistogram<double>("zlink.location.owner_lease.renew.lateness", "s");
    private static readonly Counter<long> ObserverOverflow =
        Meter.CreateCounter<long>("zlink.observability.events.overflow", "{event}");

    private static readonly ConcurrentDictionary<object, Func<string>> HostStateProviders = new();
    private static readonly ObservableGauge<long> HostState =
        Meter.CreateObservableGauge("zlink.host.state", ObserveHostStates, "{runtime}");
    private static readonly ConcurrentDictionary<
        object,
        Func<ZLinkInboundDispatchStatus>> HostInboundDispatchProviders = new();
    private static readonly ObservableGauge<long> HostInboundApplicationHwm =
        Meter.CreateObservableGauge(
            "zlink.host.inbound.application_hwm",
            () => ObserveHostInbound(static status => status.ApplicationHwmBytes),
            "By");
    private static readonly ObservableGauge<long> HostInboundPendingPayload =
        Meter.CreateObservableGauge(
            "zlink.host.inbound.pending_payload",
            ObserveHostInboundPendingPayload,
            "By");
    private static readonly ObservableGauge<long> HostInboundReceivePaused =
        Meter.CreateObservableGauge(
            "zlink.host.inbound.receive_paused",
            () => ObserveHostInbound(
                static status => status.ApplicationReceivePaused ? 1UL : 0UL),
            "{state}");
    private static readonly ObservableGauge<long> HostCompletionPendingSends =
        Meter.CreateObservableGauge(
            "zlink.host.completion.pending_sends",
            () => ObserveHostInbound(static status => status.PendingCompletionSends),
            "{request}");
    private static readonly ObservableGauge<long> HostCompletionSendLimit =
        Meter.CreateObservableGauge(
            "zlink.host.completion.send_limit",
            () => ObserveHostInbound(static status => status.CompletionSendLimit),
            "{request}");
    private static readonly Histogram<double> HostRelocationDuration =
        Meter.CreateHistogram<double>("zlink.host.relocation.duration", "s");
    private static readonly Counter<long> HostRelocationBlocked =
        Meter.CreateCounter<long>("zlink.host.relocation.blocked", "{operation}");
    private static readonly Histogram<double> HostShutdownDuration =
        Meter.CreateHistogram<double>("zlink.host.shutdown.duration", "s");
    private static readonly Counter<long> HostShutdownForced =
        Meter.CreateCounter<long>("zlink.host.shutdown.forced", "{operation}");

    public static void RecordStreamOpened(string transport)
    {
        SafeAdd(StreamConnectionsActive, 1, "transport", transport);
        SafeAdd(StreamConnectionsOpened, 1, "transport", transport);
    }

    public static void RecordStreamClosed(string transport, string closeReason)
    {
        SafeAdd(StreamConnectionsActive, -1, "transport", transport);
        SafeAdd(
            StreamConnectionsClosed,
            1,
            "transport",
            transport,
            "close_reason",
            NormalizeStreamCloseReason(closeReason));
    }

    public static void RecordSpotCreated(string meshName, string kind)
    {
        SafeAdd(
            SpotCount,
            1,
            new TagList { { "mesh_name", meshName }, { "spot_kind", kind } });
    }

    public static void RecordSpotClosed(string meshName, string kind)
    {
        SafeAdd(
            SpotCount,
            -1,
            new TagList { { "mesh_name", meshName }, { "spot_kind", kind } });
    }

    public static void RecordActorCreated(string meshName)
    {
        SafeAdd(ActorCount, 1, "mesh_name", meshName);
    }

    public static void RecordActorClosed(string meshName)
    {
        SafeAdd(ActorCount, -1, "mesh_name", meshName);
    }
    internal static bool RelocationInterruptionEnabled =>
        RelocationInterruption.Enabled;

    internal static void RecordRelocationInterruption(
        TimeSpan duration,
        string unitKind,
        string? executionMode)
    {
        if (!RelocationInterruption.Enabled) return;
        if (executionMode is null)
            SafeRecord(
                RelocationInterruption,
                Math.Max(0, duration.TotalSeconds),
                "unit_kind",
                unitKind);
        else
            SafeRecord(
                RelocationInterruption,
                Math.Max(0, duration.TotalSeconds),
                "unit_kind",
                unitKind,
                "execution_mode",
                executionMode);
    }

    public static ZLinkRelocationMetricOperation CreateRelocation(
        string meshName,
        ZLinkRelocationMetricObjectKind objectKind,
        ZLinkRelocationMetricPolicy policy)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        if (!RelocationStarted.Enabled
            && !RelocationCompleted.Enabled
            && !RelocationDuration.Enabled)
            return ZLinkRelocationMetricOperation.Disabled;

        return new ZLinkRelocationMetricOperation(
            meshName,
            RelocationObjectKind(objectKind),
            RelocationPolicy(policy));
    }

    public static ZLinkRelocationMetricOperation StartRelocation(
        string meshName,
        ZLinkRelocationMetricObjectKind objectKind,
        ZLinkRelocationMetricPolicy policy)
    {
        var operation = CreateRelocation(meshName, objectKind, policy);
        operation.Start();
        return operation;
    }

    public static ZLinkRequestMetricOperation StartRequest(
        string meshName,
        string surface)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        ArgumentException.ThrowIfNullOrWhiteSpace(surface);
        var operation = new ZLinkRequestMetricOperation(meshName, surface);
        operation.Start();
        return operation;
    }

    public static void RecordChannelSelectionFailure(
        string meshName,
        string channelName,
        string reason)
    {
        if (!MeshChannelSelectionFailures.Enabled) return;
        var tags = new TagList
        {
            { "mesh_name", meshName },
            { "channel_name", channelName },
            { "reason", reason }
        };
        SafeAdd(MeshChannelSelectionFailures, 1, tags);
    }

    public static void RecordMessageDropped(
        string meshName,
        string surface,
        string messageKind,
        string reason)
    {
        if (!MeshMessagesDropped.Enabled) return;
        try
        {
            MeshMessagesDropped.Add(
                1,
                new KeyValuePair<string, object?>("mesh_name", meshName),
                new KeyValuePair<string, object?>("surface", surface),
                new KeyValuePair<string, object?>("message_kind", messageKind),
                new KeyValuePair<string, object?>("reason", reason));
        }
        catch
        {
        }
    }

    public static IDisposable RegisterMeshSnapshots(
        Func<IReadOnlyList<ZLinkRuntimeMetricMeshSnapshot>> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var owner = new object();
        MeshSnapshotProviders[owner] = snapshot;
        return new ProviderRegistration(() => MeshSnapshotProviders.TryRemove(owner, out _));
    }

    public static IDisposable RegisterHostState(Func<string> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var owner = new object();
        HostStateProviders[owner] = snapshot;
        return new ProviderRegistration(() => HostStateProviders.TryRemove(owner, out _));
    }

    public static IDisposable RegisterHostInboundDispatch(
        Func<ZLinkInboundDispatchStatus> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var owner = new object();
        HostInboundDispatchProviders[owner] = snapshot;
        return new ProviderRegistration(
            () => HostInboundDispatchProviders.TryRemove(owner, out _));
    }

    public static void RecordLocationStoreError(string operation) =>
        SafeAdd(LocationStoreErrors, 1, "operation", operation);

    public static void RecordOwnerLeaseRenewFailure(
        string scopeKind,
        string scopeName) =>
        SafeAdd(
            LocationOwnerLeaseRenewFailures,
            1,
            "scope_kind",
            scopeKind,
            "scope_name",
            scopeName);

    public static void RecordOwnerLeaseRenewLateness(
        TimeSpan lateness,
        string scopeKind,
        string scopeName)
    {
        if (!LocationOwnerLeaseRenewLateness.Enabled) return;
        SafeRecord(
            LocationOwnerLeaseRenewLateness,
            Math.Max(0, lateness.TotalSeconds),
            "scope_kind",
            scopeKind,
            "scope_name",
            scopeName);
    }

    public static void RecordOwnerLeaseRenewAttempt(
        TimeProvider timeProvider,
        long scheduledTimestamp,
        string scopeKind,
        string scopeName)
    {
        if (scheduledTimestamp == 0 || !LocationOwnerLeaseRenewLateness.Enabled) return;
        var elapsedTicks = timeProvider.GetTimestamp() - scheduledTimestamp;
        if (elapsedTicks <= 0) return;
        SafeRecord(
            LocationOwnerLeaseRenewLateness,
            elapsedTicks / (double)timeProvider.TimestampFrequency,
            "scope_kind",
            scopeKind,
            "scope_name",
            scopeName);
    }

    public static void RecordObserverOverflow(string eventName)
    {
        if (!ObserverOverflow.Enabled) return;
        SafeAdd(ObserverOverflow, 1, "source", eventName);
    }

    public static ZLinkInstanceSpotMetricOperation StartInstanceSpotActivation(
        string meshName,
        string instanceSpotType)
    {
        var operation = new ZLinkInstanceSpotMetricOperation(
            meshName,
            instanceSpotType);
        operation.Start();
        return operation;
    }

    public static void RecordInstanceSpotClaimConflict(
        string meshName,
        string instanceSpotType,
        string reason)
    {
        var tags = new TagList
        {
            { "mesh_name", meshName },
            { "instance_spot_type", instanceSpotType },
            { "reason", reason }
        };
        SafeAdd(InstanceSpotClaimConflicts, 1, tags);
    }

    public static void RecordInstanceSpotTakeover(
        string meshName,
        string instanceSpotType,
        string outcome)
    {
        var tags = new TagList
        {
            { "mesh_name", meshName },
            { "instance_spot_type", instanceSpotType },
            { "outcome", outcome }
        };
        SafeAdd(InstanceSpotTakeovers, 1, tags);
    }

    public static ZLinkHostMetricOperation StartHostRelocation(string mode) =>
        new(HostRelocationDuration, mode, isRelocation: true);

    public static ZLinkHostMetricOperation StartHostShutdown() =>
        new(HostShutdownDuration, string.Empty, isRelocation: false);

    private static void CompleteHostOperation(
        ZLinkHostMetricOperation operation,
        string outcome,
        string reason)
    {
        if (operation.StartedTimestamp != 0 && operation.Histogram.Enabled)
        {
            var seconds = Stopwatch.GetElapsedTime(operation.StartedTimestamp).TotalSeconds;
            if (operation.IsRelocation)
                SafeRecord(
                    operation.Histogram,
                    seconds,
                    "mode",
                    operation.Mode,
                    "outcome",
                    outcome);
            else
                SafeRecord(operation.Histogram, seconds, "outcome", outcome);
        }
        if (operation.IsRelocation
            && string.Equals(outcome, "blocked", StringComparison.Ordinal))
            SafeAdd(
                HostRelocationBlocked,
                1,
                "mode",
                operation.Mode,
                "reason",
                reason);
        else if (!operation.IsRelocation
                 && string.Equals(outcome, "force_stopped", StringComparison.Ordinal))
            SafeAdd(HostShutdownForced, 1, "reason", reason);
    }

    private static IEnumerable<Measurement<long>> ObserveMeshPeerCount(
        Func<ZLinkRuntimeMetricMeshSnapshot, long> select)
    {
        foreach (var snapshot in MeshSnapshots())
            yield return new Measurement<long>(
                Math.Max(0, select(snapshot)),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName),
                new KeyValuePair<string, object?>("source", snapshot.Source));
    }

    private static IEnumerable<Measurement<long>> ObserveReadyMembers()
    {
        foreach (var snapshot in MeshSnapshots())
        foreach (var channel in snapshot.Channels)
            yield return new Measurement<long>(
                Math.Max(0, channel.ReadyMembers),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName),
                new KeyValuePair<string, object?>("channel_name", channel.ChannelName));
    }

    private static IEnumerable<Measurement<long>> ObservePopulationCapacity(
        Func<ZLinkRuntimeMetricCapacity, long> select)
    {
        foreach (var snapshot in MeshSnapshots())
        foreach (var (scope, capacity) in new[]
                 {
                     ("actor", snapshot.ActorCapacity),
                     ("spot", snapshot.SpotCapacity)
                 })
            yield return new Measurement<long>(
                Math.Max(0, select(capacity)),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName),
                new KeyValuePair<string, object?>("capacity_scope", scope));
    }

    private static IEnumerable<Measurement<long>> ObserveSpotTypeCapacity(
        Func<ZLinkRuntimeMetricSpotTypeCapacity, long> select)
    {
        foreach (var snapshot in MeshSnapshots())
        foreach (var capacity in snapshot.SpotTypeCapacities)
            yield return new Measurement<long>(
                Math.Max(0, select(capacity)),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName),
                new KeyValuePair<string, object?>("spot_kind", capacity.SpotKind),
                new KeyValuePair<string, object?>("stable_type", capacity.StableType));
    }

    private static IEnumerable<Measurement<long>> ObserveActivation(
        Func<ZLinkRuntimeMetricActivation, long> select)
    {
        foreach (var snapshot in MeshSnapshots())
            yield return new Measurement<long>(
                Math.Max(0, select(snapshot.Activation)),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName));
    }

    private static IEnumerable<Measurement<long>> ObserveInstanceSpot(
        Func<ZLinkRuntimeMetricInstanceSpot, long> select)
    {
        foreach (var snapshot in MeshSnapshots())
        foreach (var instance in snapshot.InstanceSpots)
            yield return new Measurement<long>(
                Math.Max(0, select(instance)),
                new KeyValuePair<string, object?>("mesh_name", snapshot.MeshName),
                new KeyValuePair<string, object?>(
                    "instance_spot_type",
                    instance.InstanceSpotType));
    }

    private static IEnumerable<ZLinkRuntimeMetricMeshSnapshot> MeshSnapshots()
    {
        foreach (var provider in MeshSnapshotProviders.Values)
        {
            IReadOnlyList<ZLinkRuntimeMetricMeshSnapshot> snapshots;
            try
            {
                snapshots = provider();
            }
            catch
            {
                continue;
            }
            foreach (var snapshot in snapshots)
                yield return snapshot;
        }
    }

    private static IEnumerable<Measurement<long>> ObserveHostStates()
    {
        foreach (var snapshot in HostStateProviders.Values)
        {
            string state;
            try
            {
                state = snapshot();
            }
            catch
            {
                continue;
            }
            yield return new Measurement<long>(
                1,
                new KeyValuePair<string, object?>("state", state));
        }
    }

    private static IEnumerable<Measurement<long>> ObserveHostInbound(
        Func<ZLinkInboundDispatchStatus, ulong> select)
    {
        foreach (var status in HostInboundDispatchSnapshots())
            yield return new Measurement<long>(ToMetricValue(select(status)));
    }

    private static IEnumerable<Measurement<long>> ObserveHostInboundPendingPayload()
    {
        foreach (var status in HostInboundDispatchSnapshots())
        {
            yield return new Measurement<long>(
                ToMetricValue(status.QueuedPayloadBytes),
                new KeyValuePair<string, object?>("state", "queued"));
            yield return new Measurement<long>(
                ToMetricValue(status.ActivePayloadBytes),
                new KeyValuePair<string, object?>("state", "active"));
        }
    }

    private static IEnumerable<ZLinkInboundDispatchStatus> HostInboundDispatchSnapshots()
    {
        foreach (var provider in HostInboundDispatchProviders.Values)
        {
            ZLinkInboundDispatchStatus status;
            try
            {
                status = provider();
            }
            catch
            {
                continue;
            }
            yield return status;
        }
    }

    private static long ToMetricValue(ulong value) =>
        value > long.MaxValue ? long.MaxValue : (long)value;

    private sealed class ProviderRegistration(Action unregister) : IDisposable
    {
        private Action? _unregister = unregister;

        public void Dispose() => Interlocked.Exchange(ref _unregister, null)?.Invoke();
    }

    private static string NormalizeStreamCloseReason(string closeReason) =>
        closeReason switch
        {
            "client_close" => "client_close",
            "idle_timeout" => "idle_timeout",
            "heartbeat_timeout" => "heartbeat_timeout",
            "server_shutdown" or "server_drain" => "server_shutdown",
            "protocol_error" => "protocol_error",
            _ => "transport_error"
        };

    private static long StartTimestamp(Histogram<double> histogram) =>
        histogram.Enabled ? Stopwatch.GetTimestamp() : 0;

    private static void RecordElapsed(Histogram<double> histogram, long startedTimestamp)
    {
        if (startedTimestamp == 0 || !histogram.Enabled) return;
        SafeRecord(histogram, Stopwatch.GetElapsedTime(startedTimestamp).TotalSeconds);
    }

    private static void RecordElapsed(
        Histogram<double> histogram,
        long startedTimestamp,
        string tagName,
        object? tagValue)
    {
        if (startedTimestamp == 0 || !histogram.Enabled) return;
        SafeRecord(
            histogram,
            Stopwatch.GetElapsedTime(startedTimestamp).TotalSeconds,
            tagName,
            tagValue);
    }

    private static void SafeAdd(Counter<long> counter, long value)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value);
        }
        catch
        {
        }
    }

    private static void SafeAdd(Counter<long> counter, long value, in TagList tags)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value, tags);
        }
        catch
        {
        }
    }

    private static void SafeAdd(
        Counter<long> counter,
        long value,
        string tagName,
        object? tagValue)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value, new KeyValuePair<string, object?>(tagName, tagValue));
        }
        catch
        {
        }
    }

    private static void SafeAdd(UpDownCounter<long> counter, long value)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value);
        }
        catch
        {
        }
    }

    private static void SafeAdd(
        UpDownCounter<long> counter,
        long value,
        string tagName,
        object? tagValue)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value, new KeyValuePair<string, object?>(tagName, tagValue));
        }
        catch
        {
        }
    }

    private static void SafeAdd(
        UpDownCounter<long> counter,
        long value,
        in TagList tags)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(value, tags);
        }
        catch
        {
        }
    }

    private static void SafeAdd(
        Counter<long> counter,
        long value,
        string firstTagName,
        object? firstTagValue,
        string secondTagName,
        object? secondTagValue)
    {
        if (!counter.Enabled) return;
        try
        {
            counter.Add(
                value,
                new KeyValuePair<string, object?>(firstTagName, firstTagValue),
                new KeyValuePair<string, object?>(secondTagName, secondTagValue));
        }
        catch
        {
        }
    }

    private static void SafeRecord(Histogram<double> histogram, double value)
    {
        if (!histogram.Enabled) return;
        try
        {
            histogram.Record(value);
        }
        catch
        {
        }
    }

    private static void SafeRecord(
        Histogram<double> histogram,
        double value,
        string tagName,
        object? tagValue)
    {
        if (!histogram.Enabled) return;
        try
        {
            histogram.Record(value, new KeyValuePair<string, object?>(tagName, tagValue));
        }
        catch
        {
        }
    }

    private static void SafeRecord(
        Histogram<double> histogram,
        double value,
        string firstTagName,
        object? firstTagValue,
        string secondTagName,
        object? secondTagValue)
    {
        if (!histogram.Enabled) return;
        try
        {
            histogram.Record(
                value,
                new KeyValuePair<string, object?>(firstTagName, firstTagValue),
                new KeyValuePair<string, object?>(secondTagName, secondTagValue));
        }
        catch
        {
        }
    }

    private static void SafeRecord(Histogram<long> histogram, long value)
    {
        if (!histogram.Enabled) return;
        try
        {
            histogram.Record(value);
        }
        catch
        {
        }
    }

    private static void SafeRecord(Histogram<long> histogram, long value, in TagList tags)
    {
        if (!histogram.Enabled) return;
        try
        {
            histogram.Record(value, tags);
        }
        catch
        {
        }
    }

    private static void CompleteRelocation(
        ZLinkRelocationMetricOperation operation,
        ZLinkRelocationMetricOutcome outcome)
    {
        var outcomeValue = RelocationOutcome(outcome);
        var terminalTags = operation.TerminalTags(outcomeValue);
        SafeAdd(RelocationCompleted, 1, terminalTags);
        if (operation.StartedTimestamp != 0 && RelocationDuration.Enabled)
            try
            {
                RelocationDuration.Record(
                    Stopwatch.GetElapsedTime(operation.StartedTimestamp).TotalSeconds,
                    terminalTags);
            }
            catch
            {
            }
    }

    internal sealed class ZLinkRequestMetricOperation
    {
        private readonly string _meshName;
        private readonly string _surface;
        private long _startedTimestamp;
        private int _completed;

        internal ZLinkRequestMetricOperation(string meshName, string surface)
        {
            _meshName = meshName;
            _surface = surface;
        }

        internal void Start()
        {
            var tags = new TagList
            {
                { "mesh_name", _meshName },
                { "surface", _surface }
            };
            SafeAdd(MeshRequestsInflight, 1, tags);
            _startedTimestamp = MeshRequestDuration.Enabled
                ? Stopwatch.GetTimestamp()
                : 0;
        }

        internal void Complete(string outcome)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            var tags = new TagList
            {
                { "mesh_name", _meshName },
                { "surface", _surface }
            };
            SafeAdd(MeshRequestsInflight, -1, tags);
            if (_startedTimestamp != 0 && MeshRequestDuration.Enabled)
            {
                tags.Add("outcome", outcome);
                try
                {
                    MeshRequestDuration.Record(
                        Stopwatch.GetElapsedTime(_startedTimestamp).TotalSeconds,
                        tags);
                }
                catch
                {
                }
            }
            if (string.Equals(outcome, "timed_out", StringComparison.Ordinal))
                SafeAdd(MeshRequestTimeouts, 1, new TagList
                {
                    { "mesh_name", _meshName },
                    { "surface", _surface }
                });
        }
    }

    internal sealed class ZLinkHostMetricOperation
    {
        private int _completed;

        internal ZLinkHostMetricOperation(
            Histogram<double> histogram,
            string mode,
            bool isRelocation)
        {
            Histogram = histogram;
            Mode = mode;
            IsRelocation = isRelocation;
            StartedTimestamp = histogram.Enabled ? Stopwatch.GetTimestamp() : 0;
        }

        internal Histogram<double> Histogram { get; }
        internal string Mode { get; }
        internal bool IsRelocation { get; }
        internal long StartedTimestamp { get; }

        internal void Complete(string outcome, string reason)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            CompleteHostOperation(this, outcome, reason);
        }
    }

    internal sealed class ZLinkInstanceSpotMetricOperation
    {
        private readonly string _meshName;
        private readonly string _instanceSpotType;
        private long _startedTimestamp;
        private int _completed;

        internal ZLinkInstanceSpotMetricOperation(
            string meshName,
            string instanceSpotType)
        {
            _meshName = meshName;
            _instanceSpotType = instanceSpotType;
        }

        internal void Start()
        {
            _startedTimestamp = InstanceSpotActivationDuration.Enabled
                ? Stopwatch.GetTimestamp()
                : 0;
        }

        internal void Complete(string outcome)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            var tags = new TagList
            {
                { "mesh_name", _meshName },
                { "instance_spot_type", _instanceSpotType },
                { "outcome", outcome }
            };
            SafeAdd(InstanceSpotActivations, 1, tags);
            if (_startedTimestamp != 0 && InstanceSpotActivationDuration.Enabled)
                try
                {
                    InstanceSpotActivationDuration.Record(
                        Stopwatch.GetElapsedTime(_startedTimestamp).TotalSeconds,
                        tags);
                }
                catch
                {
                }
        }
    }

    private static string RelocationObjectKind(ZLinkRelocationMetricObjectKind objectKind) =>
        objectKind switch
        {
            ZLinkRelocationMetricObjectKind.Actor => "actor",
            ZLinkRelocationMetricObjectKind.UserSpot => "user_spot",
            ZLinkRelocationMetricObjectKind.InstanceSpot => "instance_spot",
            _ => throw new ArgumentOutOfRangeException(nameof(objectKind))
        };

    private static string RelocationPolicy(ZLinkRelocationMetricPolicy policy) =>
        policy switch
        {
            ZLinkRelocationMetricPolicy.Recreate => "recreate",
            ZLinkRelocationMetricPolicy.Snapshot => "snapshot",
            _ => throw new ArgumentOutOfRangeException(nameof(policy))
        };

    private static string RelocationOutcome(ZLinkRelocationMetricOutcome outcome) =>
        outcome switch
        {
            ZLinkRelocationMetricOutcome.Completed => "completed",
            ZLinkRelocationMetricOutcome.Aborted => "aborted",
            ZLinkRelocationMetricOutcome.Recovered => "recovered",
            ZLinkRelocationMetricOutcome.Failed => "failed",
            ZLinkRelocationMetricOutcome.Shutdown => "shutdown",
            _ => throw new ArgumentOutOfRangeException(nameof(outcome))
        };

    internal sealed class ZLinkRelocationMetricOperation
    {
        internal static readonly ZLinkRelocationMetricOperation Disabled = new();

        private const int Created = 0;
        private const int Starting = 1;
        private const int Started = 2;
        private const int Completed = 3;

        private int _state;
        private long _startedTimestamp;

        private ZLinkRelocationMetricOperation()
        {
            MeshName = string.Empty;
            ObjectKind = string.Empty;
            Policy = string.Empty;
        }

        internal ZLinkRelocationMetricOperation(
            string meshName,
            string objectKind,
            string policy)
        {
            MeshName = meshName;
            ObjectKind = objectKind;
            Policy = policy;
        }

        private string MeshName { get; }
        private string ObjectKind { get; }
        private string Policy { get; }
        internal long StartedTimestamp => _startedTimestamp;

        internal TagList StartTags =>
            new()
            {
                { "mesh_name", MeshName },
                { "object_kind", ObjectKind },
                { "policy", Policy }
            };

        internal TagList ObjectTags =>
            new()
            {
                { "mesh_name", MeshName },
                { "object_kind", ObjectKind }
            };

        internal TagList TerminalTags(string outcome) =>
            new()
            {
                { "mesh_name", MeshName },
                { "object_kind", ObjectKind },
                { "policy", Policy },
                { "outcome", outcome }
            };

        internal void Complete(ZLinkRelocationMetricOutcome outcome)
        {
            if (ReferenceEquals(this, Disabled))
                return;
            var spinner = new SpinWait();
            while (Volatile.Read(ref _state) == Starting)
                spinner.SpinOnce();
            if (Interlocked.CompareExchange(ref _state, Completed, Started) != Started)
                return;
            CompleteRelocation(this, outcome);
        }

        internal void RecordBytes(long byteCount)
        {
            if (byteCount < 0)
                throw new ArgumentOutOfRangeException(nameof(byteCount));
            if (Volatile.Read(ref _state) != Started || !RelocationBytes.Enabled)
                return;
            SafeRecord(RelocationBytes, byteCount, StartTags);
        }

        internal void Start()
        {
            if (ReferenceEquals(this, Disabled)
                || Interlocked.CompareExchange(ref _state, Starting, Created) != Created)
                return;
            _startedTimestamp = RelocationDuration.Enabled
                ? Stopwatch.GetTimestamp()
                : 0;
            SafeAdd(RelocationStarted, 1, StartTags);
            Volatile.Write(ref _state, Started);
        }
    }
}

internal enum ZLinkRelocationMetricObjectKind
{
    Actor,
    UserSpot,
    InstanceSpot
}

internal enum ZLinkRelocationMetricPolicy
{
    Recreate,
    Snapshot
}

internal enum ZLinkRelocationMetricOutcome
{
    Completed,
    Aborted,
    Recovered,
    Failed,
    Shutdown
}

internal sealed record ZLinkRuntimeMetricMeshSnapshot(
    string MeshName,
    string Source,
    long ConfiguredPeers,
    long ConnectedPeers,
    long ReadyPeers,
    IReadOnlyList<ZLinkRuntimeMetricChannel> Channels,
    ZLinkRuntimeMetricCapacity ActorCapacity,
    ZLinkRuntimeMetricCapacity SpotCapacity,
    IReadOnlyList<ZLinkRuntimeMetricSpotTypeCapacity> SpotTypeCapacities,
    ZLinkRuntimeMetricActivation Activation,
    IReadOnlyList<ZLinkRuntimeMetricInstanceSpot> InstanceSpots);

internal sealed record ZLinkRuntimeMetricChannel(
    string ChannelName,
    long ReadyMembers);

internal sealed record ZLinkRuntimeMetricCapacity(
    long Active,
    long Reserved,
    long Limit);

internal sealed record ZLinkRuntimeMetricSpotTypeCapacity(
    string SpotKind,
    string StableType,
    long Active,
    long Reserved,
    long Limit);

internal sealed record ZLinkRuntimeMetricActivation(
    long Active,
    long Limit);

internal sealed record ZLinkRuntimeMetricInstanceSpot(
    string InstanceSpotType,
    ulong PendingMessages,
    ulong PendingBytes);
