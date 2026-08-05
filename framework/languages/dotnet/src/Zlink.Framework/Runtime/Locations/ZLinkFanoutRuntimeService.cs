using System.Runtime.CompilerServices;

using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkFanoutRuntimeService : IZLinkFanoutRuntime, IDisposable
{
    private readonly object _gate = new();
    private readonly HashSet<string> _automaticChannels;
    private readonly Dictionary<string, ChannelState> _states =
        new(StringComparer.Ordinal);
    private readonly Dictionary<
            string,
            List<ZLinkObservationQueue<ZLinkFanoutRuntimeEvent>>> _observers =
        new(StringComparer.Ordinal);
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
            .Select(static channel => channel.ChannelName)
            .ToHashSet(StringComparer.Ordinal);
        foreach (var channelName in _automaticChannels)
            _states[channelName] = ChannelState.Empty(channelName);
        _hostLifecycle.Changed += OnHostStateChanged;
    }

    internal ZLinkFanoutRuntimeService(ZLinkFrameworkRegistration registration)
        : this(registration, new ZLinkFrameworkHostLifecycleState())
    {
    }

    private ZLinkFanoutChannelSnapshot SnapshotInternal(string channelName)
    {
        lock (_gate)
            return RequireState(channelName).Snapshot;
    }

    public ZLinkFanoutStatus GetStatus(string channelName)
    {
        var snapshot = SnapshotInternal(channelName);
        var publishers = snapshot.Publishers
            .Select(static publisher => new ZLinkPeerStatus(
                publisher.PublisherRid,
                MapPeerState(publisher.State),
                MapUnavailableReason(publisher.State)))
            .ToArray();
        var hostState = _hostLifecycle.State;
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
        const int capacity = 1024;
        var observer = new ZLinkObservationQueue<ZLinkFanoutRuntimeEvent>(
            capacity,
            static item => item.Sequence,
            "fanout");
        lock (_gate)
        {
            _ = RequireState(channelName);
            if (!_observers.TryGetValue(channelName, out var observers))
                _observers[channelName] = observers = [];
            observers.Add(observer);
        }

        try
        {
            await foreach (var item in observer.ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
                yield return new ZLinkObservedStatus<ZLinkFanoutStatus>(
                    GetStatus(channelName),
                    item.Loss);
        }
        finally
        {
            lock (_gate)
                if (_observers.TryGetValue(channelName, out var observers))
                {
                    observers.Remove(observer);
                    if (observers.Count == 0)
                        _observers.Remove(channelName);
                }
            observer.Complete();
        }
    }

    internal void RecordSnapshot(
        string channelName,
        IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> publishers,
        ZLinkLocationRuntimeSnapshot location)
    {
        lock (_gate)
        {
            var previous = RequireState(channelName);
            var now = DateTimeOffset.UtcNow;
            var nextSequence = previous.Snapshot.Sequence;
            var previousByIdentity = previous.Snapshot.Publishers.ToDictionary(
                IdentityKey,
                StringComparer.Ordinal);

            foreach (var entry in publishers)
            {
                var key = IdentityKey(entry);
                if (previousByIdentity.Remove(key, out var old)
                    && old == entry)
                    continue;
                Emit(
                    channelName,
                    new ZLinkFanoutRuntimeEvent.PublisherChanged(
                        ++nextSequence,
                        now,
                        channelName,
                        entry));
            }

            foreach (var removed in previousByIdentity.Values)
                Emit(
                    channelName,
                    new ZLinkFanoutRuntimeEvent.PublisherChanged(
                        ++nextSequence,
                        now,
                        channelName,
                        removed with
                        {
                            ConnectionIntent = false,
                            Ready = false,
                            State = ZLinkFanoutPublisherConnectionState
                                .Disconnected
                        }));

            if (previous.Snapshot.Location != location)
                Emit(
                    channelName,
                    new ZLinkFanoutRuntimeEvent.LocationChanged(
                        ++nextSequence,
                        now,
                        channelName,
                        location));

            var ordered = publishers
                .OrderBy(static entry => entry.PublisherRid.ToHex(),
                    StringComparer.Ordinal)
                .ThenBy(static entry => entry.LifecycleGeneration)
                .ToArray();
            _states[channelName] = new ChannelState(
                new ZLinkFanoutChannelSnapshot(
                    channelName,
                    ordered.Count(static entry => entry.ConnectionIntent),
                    ordered.Count(static entry => entry.Ready),
                    nextSequence,
                    now,
                    ordered,
                    location));
        }
    }

    internal void RecordLocationFailure(
        string channelName,
        DateTimeOffset? lastSuccessAt,
        DateTimeOffset failureAt)
    {
        lock (_gate)
        {
            var current = RequireState(channelName);
            RecordSnapshot(
                channelName,
                current.Snapshot.Publishers,
                new ZLinkLocationRuntimeSnapshot(
                    "degraded",
                    lastSuccessAt,
                    failureAt));
        }
    }

    private ChannelState RequireState(string channelName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
        if (!_automaticChannels.Contains(channelName)
            || !_states.TryGetValue(channelName, out var state))
            throw new ZLinkConfigurationException(
                $"Fanout channel '{channelName}' is not an automatic subscriber.");
        return state;
    }

    private void Emit(string channelName, ZLinkFanoutRuntimeEvent item)
    {
        if (!_observers.TryGetValue(channelName, out var observers))
            return;
        foreach (var observer in observers.ToArray())
            observer.Publish(
                item,
                _hostLifecycle.State is ZLinkFrameworkRuntimeState.Stopped
                    or ZLinkFrameworkRuntimeState.Error);
    }

    private void OnHostStateChanged(ZLinkFrameworkRuntimeState _)
    {
        lock (_gate)
        {
            var now = DateTimeOffset.UtcNow;
            foreach (var (channelName, state) in _states.ToArray())
            {
                var sequence = checked(state.Snapshot.Sequence + 1);
                _states[channelName] = new ChannelState(state.Snapshot with
                {
                    Sequence = sequence,
                    ObservedAt = now
                });
                Emit(
                    channelName,
                    new ZLinkFanoutRuntimeEvent.RuntimeChanged(
                        sequence,
                        now,
                        channelName));
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

    private static string IdentityKey(
        ZLinkFanoutPublisherConnectionSnapshot entry) =>
        $"{entry.PublisherRid.ToHex()}:{entry.LifecycleGeneration}";

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

}
