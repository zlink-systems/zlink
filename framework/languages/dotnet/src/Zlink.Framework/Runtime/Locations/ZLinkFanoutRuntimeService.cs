using System.Runtime.CompilerServices;

using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkFanoutRuntimeService : IZLinkFanoutRuntime, IDisposable
{
    private readonly object _gate = new();
    private readonly HashSet<ZLinkChannelName> _automaticChannels;
    private readonly Dictionary<ZLinkChannelName, ChannelState> _states = [];
    private readonly Dictionary<
            ZLinkChannelName,
            List<ZLinkObservationQueue<RetainedObservation>>> _observers = [];
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle;

    internal ZLinkFanoutRuntimeService(
        ZLinkFrameworkRegistration registration,
        ZLinkFrameworkHostLifecycleState hostLifecycle)
    {
        _hostLifecycle = hostLifecycle;
        _automaticChannels = registration.Channels.Values
            .Where(static channel =>
                channel.AutoConnectType == ZLinkLocationAutoConnectType.Fanout
                && channel.Subscriber?.AutomaticDiscoveryEnabled == true)
            .Select(static channel =>
                ZLinkChannelName.FromBoundary(
                    channel.ChannelName,
                    nameof(channel.ChannelName)))
            .ToHashSet();
        foreach (var channelName in _automaticChannels)
            _states[channelName] = ChannelState.Empty(channelName.Value);
        _hostLifecycle.Changed += OnHostStateChanged;
    }

    internal ZLinkFanoutRuntimeService(ZLinkFrameworkRegistration registration)
        : this(registration, new ZLinkFrameworkHostLifecycleState())
    {
    }

    private ZLinkFanoutChannelSnapshot SnapshotInternal(string channelName)
    {
        var channel = Channel(channelName);
        lock (_gate)
            return RequireState(channel).Snapshot;
    }

    public ZLinkFanoutStatus GetStatus(string channelName)
    {
        var snapshot = SnapshotInternal(channelName);
        return Project(snapshot, _hostLifecycle.State);
    }

    private static ZLinkFanoutStatus Project(
        ZLinkFanoutChannelSnapshot snapshot,
        ZLinkFrameworkRuntimeState hostState)
    {
        var publishers = snapshot.Publishers
            .Select(static publisher => new ZLinkPeerStatus(
                publisher.PublisherRid,
                MapPeerState(publisher.State),
                MapUnavailableReason(publisher.State)))
            .ToArray();
        var isReady = hostState == ZLinkFrameworkRuntimeState.Serving
                      && snapshot.ReadyConnectionCount > 0;
        return new ZLinkFanoutStatus(
            snapshot.ChannelName,
            isReady
                ? ZLinkTopologyState.Ready
                : HostTopologyState(hostState),
            isReady,
            snapshot.ReadyConnectionCount,
            publishers,
            snapshot.Sequence,
            snapshot.ObservedAt);
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkFanoutStatus>> ObserveAsync(
        string channelName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        var channel = Channel(channelName);
        var observer = new ZLinkObservationQueue<RetainedObservation>(
            static item => item.SourceKey,
            eventName: "fanout");
        lock (_gate)
        {
            _ = RequireState(channel);
            if (!_observers.TryGetValue(channel, out var observers))
                _observers[channel] = observers = [];
            observers.Add(observer);
        }

        try
        {
            await foreach (var item in observer.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
                yield return new ZLinkObservedStatus<ZLinkFanoutStatus>(
                    item.Status.Status,
                    item.Loss);
        }
        finally
        {
            lock (_gate)
                if (_observers.TryGetValue(channel, out var observers))
                {
                    observers.Remove(observer);
                    if (observers.Count == 0)
                        _observers.Remove(channel);
                }
            observer.Complete();
        }
    }

    internal void RecordSnapshot(
        string channelName,
        IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> publishers,
        ZLinkLocationRuntimeSnapshot location)
    {
        var channel = Channel(channelName);
        lock (_gate)
        {
            var previous = RequireState(channel);
            var now = DateTimeOffset.UtcNow;
            var nextSequence = previous.Snapshot.Sequence;
            var previousByIdentity = previous.Snapshot.Publishers.ToDictionary(
                IdentityKey);
            var changes = new List<ZLinkFanoutRuntimeEvent>();

            foreach (var entry in publishers)
            {
                var key = IdentityKey(entry);
                if (previousByIdentity.Remove(key, out var old)
                    && old == entry)
                    continue;
                changes.Add(
                    new ZLinkFanoutRuntimeEvent.PublisherChanged(
                        ++nextSequence,
                        now,
                        channel.Value,
                        entry));
            }

            foreach (var removed in previousByIdentity.Values)
                changes.Add(
                    new ZLinkFanoutRuntimeEvent.PublisherChanged(
                        ++nextSequence,
                        now,
                        channel.Value,
                        removed with
                        {
                            ConnectionIntent = false,
                            Ready = false,
                            State = ZLinkFanoutPublisherConnectionState
                                .Disconnected
                        }));

            if (previous.Snapshot.Location != location)
                changes.Add(
                    new ZLinkFanoutRuntimeEvent.LocationChanged(
                        ++nextSequence,
                        now,
                        channel.Value,
                        location));

            var ordered = publishers
                .OrderBy(static entry => entry.PublisherRid,
                    ZLinkRoutingIdOrder.Instance)
                .ThenBy(static entry => entry.LifecycleGeneration)
                .ToArray();
            var next = new ZLinkFanoutChannelSnapshot(
                    channel.Value,
                    ordered.Count(static entry => entry.ConnectionIntent),
                    ordered.Count(static entry => entry.Ready),
                    nextSequence,
                    now,
                    ordered,
                    location);
            _states[channel] = new ChannelState(next);
            if (changes.Count != 0)
            {
                var hostState = _hostLifecycle.State;
                var retained = Project(next, hostState);
                foreach (var change in changes)
                    Emit(channel, change, retained, hostState);
            }
        }
    }

    internal void RecordLocationFailure(
        string channelName,
        DateTimeOffset? lastSuccessAt,
        DateTimeOffset failureAt)
    {
        var channel = Channel(channelName);
        lock (_gate)
        {
            var current = RequireState(channel);
            RecordSnapshot(
                channel.Value,
                current.Snapshot.Publishers,
                new ZLinkLocationRuntimeSnapshot(
                    "degraded",
                    lastSuccessAt,
                    failureAt));
        }
    }

    private ChannelState RequireState(ZLinkChannelName channelName)
    {
        if (!_automaticChannels.Contains(channelName)
            || !_states.TryGetValue(channelName, out var state))
            throw new ZLinkConfigurationException(
                $"Fanout channel '{channelName.Value}' is not an automatic subscriber.");
        return state;
    }

    private void Emit(
        ZLinkChannelName channelName,
        ZLinkFanoutRuntimeEvent item,
        ZLinkFanoutStatus status,
        ZLinkFrameworkRuntimeState hostState)
    {
        if (!_observers.TryGetValue(channelName, out var observers))
            return;
        foreach (var observer in observers.ToArray())
            observer.Publish(
                new RetainedObservation(item.SourceKey, status),
                item.IsTerminal
                || hostState is ZLinkFrameworkRuntimeState.Stopped
                    or ZLinkFrameworkRuntimeState.Error);
    }

    private void OnHostStateChanged(ZLinkFrameworkRuntimeState hostState)
    {
        lock (_gate)
        {
            var now = DateTimeOffset.UtcNow;
            foreach (var (channelName, state) in _states.ToArray())
            {
                var sequence = checked(state.Snapshot.Sequence + 1);
                var next = state.Snapshot with
                {
                    Sequence = sequence,
                    ObservedAt = now
                };
                _states[channelName] = new ChannelState(next);
                Emit(
                    channelName,
                    new ZLinkFanoutRuntimeEvent.RuntimeChanged(
                        sequence,
                        now,
                        channelName.Value),
                    Project(next, hostState),
                    hostState);
            }
        }
    }

    private static ZLinkTopologyState HostTopologyState(
        ZLinkFrameworkRuntimeState state) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
                or ZLinkFrameworkRuntimeState.Relocated
                or ZLinkFrameworkRuntimeState.Draining =>
                ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped =>
                ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => ZLinkTopologyState.Degraded
        };

    public void Dispose()
    {
        _hostLifecycle.Changed -= OnHostStateChanged;
        lock (_gate)
        {
            _observers.Clear();
        }
    }

    private static (RoutingId PublisherRid, ulong LifecycleGeneration) IdentityKey(
        ZLinkFanoutPublisherConnectionSnapshot entry) =>
        (entry.PublisherRid, entry.LifecycleGeneration);

    private static ZLinkChannelName Channel(string channelName) =>
        ZLinkChannelName.FromBoundary(channelName, nameof(channelName));

    private static ZLinkPeerState MapPeerState(
        ZLinkFanoutPublisherConnectionState state) =>
        state switch
        {
            ZLinkFanoutPublisherConnectionState.Ready => ZLinkPeerState.Ready,
            ZLinkFanoutPublisherConnectionState.Connecting
                or ZLinkFanoutPublisherConnectionState.Reconnecting =>
                ZLinkPeerState.Connecting,
            ZLinkFanoutPublisherConnectionState.ExcludedDraining =>
                ZLinkPeerState.Draining,
            _ => ZLinkPeerState.NotConnected
        };

    private static ZLinkTopologyReason? MapUnavailableReason(
        ZLinkFanoutPublisherConnectionState state) =>
        MapPeerState(state) switch
        {
            ZLinkPeerState.Ready => null,
            ZLinkPeerState.Draining => ZLinkTopologyReason.Draining,
            ZLinkPeerState.Connecting => ZLinkTopologyReason.NoReadyPeer,
            _ => ZLinkTopologyReason.InternalFailure
        };

    private sealed record ChannelState(ZLinkFanoutChannelSnapshot Snapshot)
    {
        internal static ChannelState Empty(string channelName)
        {
            var now = DateTimeOffset.UtcNow;
            return new ChannelState(
                new ZLinkFanoutChannelSnapshot(
                    channelName,
                    0,
                    0,
                    0,
                    now,
                    Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>(),
                    new ZLinkLocationRuntimeSnapshot(
                        "unknown",
                        null,
                        null)));
        }
    }

    private sealed record RetainedObservation(
        string SourceKey,
        ZLinkFanoutStatus Status);

}
