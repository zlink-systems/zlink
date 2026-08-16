using System.Collections.Concurrent;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Messaging;
using Peer = Zlink.Framework.Runtime.Service.ZLinkMeshPeer;
using OwnedMailbox = Zlink.Framework.Runtime.Service.ZLinkMeshNodeOwnedMailbox;
using QueuedRecord = Zlink.Framework.Runtime.Service.ZLinkMeshQueuedRecord;

namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkManagedMeshNode : IMeshNode
{
    private const int ReceiveBatchSize = 64;
    private const int DefaultMaxPendingOperations = 65_536;
    private const int MaxRemoteUserSpotOperations = 4_096;
    private const int MaxRemoteActorCreateOperations = 4_096;
    private const int MaxRelocationReplyTerminals = 65_536;
    private const int MaxInfrastructureControlParts = 64;
    private const long MaxInfrastructureControlBytes = 256 * 1024;
    private const long MaxInfrastructurePayloadBytes = 4_294_966_774L;
    private static readonly TimeSpan DefaultInboundOperationShutdownTimeout =
        TimeSpan.FromSeconds(2);
    private static readonly TimeSpan DefaultRemoteUserSpotTerminalRetention =
        TimeSpan.FromMinutes(5);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan TransportShutdownGrace = PollInterval + PollInterval;
    private static readonly TimeSpan AdmissionRetryInterval = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan RelocationAckRetryInterval =
        TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan ServiceTerminalRetryTimeout =
        TimeSpan.FromSeconds(5);

    private readonly IContext _context;
    private readonly string _meshName;
    private readonly int _maxPendingOperations;
    private readonly TimeSpan _remoteUserSpotTerminalRetention;
    private readonly TimeSpan _inboundOperationShutdownTimeout;
    private readonly ZLinkDeadlineClock _deadlineClock;
    private readonly ZLinkApplicationJobQueue? _applicationJobQueue;
    private readonly object _gate = new();
    private readonly object _socketGate = new();
    private readonly object _readyGate = new();
    private readonly object _operationGate = new();
    private readonly object _remoteUserSpotGate = new();
    private readonly object _remoteActorCreateGate = new();
    private readonly object _inboundOperationGate = new();
    private readonly object _disposeGate = new();
    private readonly Dictionary<ZLinkChannelName, uint> _channels = new();
    private readonly Dictionary<ulong, Peer> _peersByIntent = new();
    private readonly Dictionary<RoutingId, Peer> _peersByRid = new();
    private readonly Dictionary<RoutingId, ZLinkMeshPeerExpectation> _peerExpectations = new();
    private readonly ZLinkMeshPeerAdmission _peerAdmission = new();
    private readonly ConcurrentDictionary<MailboxKey, OwnedMailbox> _ownedMailboxes = new();
    private readonly ConcurrentDictionary<ulong, PendingOperation> _operations = new();
    private readonly Dictionary<RelocationReplyTerminalKey, RelocationReplyTerminal>
        _relocationReplyTerminals = [];
    private readonly Dictionary<RelocationReplyTerminalKey, PendingOperation>
        _relocationReplyOperations = [];
    private readonly Queue<RelocationReplyTerminalKey>
        _relocationReplyTerminalOrder = [];
    private readonly ConcurrentDictionary<ZLinkSpotId, ZLinkManagedSpot> _spots = new();
    private readonly object _entrySpotGate = new();
    private readonly ConcurrentDictionary<ZLinkActorId, ManagedActor> _actors = new();
    private readonly ConcurrentDictionary<RemoteUserSpotOperationKey, RemoteUserSpotInvocation>
        _remoteUserSpotOperations = new();
    private readonly ConcurrentDictionary<RemoteActorCreateOperationKey, RemoteActorCreateInvocation>
        _remoteActorCreateOperations = new();
    private readonly ConcurrentDictionary<PendingReplyRelayKey, PendingReplyRelay>
        _pendingReplyRelays = new();
    private readonly ConcurrentDictionary<PendingRelocationPrepareKey,
        PendingRelocationPrepare> _pendingRelocationPrepares = new();
    private readonly ConcurrentDictionary<PendingSessionRelocationKey,
        PendingSessionRelocationSeal> _pendingSessionRelocationSeals = new();
    private readonly ConcurrentDictionary<PendingSessionRelocationKey,
        ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        _sessionRelocationSealReplyTerminals = new();
    private readonly ConcurrentQueue<PendingSessionRelocationKey>
        _sessionRelocationSealReplyTerminalOrder = new();
    private readonly ConcurrentDictionary<ObservedSpotAuthorityKey, ObservedAuthority>
        _observedSpotAuthorities = new();
    private readonly ConcurrentDictionary<ObservedActorAuthorityKey, ObservedAuthority>
        _observedActorAuthorities = new();
    private readonly ConcurrentQueue<RoutingId> _transportDisconnects = new();
    private readonly List<RawMeshMonitor> _monitors = new();
    private readonly HashSet<Task> _inboundOperations = [];
    private readonly ulong _lifecycleGeneration = NewNonZeroToken();

    private IRouterSocket? _socket;
    private ISocketMonitor? _socketMonitor;
    private IPoller? _poller;
    private CancellationTokenSource? _stop;
    private Task? _receiveLoop;
    private Task? _disposeTask;
    private Func<MeshReadyDomains, MeshReadyDomains>? _readyHandler;
    private Action<MeshReceiveRecord, IReadOnlyList<Message>>?
        _completionOverflowHandler;
    private IUserSpotOperationTarget? _userSpotOperationTarget;
    private IActorCreateOperationTarget? _actorCreateOperationTarget;
    private IActorDestroyOperationTarget? _actorDestroyOperationTarget;
    private IActorMessageFollowIngressTarget? _actorMessageFollowIngressTarget;
    private Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord>?
        _messageFollowNotificationHandler;
    private Action<RoutingId, ZLinkServiceWireCodec.BoundSessionReplacedRecord>?
        _boundSessionReplacedNotificationHandler;
    private IInstanceSpotActivationTarget? _instanceSpotActivationTarget;
    private IRelocationReplyRelayTarget? _relocationReplyRelayTarget;
    private ICanonicalRelocationTarget? _canonicalRelocationTarget;
    private ISessionRelocationBarrierTarget? _sessionRelocationBarrierTarget;
    private RoutingId _routingId;
    private ZLinkManagedSpot? _entrySpot;
    private ZLinkMeshNodeObjectRole _objectRole;
    private string _bindEndpoint = string.Empty;
    private string _advertisedEndpoint = string.Empty;
    private MeshNodeState _state = MeshNodeState.Created;
    private ulong _descriptorRevision = 1;
    private ulong _nextIntent;
    private ulong _nextPeerConnectionGeneration;
    private ulong _nextOperation;
    private ulong _nextActorGeneration;
    private ulong _nextSpotGeneration;
    private ulong _nextAuthorityOwnerGeneration;
    private long _localOwnerLeaseGeneration = 1;
    private ZLinkServiceWireCodec.RequestSourceFence _localRequestSourceFence;
    private readonly ZLinkMeshChannelSelection _channelSelection = new();
    private long _queuedMessages;
    private long _queuedBytes;
    private int _readyPosted;
    private int _disposed;
    private bool _inboundOperationAdmissionClosed;
    private ulong _activeSocketGeneration;

    internal ZLinkManagedMeshNode(
        IContext context,
        string meshName,
        int maxPendingOperations = DefaultMaxPendingOperations,
        TimeSpan? remoteUserSpotTerminalRetention = null,
        TimeSpan? inboundOperationShutdownTimeout = null,
        TimeProvider? deadlineTimeProvider = null,
        ZLinkApplicationJobQueue? applicationJobQueue = null)
    {
        _context = context ?? throw new ArgumentNullException(nameof(context));
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        if (maxPendingOperations <= 0)
            throw new ArgumentOutOfRangeException(nameof(maxPendingOperations));
        if (remoteUserSpotTerminalRetention is { } retention
            && retention < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(remoteUserSpotTerminalRetention));
        if (inboundOperationShutdownTimeout is { } shutdownTimeout
            && shutdownTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(inboundOperationShutdownTimeout));
        _meshName = meshName;
        _maxPendingOperations = maxPendingOperations;
        _remoteUserSpotTerminalRetention =
            remoteUserSpotTerminalRetention
            ?? DefaultRemoteUserSpotTerminalRetention;
        _inboundOperationShutdownTimeout =
            inboundOperationShutdownTimeout
            ?? DefaultInboundOperationShutdownTimeout;
        _deadlineClock = new ZLinkDeadlineClock(
            deadlineTimeProvider ?? TimeProvider.System);
        _applicationJobQueue = applicationJobQueue;
    }

    public RoutingId RoutingId => _routingId;
    internal string MeshName => _meshName;
    public ulong RouterHighWaterMark { get; set; } = 4_096_000;
    public ulong RouterReceiveHighWaterMark { get; set; } = 4_096_000;
    public ulong MailboxMessageBudget { get; set; } = 10_000;
    public ulong MailboxByteBudget { get; set; } = 64 * 1024 * 1024;
    public TimeSpan? ReceiveTimeout { get; set; }
    public TimeSpan? SendTimeout { get; set; }

    public void SetRoutingId(RoutingId routingId)
    {
        ThrowIfStarted();
        if (routingId.IsEmpty)
            throw new ArgumentException("Routing id is required.", nameof(routingId));
        _routingId = routingId;
    }

    public void SetObjectRole(ZLinkMeshNodeObjectRole objectRole)
    {
        ThrowIfStarted();
        if (!Enum.IsDefined(objectRole))
            throw new ArgumentOutOfRangeException(nameof(objectRole));
        _objectRole = objectRole;
    }

    public void SetBind(string endpoint)
    {
        ThrowIfStarted();
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        _bindEndpoint = endpoint;
        _advertisedEndpoint = endpoint;
    }

    public void SetAdvertisedEndpoint(string endpoint)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        lock (_gate)
        {
            ThrowIfDisposed();
            _advertisedEndpoint = endpoint;
        }
    }

    public void Start()
    {
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_state != MeshNodeState.Created)
                return;
            if (_routingId.IsEmpty)
                throw new InvalidOperationException("A MeshNode routing id is required before Start.");
            if (_bindEndpoint.Length == 0)
            {
                _bindEndpoint = $"inproc://zlink-framework-{Guid.NewGuid():N}";
                if (_advertisedEndpoint.Length == 0)
                    _advertisedEndpoint = _bindEndpoint;
            }

            var socket = _context.CreateRouterSocket();
            ISocketMonitor? socketMonitor = null;
            IPoller? poller = null;
            try
            {
                socket.Options.Mandatory = true;
                socket.Options.Handover = true;
                socket.Options.Linger = TimeSpan.Zero;
                // RouteMesh has no Framework message-size contract. Explicitly
                // disable the native inbound cap instead of relying on the
                // binding or Core default.
                socket.Options.MaxMessageSize = -1;
                socket.Options.SendHighWaterMark = RouterHighWaterMark;
                socket.Options.ReceiveHighWaterMark = RouterReceiveHighWaterMark;
                if (ReceiveTimeout is { } receiveTimeout)
                    socket.Options.ReceiveTimeout = receiveTimeout;
                if (SendTimeout is { } timeout)
                    socket.Options.SendTimeout = timeout;
                socket.SetRoutingId(_routingId);
                var configuredBindEndpoint = _bindEndpoint;
                socket.Bind(configuredBindEndpoint);
                _bindEndpoint = socket.Options.LastEndpoint;
                if (string.Equals(
                        _advertisedEndpoint,
                        configuredBindEndpoint,
                        StringComparison.Ordinal))
                    _advertisedEndpoint = _bindEndpoint;

                socketMonitor = socket.MonitorOpen(
                    SocketEvent.Disconnected | SocketEvent.Closed);

                poller = Systems.Zlink.Zlink.CreatePoller();
                poller.Add(
                    socket,
                    PollEventFlags.PollIn | PollEventFlags.PollErr,
                    1);
                _socket = socket;
                _socketMonitor = socketMonitor;
                _activeSocketGeneration = _lifecycleGeneration;
                _poller = poller;
                socketMonitor = null;
                poller = null;
                _stop = new CancellationTokenSource();
                _state = MeshNodeState.Started;
                Publish(MeshMonitorEventKind.StateChanged);
                foreach (var peer in _peersByIntent.Values)
                    ConnectPeerCore(peer);
                _receiveLoop = Task.Factory.StartNew(
                        () => ReceiveLoop(_stop.Token),
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default)
                    .Unwrap();
            }
            catch
            {
                _poller?.Dispose();
                _poller = null;
                poller?.Dispose();
                _socketMonitor?.Dispose();
                _socketMonitor = null;
                socketMonitor?.Dispose();
                socket.Dispose();
                _socket = null;
                _activeSocketGeneration = 0;
                _state = MeshNodeState.Error;
                throw;
            }
        }
    }

    public ulong ConnectPeer(
        string endpoint,
        RoutingId? expectedRid = null,
        string expectedSecurityIdentity = ZLinkServiceSecurityIdentity.Plaintext)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedSecurityIdentity);
        lock (_gate)
        {
            ThrowIfDisposed();
            var intent = checked(++_nextIntent);
            var peer = new Peer(
                intent,
                endpoint,
                expectedRid,
                expectedSecurityIdentity,
                ZLinkServiceConnectionDirection.Outbound,
                checked(++_nextPeerConnectionGeneration));
            _peersByIntent.Add(intent, peer);
            if (_state != MeshNodeState.Created)
                ConnectPeerCore(peer);
            return intent;
        }
    }

    public void SetPeerExpectation(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity,
        ulong expectedLifecycleGeneration)
    {
        if (peerRid.IsEmpty)
            throw new ArgumentException("Peer routing id is required.", nameof(peerRid));
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedSecurityIdentity);
        lock (_gate)
            _peerExpectations[peerRid] = new ZLinkMeshPeerExpectation(
                endpoint,
                expectedSecurityIdentity,
                expectedLifecycleGeneration);
    }

    public void RemovePeerExpectation(RoutingId peerRid, string endpoint)
    {
        lock (_gate)
        {
            if (_peerExpectations.TryGetValue(peerRid, out var expected)
                && string.Equals(
                    expected.Endpoint,
                    endpoint,
                    StringComparison.Ordinal))
                _peerExpectations.Remove(peerRid);
        }
    }

    public void RemovePeerConnection(ulong connectionIntentId)
    {
        lock (_gate)
        {
            if (!_peersByIntent.Remove(connectionIntentId, out var peer))
                return;
            RemovePeer(peer, disconnect: true);
        }
    }

    public bool RemovePeerConnectionIfNotAdmitted(ulong connectionIntentId)
    {
        lock (_gate)
        {
            if (!_peersByIntent.TryGetValue(connectionIntentId, out var peer))
                return true;
            if (peer.State is MeshPeerState.Admitted or MeshPeerState.Draining)
                return false;
            var replacementUsesEndpoint = _peersByIntent.Values.Any(otherPeer =>
                !ReferenceEquals(otherPeer, peer)
                && string.Equals(
                    otherPeer.Endpoint,
                    peer.Endpoint,
                    StringComparison.Ordinal)
                && otherPeer.State != MeshPeerState.Closed);
            _peersByIntent.Remove(connectionIntentId);
            // Socket disconnect is endpoint-scoped. A stale non-admitted intent
            // must not tear down a replacement intent that already claimed the
            // same endpoint during a rolling RID handover.
            RemovePeer(peer, disconnect: !replacementUsesEndpoint);
            return true;
        }
    }

    public void DisconnectPeer(RoutingId peerRid, ulong lifecycleGeneration = 0)
    {
        lock (_gate)
        {
            if (!_peersByRid.TryGetValue(peerRid, out var peer))
                return;
            if (lifecycleGeneration != 0
                && lifecycleGeneration != peer.LifecycleGeneration)
                return;
            RemovePeer(peer, disconnect: true);
        }
    }

    public void AddChannel(string channelName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
        if (Encoding.UTF8.GetByteCount(channelName) > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(channelName));
        var channelKey = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        Peer[] peers;
        lock (_gate)
        {
            ThrowIfDisposed();
            if (!_channels.TryAdd(channelKey, 100))
                return;
            _descriptorRevision = checked(_descriptorRevision + 1);
            RebuildChannelSelectionPlansUnderLock();
            peers = _peersByRid.Values
                .Where(static peer => peer.Admitted)
                .ToArray();
        }
        foreach (var peer in peers)
            SendAdmission(peer, ServiceWireConstants.Command.Update);
        Publish(MeshMonitorEventKind.ChannelChanged, channelName: channelName);
    }

    public void SetChannelWeight(string channelName, uint weight)
    {
        if (weight > ZLinkSocketConfig.MaximumPeerWeight)
            throw new ArgumentOutOfRangeException(nameof(weight));
        var channelKey = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        Peer[] peers;
        lock (_gate)
        {
            if (!_channels.ContainsKey(channelKey))
                throw new InvalidOperationException($"Channel '{channelName}' is not registered.");
            _channels[channelKey] = weight;
            _descriptorRevision = checked(_descriptorRevision + 1);
            RebuildChannelSelectionPlansUnderLock();
            peers = _peersByRid.Values
                .Where(static peer => peer.Admitted)
                .ToArray();
        }
        foreach (var peer in peers)
            SendAdmission(peer, ServiceWireConstants.Command.Update);
        Publish(MeshMonitorEventKind.ChannelChanged, channelName: channelName);
    }

    //  Spec 28 §11 step 2: host를 Draining으로 바꾼 뒤 그 descriptor를 게시해
    //  peer의 새 selection과 placement에서 빠진다. Descriptor revision을 올려
    //  같은 lifecycle generation의 reader가 최신 snapshot만 적용하게 한다
    //  (SetChannelWeight와 같은 방식).
    public void PublishDraining()
    {
        Peer[] peers;
        lock (_gate)
        {
            if (_state == MeshNodeState.Draining) return;
            _state = MeshNodeState.Draining;
            _descriptorRevision = checked(_descriptorRevision + 1);
            RebuildChannelSelectionPlansUnderLock();
            peers = _peersByRid.Values
                .Where(static peer => peer.Admitted)
                .ToArray();
        }
        foreach (var peer in peers)
            SendAdmission(peer, ServiceWireConstants.Command.Update);
        Publish(MeshMonitorEventKind.StateChanged);
    }

    public void SetUserSpotOperationTarget(IUserSpotOperationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_userSpotOperationTarget is not null
                && !ReferenceEquals(_userSpotOperationTarget, target))
                throw new InvalidOperationException(
                    "A User Spot operation target is already registered.");
            _userSpotOperationTarget = target;
        }
    }

    public void SetActorCreateOperationTarget(IActorCreateOperationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_actorCreateOperationTarget is not null
                && !ReferenceEquals(_actorCreateOperationTarget, target))
                throw new InvalidOperationException(
                    "An Actor create operation target is already registered.");
            _actorCreateOperationTarget = target;
        }
    }

    public void SetActorDestroyOperationTarget(IActorDestroyOperationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_actorDestroyOperationTarget is not null
                && !ReferenceEquals(_actorDestroyOperationTarget, target))
                throw new InvalidOperationException(
                    "An Actor destroy operation target is already registered.");
            _actorDestroyOperationTarget = target;
        }
    }

    public void SetActorMessageFollowIngressTarget(
        IActorMessageFollowIngressTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_actorMessageFollowIngressTarget is not null
                && !ReferenceEquals(_actorMessageFollowIngressTarget, target))
                throw new InvalidOperationException(
                    "An Actor Message Follow ingress target is already registered.");
            _actorMessageFollowIngressTarget = target;
        }
    }

    internal void SetMessageFollowNotificationHandler(
        Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_messageFollowNotificationHandler is not null
                && !ReferenceEquals(_messageFollowNotificationHandler, handler))
                throw new InvalidOperationException(
                    "A Message Follow notification handler is already registered.");
            _messageFollowNotificationHandler = handler;
        }
    }

    internal bool TrySendMessageFollowNotification(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.MessageFollowRecord record)
    {
        var encoded = ZLinkServiceWireCodec.EncodeMessageFollow(record);
        if (targetNodeRid == _routingId)
        {
            Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord>?
                handler;
            lock (_gate)
                handler = _messageFollowNotificationHandler;
            handler?.Invoke(_routingId, record);
            return handler is not null;
        }

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            return false;
        return TryScheduleRoutedSend(
            peer.PhysicalRoutingId,
            [encoded]);
    }

    internal void SetBoundSessionReplacedNotificationHandler(
        Action<RoutingId, ZLinkServiceWireCodec.BoundSessionReplacedRecord> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_boundSessionReplacedNotificationHandler is not null
                && !ReferenceEquals(
                    _boundSessionReplacedNotificationHandler,
                    handler))
                throw new InvalidOperationException(
                    "A bound-session replacement notification handler is already registered.");
            _boundSessionReplacedNotificationHandler = handler;
        }
    }

    internal bool TrySendBoundSessionReplacedNotification(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.BoundSessionReplacedRecord record)
    {
        var encoded = ZLinkServiceWireCodec.EncodeBoundSessionReplaced(record);
        if (targetNodeRid == _routingId)
        {
            Action<RoutingId, ZLinkServiceWireCodec.BoundSessionReplacedRecord>?
                handler;
            lock (_gate)
                handler = _boundSessionReplacedNotificationHandler;
            handler?.Invoke(_routingId, record);
            return handler is not null;
        }

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            return false;
        return TryScheduleRoutedSend(
            peer.PhysicalRoutingId,
            [encoded]);
    }

    public void SetInstanceSpotActivationTarget(IInstanceSpotActivationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_instanceSpotActivationTarget is not null
                && !ReferenceEquals(_instanceSpotActivationTarget, target))
                throw new InvalidOperationException(
                    "An Instance Spot activation target is already registered.");
            _instanceSpotActivationTarget = target;
        }
    }

    public void SetRelocationReplyRelayTarget(IRelocationReplyRelayTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            if (_relocationReplyRelayTarget is not null
                && !ReferenceEquals(_relocationReplyRelayTarget, target))
                throw new InvalidOperationException(
                    "A relocation reply relay target is already registered.");
            _relocationReplyRelayTarget = target;
        }
    }

    public void SetCanonicalRelocationTarget(
        ICanonicalRelocationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            if (_canonicalRelocationTarget is not null
                && !ReferenceEquals(_canonicalRelocationTarget, target))
                throw new InvalidOperationException(
                    "A canonical relocation target is already registered.");
            _canonicalRelocationTarget = target;
        }
    }

    public void SetSessionRelocationBarrierTarget(
        ISessionRelocationBarrierTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            if (_sessionRelocationBarrierTarget is not null
                && !ReferenceEquals(_sessionRelocationBarrierTarget, target))
                throw new InvalidOperationException(
                    "A session relocation barrier target is already registered.");
            _sessionRelocationBarrierTarget = target;
        }
    }

    public async ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord>
        RelayRelocationReplyAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.ReplyRelayRecord relay,
            ZLinkServiceWireCodec.RequestSourceFence expectedSource,
            IReadOnlyList<Message> payload,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(payload);
        if (expectedSource.NodeRid != targetNodeRid)
            throw new ArgumentException(
                "The expected request source must identify the ACK target.",
                nameof(expectedSource));
        if (!IsReplyRelayPayloadAllowed(relay.TerminalResult, payload.Count))
            throw new ArgumentException(
                "A failed relocation reply cannot carry application payload.",
                nameof(payload));
        var key = PendingReplyRelayKey.Create(targetNodeRid, relay);
        var pending = new PendingReplyRelay(expectedSource, relay);
        if (!_pendingReplyRelays.TryAdd(key, pending))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The relocation reply is already awaiting an acknowledgement.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        try
        {
            Peer? peer;
            lock (_gate)
                _peersByRid.TryGetValue(targetNodeRid, out peer);
            if (peer is null || !peer.Admitted)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"MeshNode '{targetNodeRid}' is not connected.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            var wire = new List<ReadOnlyMemory<byte>>(payload.Count == 0 ? 1 : 2)
            {
                ZLinkServiceWireCodec.EncodeReplyRelay(relay)
            };
            if (payload.Count != 0)
                wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(payload));
            try
            {
                await SendRoutedAsync(
                        peer.PhysicalRoutingId,
                        wire,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZlinkException exception)
            {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "The relocation reply relay could not be submitted.",
                    ZLinkRetryAdvice.RetryAfterBackoff,
                    exception);
            }
            var effectiveTimeout = timeout <= TimeSpan.Zero
                ? TimeSpan.FromSeconds(30)
                : timeout;
            return await pending.Completion.Task
                .WaitAsync(effectiveTimeout, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _pendingReplyRelays.TryRemove(
                new KeyValuePair<PendingReplyRelayKey, PendingReplyRelay>(
                    key,
                    pending));
        }
    }

    public async ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        PrepareCanonicalRelocationAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted
            || peer.LifecycleGeneration != prepare.Target.NodeGeneration
            || prepare.Target.NodeRid != targetNodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var key = new PendingRelocationPrepareKey(
            targetNodeRid, prepare.RelocationId,
            prepare.TargetAttemptGeneration, prepare.Coordinator);
        var fingerprint = SHA256.HashData(
            ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare));
        var created = new PendingRelocationPrepare(prepare, fingerprint);
        PendingRelocationPrepare pending;
        while (true)
        {
            if (_pendingRelocationPrepares.TryAdd(key, created))
            {
                pending = created;
                break;
            }
            if (!_pendingRelocationPrepares.TryGetValue(key, out pending!))
                continue;
            if (!pending.Fingerprint.AsSpan().SequenceEqual(fingerprint))
                throw new InvalidDataException(
                    "A canonical relocation retry changed command 40 fields.");
            if (pending.TryAcquireWaiter())
                break;
            _pendingRelocationPrepares.TryRemove(
                new KeyValuePair<PendingRelocationPrepareKey,
                    PendingRelocationPrepare>(key, pending));
        }
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _stop?.Token ?? CancellationToken.None);
        deadline.CancelAfter(timeout > TimeSpan.Zero
            ? timeout
            : TimeSpan.FromSeconds(30));
        try
        {
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare),
                    deadline.Token)
                .ConfigureAwait(false);
            return await pending.Ready.Task.WaitAsync(deadline.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (!cancellationToken.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "The canonical relocation target did not become ready.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }
        finally
        {
            if (pending.ReleaseWaiter())
                _pendingRelocationPrepares.TryRemove(
                    new KeyValuePair<PendingRelocationPrepareKey,
                        PendingRelocationPrepare>(key, pending));
        }
    }

    public async ValueTask SendCanonicalRelocationDataAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data,
        CancellationToken cancellationToken)
    {
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        await SendCanonicalRelocationRecordAsync(
                peer,
                ZLinkServiceWireCodec.EncodeRelocationData(data),
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask SendCanonicalRelocationCutoverAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        CancellationToken cancellationToken)
    {
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.", ZLinkRetryAdvice.RetryAfterBackoff);
        await SendCanonicalRelocationRecordAsync(
                peer,
                ZLinkServiceWireCodec.EncodeRelocationCutover(cutover),
                cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        SealSessionRelocationAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            TimeSpan timeout,
            CancellationToken cancellationToken) =>
        RunSessionRelocationSealAsync(
            sessionOwnerNodeRid,
            seal,
            timeout,
            cancellationToken);

    private async ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        RunSessionRelocationSealAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        Peer peer = RequireSessionRelocationPeer(sessionOwnerNodeRid);
        var encoded = ZLinkServiceWireCodec.EncodeSessionRelocationSeal(seal);
        var key = PendingSessionRelocationKey.Create(sessionOwnerNodeRid, seal);
        var fingerprint = SHA256.HashData(encoded);
        var pending = new PendingSessionRelocationSeal(fingerprint, seal);
        if (!_pendingSessionRelocationSeals.TryAdd(key, pending))
        {
            if (!_pendingSessionRelocationSeals.TryGetValue(key, out pending!)
                || !CryptographicOperations.FixedTimeEquals(
                    pending.Fingerprint,
                    fingerprint))
                throw new InvalidDataException(
                    "A command 42 retry changed fields.");
        }
        try
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                _stop?.Token ?? CancellationToken.None);
            deadline.CancelAfter(timeout > TimeSpan.Zero
                ? timeout
                : TimeSpan.FromSeconds(30));
            while (true)
            {
                await SendCanonicalRelocationRecordAsync(
                        peer,
                        encoded,
                        deadline.Token)
                    .ConfigureAwait(false);
                var completed = await Task.WhenAny(
                        pending.Completion.Task,
                        Task.Delay(RelocationAckRetryInterval, deadline.Token))
                    .ConfigureAwait(false);
                if (ReferenceEquals(completed, pending.Completion.Task))
                {
                    var response = await pending.Completion.Task
                        .ConfigureAwait(false);
                    RememberSessionRelocationSealReply(key, response);
                    return response;
                }
            }
        }
        catch (OperationCanceledException)
            when (!cancellationToken.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "The session relocation seal was not acknowledged.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }
        finally
        {
            _pendingSessionRelocationSeals.TryRemove(
                new KeyValuePair<PendingSessionRelocationKey,
                    PendingSessionRelocationSeal>(key, pending));
        }
    }

    public async ValueTask RouteSessionRelocationAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
            CancellationToken cancellationToken)
    {
        var peer = RequireSessionRelocationPeer(sessionOwnerNodeRid);
        var encoded = ZLinkServiceWireCodec.EncodeSessionRelocationRoute(route);
        await SendCanonicalRelocationRecordAsync(
                peer,
                encoded,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private Peer RequireSessionRelocationPeer(RoutingId targetNodeRid)
    {
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetNodeRid, out peer);
        // Admission succeeds only after framework-service-v12 is decoded, so
        // this is also the capability preflight for commands 42-45.
        if (peer is null || !peer.Admitted || peer.Admission is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "The session relocation owner lacks framework-service-v12 capability evidence.",
                ZLinkRetryAdvice.DoNotRetry);
        return peer;
    }

    private async ValueTask SendCanonicalRelocationRecordAsync(
        Peer expectedPeer,
        byte[] encoded,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Peer? current;
        lock (_gate)
            _peersByRid.TryGetValue(expectedPeer.RoutingId, out current);
        if (!ReferenceEquals(current, expectedPeer) || !expectedPeer.Admitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation connection changed.",
                ZLinkRetryAdvice.RetryAfterBackoff);

        try
        {
            await SendRoutedAsync(
                    expectedPeer.PhysicalRoutingId,
                    [encoded],
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkException exception)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation record could not be submitted.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                exception);
        }
    }

    public SubmitResult ActivateInstanceSpot(
        InstanceSpotActivationTarget target,
        string sourceSpotId,
        IReadOnlyList<Message> parts,
        bool request,
        out MeshOperationId operationId,
        ulong deadlineUnixMs,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException(
                "The first Instance Spot message is required.",
                nameof(parts));
        if (target.TargetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 39 cold activation is reserved for a remote target.",
                nameof(target));

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(target.TargetNodeRid, out peer);
        if (peer is null
            || !peer.Admitted
            || peer.LifecycleGeneration != target.TargetNodeGeneration)
        {
            operationId = default;
            return SubmitResult.NotConnected;
        }

        PendingOperation? pending = null;
        ulong replyRouteId = 0;
        var effectiveTimeout = timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30)
            : timeout;
        if (request)
        {
            if (!TryCreateOperation(
                    MeshOperationKind.InstanceSpotRequest,
                    out replyRouteId,
                    out pending))
            {
                operationId = default;
                Publish(
                    MeshMonitorEventKind.Backpressured,
                    peerRid: target.TargetNodeRid);
                return SubmitResult.Backpressured;
            }
            operationId = pending.OperationId;
            var timeoutDeadline = checked((ulong)DateTimeOffset.UtcNow
                .Add(effectiveTimeout)
                .ToUnixTimeMilliseconds());
            pending.DeadlineUnixMs = Math.Min(deadlineUnixMs, timeoutDeadline);
        }
        else
        {
            operationId = NextStandaloneOperationId();
        }

        var operation = new InstanceSpotActivationOperation(
            target,
            _routingId,
            _lifecycleGeneration,
            sourceSpotId,
            operationId,
            request,
            replyRouteId,
            deadlineUnixMs);
        var head = ZLinkServiceWireCodec.EncodeInstanceSpotActivation(
            operation,
            !metadata.IsEmpty);
        var submit = pending is null
            ? SubmitInstanceSpotSend(peer, head, parts, flags, metadata)
            : SubmitNativeServiceRequest(
                peer,
                head,
                parts,
                flags,
                metadata,
                pending);
        if (submit != SubmitResult.Ok)
        {
            if (pending is not null)
            {
                TryRemoveOperation(replyRouteId, out _);
                pending.Cancel();
            }
            operationId = default;
            return submit;
        }

        return SubmitResult.Ok;
    }

    internal SubmitResult ActivateInstanceSpot(
        InstanceSpotActivationTarget target,
        string sourceSpotId,
        IReadOnlyList<Message> parts,
        MeshOperationId correlationId,
        ulong deadlineUnixMs,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException(
                "The first Instance Spot message is required.",
                nameof(parts));
        if (target.TargetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 39 cold activation is reserved for a remote target.",
                nameof(target));

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(target.TargetNodeRid, out peer);
        if (peer is null
            || !peer.Admitted
            || peer.LifecycleGeneration != target.TargetNodeGeneration)
            return SubmitResult.NotConnected;

        if (!TryCreateOperation(
                MeshOperationKind.InstanceSpotRequest,
                correlationId,
                out var replyRouteId,
                out var pending))
        {
            Publish(
                MeshMonitorEventKind.Backpressured,
                peerRid: target.TargetNodeRid);
            return SubmitResult.Backpressured;
        }
        var effectiveTimeout = timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30)
            : timeout;
        var timeoutDeadline = checked((ulong)DateTimeOffset.UtcNow
            .Add(effectiveTimeout)
            .ToUnixTimeMilliseconds());
        pending.DeadlineUnixMs = Math.Min(deadlineUnixMs, timeoutDeadline);

        var operation = new InstanceSpotActivationOperation(
            target,
            _routingId,
            _lifecycleGeneration,
            sourceSpotId,
            correlationId,
            true,
            replyRouteId,
            deadlineUnixMs);
        var head = ZLinkServiceWireCodec.EncodeInstanceSpotActivation(
            operation,
            !metadata.IsEmpty);
        var submit = SubmitNativeServiceRequest(
            peer,
            head,
            parts,
            flags,
            metadata,
            pending);
        if (submit != SubmitResult.Ok)
        {
            TryRemoveOperation(replyRouteId, out _);
            pending.Cancel();
        }
        return submit;
    }

    internal async ValueTask<InstanceSpotActivationTerminal>
        ForwardInstanceSpotActivationAsync(
        InstanceSpotActivationOperation operation,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ReadOnlyMemory<byte>? metadata,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException(
                "The first Instance Spot message is required.",
                nameof(parts));
        if (operation.Target.TargetNodeRid == _routingId)
            throw new ArgumentException(
                "An Instance Spot activation cannot be forwarded to the current node.",
                nameof(operation));

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(operation.Target.TargetNodeRid, out peer);
        if (peer is null
            || !peer.Admitted
            || peer.LifecycleGeneration != operation.Target.TargetNodeGeneration)
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.NotConnected);

        var head = ZLinkServiceWireCodec.EncodeInstanceSpotActivation(
            operation,
            metadata.HasValue && !metadata.Value.IsEmpty);
        if (!operation.IsRequest)
        {
            var messageParts = parts.Select(Message.From).ToArray();
            try
            {
                var submit = SubmitInstanceSpotSend(
                    peer,
                    head,
                    messageParts,
                    SendFlags.DontWait,
                    metadata ?? ReadOnlyMemory<byte>.Empty);
                if (submit != SubmitResult.Ok)
                    throw new ZlinkSubmitException(
                        (ZlinkSubmitException.ErrorCode)(int)submit);
                return new InstanceSpotActivationTerminal(
                    RequestResult.Ok,
                    ServiceWireConstants.FrameworkErrorCode.None,
                    Array.Empty<ReadOnlyMemory<byte>>(),
                    Forwarded: true);
            }
            finally
            {
                DisposeParts(messageParts);
            }
        }

        return await SubmitForwardedInstanceSpotRequestAsync(
                peer,
                operation,
                head,
                parts,
                metadata,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private SubmitResult SubmitInstanceSpotSend(
        Peer peer,
        byte[] head,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        var wireParts = new List<ReadOnlyMemory<byte>>(3) { head };
        if (!metadata.IsEmpty)
            wireParts.Add(metadata);
        wireParts.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        if (!TryScheduleRoutedSend(peer.PhysicalRoutingId, wireParts))
        {
            Publish(MeshMonitorEventKind.Backpressured, peerRid: peer.RoutingId);
            return SubmitResult.Backpressured;
        }
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: peer.RoutingId);
        return SubmitResult.Ok;
    }

    public SubmitResult CreateUserSpot(
        RoutingId targetNodeRid,
        string spotId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 47 is reserved for a remote User Spot target.",
                nameof(targetNodeRid));
        if (reservation.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The reservation target must match the command target.",
                nameof(reservation));
        return SubmitInfrastructureOperation(
            targetNodeRid,
            reservation.TargetNodeGeneration,
            MeshOperationKind.UserSpotCreate,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeUserSpotCreate(
                new UserSpotCreateOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    spotId,
                    stableType,
                    reservation,
                    deadlineUnixMs)),
            out operationId,
            timeout);
    }

    internal SubmitResult CreateUserSpot(
        RoutingId targetNodeRid,
        string spotId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 47 is reserved for a remote User Spot target.",
                nameof(targetNodeRid));
        if (reservation.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The reservation target must match the command target.",
                nameof(reservation));
        var result = SubmitInfrastructureOperation(
            targetNodeRid,
            reservation.TargetNodeGeneration,
            MeshOperationKind.UserSpotCreate,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeUserSpotCreate(
                new UserSpotCreateOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    spotId,
                    stableType,
                    reservation,
                    deadlineUnixMs)),
            out _,
            timeout,
            correlationId);
        return result;
    }

    public SubmitResult CloseUserSpot(
        RoutingId targetNodeRid,
        UserSpotCloseFence target,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 48 is reserved for a remote User Spot owner.",
                nameof(targetNodeRid));
        if (target.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The close fence target must match the command target.",
                nameof(target));
        return SubmitInfrastructureOperation(
            targetNodeRid,
            target.TargetNodeGeneration,
            MeshOperationKind.UserSpotClose,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeUserSpotClose(
                new UserSpotCloseOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    target,
                    deadlineUnixMs)),
            out operationId,
            timeout);
    }

    internal SubmitResult CloseUserSpot(
        RoutingId targetNodeRid,
        UserSpotCloseFence target,
        ulong deadlineUnixMs,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 48 is reserved for a remote User Spot owner.",
                nameof(targetNodeRid));
        if (target.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The close fence target must match the command target.",
                nameof(target));
        return SubmitInfrastructureOperation(
            targetNodeRid,
            target.TargetNodeGeneration,
            MeshOperationKind.UserSpotClose,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeUserSpotClose(
                new UserSpotCloseOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    target,
                    deadlineUnixMs)),
            out _,
            timeout,
            correlationId);
    }

    public SubmitResult CreateActorRemote(
        RoutingId targetNodeRid,
        string actorId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 49 is reserved for a remote Actor target.",
                nameof(targetNodeRid));
        if (reservation.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The reservation target must match the command target.",
                nameof(reservation));
        return SubmitInfrastructureOperation(
            targetNodeRid,
            reservation.TargetNodeGeneration,
            MeshOperationKind.ActorCreate,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeActorCreate(
                new ActorCreateOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    actorId,
                    stableType,
                    reservation,
                    deadlineUnixMs)),
            out operationId,
            timeout);
    }

    internal SubmitResult CreateActorRemote(
        RoutingId targetNodeRid,
        string actorId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (targetNodeRid == _routingId)
            throw new ArgumentException(
                "Command 49 is reserved for a remote Actor target.",
                nameof(targetNodeRid));
        if (reservation.TargetNodeRid != targetNodeRid)
            throw new ArgumentException(
                "The reservation target must match the command target.",
                nameof(reservation));
        return SubmitInfrastructureOperation(
            targetNodeRid,
            reservation.TargetNodeGeneration,
            MeshOperationKind.ActorCreate,
            (correlation, operation) => ZLinkServiceWireCodec.EncodeActorCreate(
                new ActorCreateOperation(
                    correlation,
                    operation,
                    _routingId,
                    _lifecycleGeneration,
                    actorId,
                    stableType,
                    reservation,
                    deadlineUnixMs)),
            out _,
            timeout,
            correlationId);
    }

    public SubmitResult DestroyActorRemote(
        ActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        if (actor.NodeRid == _routingId)
            throw new ArgumentException(
                "Command 27 is reserved for a remote Actor owner.",
                nameof(actor));
        return SubmitInfrastructureOperation(
            actor.NodeRid,
            targetNodeGeneration,
            MeshOperationKind.ActorDestroy,
            (correlation, _) => ZLinkServiceWireCodec.EncodeActorDestroy(
                new ActorDestroyOperation(
                    correlation,
                    actor,
                    actor.NodeRid,
                    targetNodeGeneration,
                    authorityOwnerGeneration)),
            out operationId,
            timeout);
    }

    internal SubmitResult DestroyActorRemote(
        ActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (actor.NodeRid == _routingId)
            throw new ArgumentException(
                "Command 27 is reserved for a remote Actor owner.",
                nameof(actor));
        return SubmitInfrastructureOperation(
            actor.NodeRid,
            targetNodeGeneration,
            MeshOperationKind.ActorDestroy,
            (correlation, _) => ZLinkServiceWireCodec.EncodeActorDestroy(
                new ActorDestroyOperation(
                    correlation,
                    actor,
                    actor.NodeRid,
                    targetNodeGeneration,
                    authorityOwnerGeneration)),
            out _,
            timeout,
            correlationId);
    }

    internal SubmitResult ResubmitUserSpotOperation(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.UserSpotOperationRecord operation)
    {
        var sourceNodeRid = operation.Command
            == ServiceWireConstants.Command.UserSpotCreate
                ? operation.Create.SourceNodeRid
                : operation.Close.SourceNodeRid;
        var sourceNodeGeneration = operation.Command
            == ServiceWireConstants.Command.UserSpotCreate
                ? operation.Create.SourceNodeGeneration
                : operation.Close.SourceNodeGeneration;
        if (sourceNodeRid != _routingId
            || sourceNodeGeneration != _lifecycleGeneration)
            throw new ArgumentException(
                "The operation source must match this MeshNode.",
                nameof(operation));
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            return SubmitResult.NotConnected;
        var head = operation.Command
            == ServiceWireConstants.Command.UserSpotCreate
                ? ZLinkServiceWireCodec.EncodeUserSpotCreate(operation.Create)
                : ZLinkServiceWireCodec.EncodeUserSpotClose(operation.Close);
        return TryScheduleRoutedSend(peer.PhysicalRoutingId, [head])
            ? SubmitResult.Ok
            : SubmitResult.Backpressured;
    }

    internal int RetainedUserSpotOperationCount =>
        _remoteUserSpotOperations.Count;

    internal SubmitResult ResubmitActorCreateOperation(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.ActorCreateOperationRecord record)
    {
        var operation = record.Operation;
        if (operation.SourceNodeRid != _routingId
            || operation.SourceNodeGeneration != _lifecycleGeneration)
            throw new ArgumentException(
                "The operation source must match this MeshNode.",
                nameof(record));
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            return SubmitResult.NotConnected;
        var head = ZLinkServiceWireCodec.EncodeActorCreate(operation);
        return TryScheduleRoutedSend(peer.PhysicalRoutingId, [head])
            ? SubmitResult.Ok
            : SubmitResult.Backpressured;
    }

    internal int PendingCanonicalRelocationPrepareCount =>
        _pendingRelocationPrepares.Count;

    public MeshNodeStatus Status()
    {
        lock (_gate)
        {
            var admitted = _peersByRid.Values.Count(static peer => peer.Admitted);
            var draining = _peersByRid.Values.Count(
                static peer => peer.State == MeshPeerState.Draining);
            var pendingApplication = _ownedMailboxes
                .Where(static entry =>
                    entry.Key.Domain == MeshReadyDomains.Application)
                .Sum(static entry => entry.Value.Count);
            var pendingInfrastructure = _ownedMailboxes
                .Where(static entry =>
                    entry.Key.Domain == MeshReadyDomains.Infrastructure)
                .Sum(static entry => entry.Value.Count);
            var pendingBytes = checked(
                (ulong)Math.Max(0, Volatile.Read(ref _queuedBytes)));
            return new MeshNodeStatus(
                _state,
                _routingId,
                _meshName,
                _bindEndpoint,
                _lifecycleGeneration,
                _descriptorRevision,
                checked((uint)_channels.Count),
                checked((uint)_peersByIntent.Count),
                checked((uint)admitted),
                checked((uint)draining),
                checked((ulong)pendingApplication),
                checked((ulong)pendingInfrastructure),
                pendingBytes,
                0,
                checked((ulong)Environment.TickCount64));
        }
    }

    public MeshNodePeer[] Peers()
    {
        lock (_gate)
            return _peersByIntent.Values
                .Select(static peer => peer.Snapshot())
                .ToArray();
    }

    public MeshPeerChannel[] PeerChannels(
        RoutingId peerRid,
        ulong lifecycleGeneration)
    {
        lock (_gate)
        {
            if (!_peersByRid.TryGetValue(peerRid, out var peer)
                || peer.LifecycleGeneration != lifecycleGeneration)
                return Array.Empty<MeshPeerChannel>();
            return peer.Channels.Select(static channel =>
                    new MeshPeerChannel(channel.Key, channel.Value))
                .ToArray();
        }
    }

    public IMeshNodeMonitor OpenMonitor(
        MeshMonitorEventMask events = MeshMonitorEventMask.All)
    {
        var monitor = new RawMeshMonitor(events);
        lock (_gate)
            _monitors.Add(monitor);
        return monitor;
    }

    public void SetReadyHandler(Func<MeshReadyDomains, MeshReadyDomains> handler)
    {
        _readyHandler = handler ?? throw new ArgumentNullException(nameof(handler));
        // Records may be queued while the node starts, before the framework
        // installs its pull-dispatch pump. Such an enqueue marks ready as
        // posted even though no callback existed. Re-arm the edge when the
        // handler is installed so those records and all later completions can
        // be drained.
        Volatile.Write(ref _readyPosted, 0);
        SignalReadyIfNeeded();
    }

    void IMeshNode.SetCompletionOverflowHandler(
        Action<MeshReceiveRecord, IReadOnlyList<Message>> handler) =>
        SetCompletionOverflowHandlerCore(handler);

    internal void SetCompletionOverflowHandlerCore(
        Action<MeshReceiveRecord, IReadOnlyList<Message>> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        Volatile.Write(ref _completionOverflowHandler, handler);
    }

    public bool DrainReady(
        MeshReadyDomains domains,
        MeshReadyBatch batch,
        RecvFlags flags = RecvFlags.None)
    {
        ArgumentNullException.ThrowIfNull(batch);
        if ((domains & MeshReadyDomains.All) == 0)
            return false;

        lock (_readyGate)
        {
            Volatile.Write(ref _readyPosted, 0);
            foreach (var entry in _ownedMailboxes
                         .Where(entry => (entry.Key.Domain & domains) != 0)
                         .OrderBy(static entry =>
                             entry.Key.Domain == MeshReadyDomains.Infrastructure ? 0 : 1)
                         .ThenBy(static entry => entry.Key.OwnerKind)
                         .ThenBy(static entry => entry.Key.Identity, StringComparer.Ordinal))
            {
                if (!entry.Value.TryClaim())
                    continue;
                var mailbox = entry.Value;
                batch.Add(
                    new MeshReadyRecord(
                        entry.Key.OwnerKind,
                        entry.Key.Domain,
                        entry.Key.SpotId,
                        entry.Key.Actor),
                    new MeshClaim
                    {
                        Receiver = (receiveBatch, receiveFlags) =>
                            DrainOwnedQueue(mailbox, receiveBatch, receiveFlags),
                        Releaser = () => ReleaseOwnedMailbox(mailbox)
                    });
            }
            return false;
        }
    }

    public ISpot CreateSpot()
    {
        var spotId = Guid.NewGuid().ToString("D");
        var spotKey = ZLinkSpotId.FromBoundary(spotId, nameof(spotId));
        return _spots.GetOrAdd(
            spotKey,
            value => new ZLinkManagedSpot(
                this,
                value.Value,
                Interlocked.Increment(ref _nextSpotGeneration),
                NextAuthorityOwnerGeneration()));
    }

    public ISpot EntrySpot()
    {
        if (_routingId.IsEmpty)
            throw new InvalidOperationException(
                "The MeshNode routing id must be configured before its Entry Spot is used.");
        lock (_entrySpotGate)
        {
            // The backend assigns the public Entry Spot id by rekeying this
            // object after the node-rid placeholder has been created. Keep the
            // object reference so later actor creation and lookup use the same
            // logical owner instead of recreating the placeholder under the
            // node routing id.
            var entrySpotId = _routingId.ToString();
            var entrySpotKey = ZLinkSpotId.FromBoundary(
                entrySpotId,
                nameof(entrySpotId));
            return _entrySpot ??= _spots.GetOrAdd(
                entrySpotKey,
                value => new ZLinkManagedSpot(
                    this,
                    value.Value,
                    _lifecycleGeneration,
                    _lifecycleGeneration));
        }
    }

    public ISpot GetOrCreateSpot(string spotId, out bool created)
    {
        var spotKey = ZLinkSpotId.FromBoundary(spotId, nameof(spotId));
        if (_spots.TryGetValue(spotKey, out var existing))
        {
            created = false;
            return existing;
        }

        var candidate = new ZLinkManagedSpot(
            this,
            spotId,
            Interlocked.Increment(ref _nextSpotGeneration),
            NextAuthorityOwnerGeneration());
        var spot = _spots.GetOrAdd(spotKey, candidate);
        created = ReferenceEquals(spot, candidate);
        if (!created)
            candidate.Dispose();
        return spot;
    }

    public ISpot GetOrCreateReservedSpot(
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        out bool created)
    {
        var spotKey = ZLinkSpotId.FromBoundary(spotId, nameof(spotId));
        if (objectGeneration == 0 || objectGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(objectGeneration));
        if (authorityOwnerGeneration == 0 || authorityOwnerGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(authorityOwnerGeneration));
        if (_spots.TryGetValue(spotKey, out var existing))
        {
            created = false;
            return existing;
        }

        ulong observed;
        do
        {
            observed = Volatile.Read(ref _nextSpotGeneration);
            if (observed >= objectGeneration) break;
        } while (Interlocked.CompareExchange(
                     ref _nextSpotGeneration,
                     objectGeneration,
                     observed) != observed);

        var candidate = new ZLinkManagedSpot(
            this,
            spotId,
            objectGeneration,
            authorityOwnerGeneration);
        var spot = _spots.GetOrAdd(spotKey, candidate);
        created = ReferenceEquals(spot, candidate);
        if (!created)
            candidate.Dispose();
        return spot;
    }

    public ActorRef CreateActor(
        string actorId,
        IReadOnlyList<Message>? creationParts = null,
        TimeSpan timeout = default)
    {
        var generation = Interlocked.Increment(ref _nextActorGeneration);
        return CreateActorCore(
            actorId,
            checked((ulong)generation),
            creationParts,
            timeout);
    }

    public ActorRef CreateReservedActor(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        IReadOnlyList<Message>? creationParts = null,
        TimeSpan timeout = default)
    {
        if (objectGeneration == 0 || objectGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(objectGeneration));
        if (authorityOwnerGeneration == 0 || authorityOwnerGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(authorityOwnerGeneration));
        ulong observed;
        do
        {
            observed = Volatile.Read(ref _nextActorGeneration);
            if (observed >= objectGeneration) break;
        } while (Interlocked.CompareExchange(
                     ref _nextActorGeneration,
                     objectGeneration,
                     observed) != observed);
        return CreateActorCore(
            actorId,
            objectGeneration,
            creationParts,
            timeout,
            authorityOwnerGeneration);
    }

    private ActorRef CreateActorCore(
        string actorId,
        ulong generation,
        IReadOnlyList<Message>? creationParts,
        TimeSpan timeout,
        ulong? reservedAuthorityOwnerGeneration = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (Encoding.UTF8.GetByteCount(actorId) > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(actorId));
        ThrowIfDisposed();

        var entry = (ZLinkManagedSpot)EntrySpot();
        if (generation == 0 || generation > long.MaxValue)
            throw new InvalidOperationException("The Actor generation space was exhausted.");
        var actorRef = new ActorRef(
            actorId,
            generation,
            _meshName,
            _routingId);
        var actor = new ManagedActor(
            actorRef,
            entry.SpotId,
            entry.LifecycleGeneration,
            membershipEpoch: 1,
            reservedAuthorityOwnerGeneration ?? NextAuthorityOwnerGeneration());
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        if (!_actors.TryAdd(actorKey, actor))
            throw new InvalidOperationException($"Actor '{actorId}' already exists.");
        entry.AddActor();
        EnqueueActorLifecycle(
            entry,
            ActorLifecycleKind.Created,
            actor,
            actor,
            creationParts ?? Array.Empty<Message>());
        return actorRef;
    }

    public void SetActorAuthority(
        ActorRef actor,
        ulong authorityOwnerGeneration)
    {
        if (authorityOwnerGeneration == 0
            || !TryGetActor(actor, out var current))
            throw new InvalidOperationException(
                $"Actor '{actor.ActorId}' does not match the local authority update.");
        current.SetAuthorityOwnerGeneration(authorityOwnerGeneration);
    }

    public bool ActorLookup(string actorId, out ActorLocation location)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        if (_actors.TryGetValue(actorKey, out var actor)
            && !actor.Draining)
        {
            location = actor.Location;
            return true;
        }
        location = default!;
        return false;
    }

    internal void ObserveSpotAuthority(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong objectGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        ValidateObservedAuthority(
            targetNodeRid,
            objectGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        if (targetNodeGeneration == 0)
            throw new ArgumentOutOfRangeException(
                nameof(targetNodeGeneration),
                "The target node lifecycle generation must be non-zero.");
        if (string.IsNullOrEmpty(targetSpotId))
            throw new ArgumentException(
                "The observed Spot routing id is required.",
                nameof(targetSpotId));
        _observedSpotAuthorities[
            new ObservedSpotAuthorityKey(
                targetNodeRid,
                targetSpotId,
                objectGeneration)] = new ObservedAuthority(
                    targetNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration);
    }

    internal void ObserveActorAuthority(
        ActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        ValidateObservedAuthority(
            actor.NodeRid,
            actor.ObjectGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        ArgumentException.ThrowIfNullOrWhiteSpace(actor.ActorId);
        if (targetNodeGeneration == 0)
            throw new ArgumentOutOfRangeException(
                nameof(targetNodeGeneration),
                "The target node lifecycle generation must be non-zero.");
        _observedActorAuthorities[
            new ObservedActorAuthorityKey(
                actor.NodeRid,
                actor.ActorId,
                actor.ObjectGeneration)] = new ObservedAuthority(
                    targetNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration);
    }

    internal void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration)
    {
        if (ownerLeaseGeneration is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(ownerLeaseGeneration));
        Volatile.Write(
            ref _localOwnerLeaseGeneration,
            checked((long)ownerLeaseGeneration));
    }

    internal void SetLocalRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source)
    {
        if (source.NodeRid != _routingId
            || source.NodeGeneration != _lifecycleGeneration
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(source));
        lock (_operationGate)
            _localRequestSourceFence = source;
    }

    internal bool TryGetActorAuthority(
        ActorRef actor,
        out ulong authorityOwnerGeneration,
        out ulong ownerLeaseGeneration)
    {
        if (TryGetActor(actor, out var current))
        {
            authorityOwnerGeneration = current.AuthorityOwnerGeneration;
            ownerLeaseGeneration = checked(
                (ulong)Volatile.Read(ref _localOwnerLeaseGeneration));
            return true;
        }
        authorityOwnerGeneration = 0;
        ownerLeaseGeneration = 0;
        return false;
    }

    public MeshOperationId DestroyActor(ActorRef actor, TimeSpan timeout = default)
    {
        var operationId = AllocateOperationId();
        DestroyActor(actor, operationId, timeout);
        return operationId;
    }

    internal void DestroyActor(
        ActorRef actor,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (!TryBeginOperation(
                MeshOperationKind.ActorDestroy,
                correlationId,
                timeout,
                out var operation))
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Backpressured);
        var actorKey = ZLinkActorId.FromBoundary(actor.ActorId, nameof(actor.ActorId));
        if (!_actors.TryGetValue(actorKey, out var current)
            || current.Ref.ObjectGeneration != actor.ObjectGeneration
            || current.Ref.NodeRid != actor.NodeRid
            || !current.TryDrain())
        {
            CompleteManagedOperation(
                operation,
                RequestResult.Conflict,
                current is null ? 2 : 1,
                Array.Empty<Message>());
            return;
        }

        _actors.TryRemove(new KeyValuePair<ZLinkActorId, ManagedActor>(actorKey, current));
        if (_spots.TryGetValue(
                ZLinkSpotId.FromBoundary(current.SpotId, nameof(current.SpotId)),
                out var spot))
        {
            spot.RemoveActor();
            EnqueueActorLifecycle(
                spot,
                ActorLifecycleKind.Destroyed,
                current,
                current,
                Array.Empty<Message>());
        }
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
    }

    public MeshOperationId JoinSpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message>? creationParts = null,
        TimeSpan timeout = default) =>
        BeginJoin(
            actor,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            entry: false,
            creationParts,
            timeout);

    internal void JoinSpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message>? creationParts,
        MeshOperationId correlationId,
        TimeSpan timeout) =>
        BeginJoin(
            actor,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            entry: false,
            creationParts,
            timeout,
            correlationId);

    public MeshOperationId JoinEntrySpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        IReadOnlyList<Message>? creationParts = null,
        TimeSpan timeout = default) =>
        BeginJoin(
            actor,
            targetNodeRid,
            targetNodeRid == _routingId
                ? ((ZLinkManagedSpot)EntrySpot()).SpotId
                : throw new InvalidOperationException(
                    "Remote Entry Spot joins require the descriptor EntrySpotId mapping."),
            0,
            entry: true,
            creationParts,
            timeout);

    internal void JoinEntrySpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        IReadOnlyList<Message>? creationParts,
        MeshOperationId correlationId,
        TimeSpan timeout) =>
        BeginJoin(
            actor,
            targetNodeRid,
            targetNodeRid == _routingId
                ? ((ZLinkManagedSpot)EntrySpot()).SpotId
                : throw new InvalidOperationException(
                    "Remote Entry Spot joins require the descriptor EntrySpotId mapping."),
            0,
            entry: true,
            creationParts,
            timeout,
            correlationId);

    public SubmitResult SendToNode(
        RoutingId targetRid,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        SubmitApplication(
            targetRid,
            ServiceWireConstants.Command.NodeSend,
            0,
            null,
            parts,
            flags,
            metadata);

    public SubmitResult RequestToNode(
        RoutingId targetRid,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        SubmitRequest(
            targetRid,
            ServiceWireConstants.Command.NodeRequest,
            null,
            parts,
            timeout,
            flags,
            metadata,
            out operationId);

    public SubmitResult RequestToNode(
        RoutingId targetRid,
        IReadOnlyList<Message> parts,
        ZLinkBackendRequestCallback callback,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default)
    {
        ArgumentNullException.ThrowIfNull(callback);
        var submit = SubmitRequest(
            targetRid,
            ServiceWireConstants.Command.NodeRequest,
            null,
            parts,
            timeout,
            flags,
            metadata,
            out _,
            awaitCompletion: true,
            out var completion);
        if (submit == SubmitResult.Ok)
            _ = DeliverRequestCompletionAsync(completion!, callback);
        return submit;
    }

    internal SubmitResult SendToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TrySelectChannelTarget(channelName, out var targetRid))
        {
            return ChannelSelectionFailureResult(channelName);
        }
        return SubmitApplication(
            targetRid,
            ServiceWireConstants.Command.ChannelSend,
            0,
            channelName,
            parts,
            flags,
            metadata);
    }

    internal SubmitResult RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TrySelectChannelTarget(channelName, out var targetRid))
        {
            operationId = default;
            return ChannelSelectionFailureResult(channelName);
        }
        return SubmitRequest(
            targetRid,
            ServiceWireConstants.Command.ChannelRequest,
            channelName,
            parts,
            timeout,
            flags,
            metadata,
            out operationId);
    }

    internal SubmitResult RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        ZLinkBackendRequestCallback callback,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        ArgumentNullException.ThrowIfNull(callback);
        if (!TrySelectChannelTarget(channelName, out var targetRid))
        {
            return ChannelSelectionFailureResult(channelName);
        }
        var submit = SubmitRequest(
            targetRid,
            ServiceWireConstants.Command.ChannelRequest,
            channelName,
            parts,
            timeout,
            flags,
            metadata,
            out _,
            awaitCompletion: true,
            out var completion);
        if (submit == SubmitResult.Ok)
            _ = DeliverRequestCompletionAsync(completion!, callback);
        return submit;
    }

    internal void Publish(
        string sourceSpotId,
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        var localTargets = _spots.Values
            .Where(spot => spot.Matches(channelName, topic))
            .OrderBy(static spot => spot.RoutingId.ToHex(), StringComparer.Ordinal)
            .ToArray();
        foreach (var spot in localTargets)
        {
            var retained = CloneParts(parts);
            EnqueueOwned(
                MailboxKey.ForSpot(spot, MeshReadyDomains.Application),
                new MeshReceiveRecord(
                    MeshRecordKind.SpotMulticast,
                    MeshReadyDomains.Application,
                    _routingId,
                    sourceSpotId,
                    _lifecycleGeneration,
                    default,
                    default,
                    default,
                    channelName,
                    topic,
                    metadata.IsEmpty ? null : metadata.ToArray(),
                    0,
                    retained.Count,
                    0,
                    0,
                    null),
                retained);
        }

        IReadOnlyList<ZLinkMeshChannelTarget> targets;
        lock (_gate)
            targets = _channelSelection.Candidates(channelName);
        foreach (var target in targets)
        {
            if (target.RoutingId == _routingId)
                continue;
            Peer? peer;
            lock (_gate)
                _peersByRid.TryGetValue(target.RoutingId, out peer);
            if (peer is null || !peer.Admitted)
                continue;

            var head = ZLinkServiceWireCodec.EncodeLogicalMulticast(
                channelName,
                topic,
                sourceSpotId,
                !metadata.IsEmpty);
            var wireParts = new List<ReadOnlyMemory<byte>>(3)
            {
                head
            };
            if (!metadata.IsEmpty)
                wireParts.Add(metadata);
            wireParts.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
            _ = TryScheduleRoutedSend(peer.PhysicalRoutingId, wireParts);
        }
    }

    public SubmitResult SendToActor(
        ActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None) =>
        SubmitActor(actor, parts, request: false, default, flags, out _);

    public SubmitResult RequestToActor(
        ActorRef actor,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        if (!TryBeginOperation(
                MeshOperationKind.ActorRequest,
                timeout,
                out var operation))
        {
            operationId = default;
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        operationId = operation.OperationId;
        var result = SubmitActor(
            actor,
            parts,
            request: true,
            operation,
            SendFlags.None,
            out _);
        if (result != SubmitResult.Ok)
        {
            RemoveManagedOperation(operation);
            operationId = default;
        }
        return result;
    }

    internal SubmitResult RequestToActor(
        ActorRef actor,
        IReadOnlyList<Message> parts,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        if (!TryBeginOperation(
                MeshOperationKind.ActorRequest,
                correlationId,
                timeout,
                out var operation))
        {
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        var result = SubmitActor(
            actor,
            parts,
            request: true,
            operation,
            SendFlags.None,
            out _);
        if (result != SubmitResult.Ok)
            RemoveManagedOperation(operation);
        return result;
    }

    public SubmitResult SendBoundSession(
        ActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        if (!TryGetActor(actor, out var current))
            return SubmitResult.NotFound;
        var binding = current.Binding;
        if (binding is null)
            return SubmitResult.NotConnected;
        return binding.Service.SendToSession(binding.SessionRid, parts, flags);
    }

    internal ValueTask SendBoundSessionAsync(
        ActorRef actor,
        ulong expectedBindingGeneration,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        if (!TryGetActor(actor, out var current))
            return ValueTask.FromException(new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.NotFound));
        var binding = current.Binding;
        if (binding is null)
            return ValueTask.FromException(new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.NotConnected));
        if (binding.Generation != expectedBindingGeneration)
            return ValueTask.FromException(new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.InvalidState));
        return binding.Service.SendToSessionAsync(
            binding.SessionRid,
            parts,
            cancellationToken);
    }

    public MeshOperationId CloseBoundSession(
        ActorRef actor,
        ulong expectedBindingGeneration,
        TimeSpan timeout = default)
    {
        var operation = BeginOperation(MeshOperationKind.StreamUnbind, timeout);
        if (!TryGetActor(actor, out var current)
            || !current.TryClearBinding(expectedBindingGeneration))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.NotFound,
                0,
                Array.Empty<Message>());
            return operation.OperationId;
        }
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
        return operation.OperationId;
    }

    public IStreamSessionService CreateStreamSessionService(IStreamSocket stream) =>
        new ZLinkManagedStreamSessionService(this, stream);

    public void Dispose()
    {
        DisposeAsync().AsTask().GetAwaiter().GetResult();
    }

    public async ValueTask DisposeAsync()
    {
        Task disposal;
        lock (_disposeGate)
            disposal = _disposeTask ??= DisposeWithDefaultBoundAsync();
        await disposal.ConfigureAwait(false);
    }

    public async ValueTask ForceStopAsync(CancellationToken cancellationToken)
    {
        Task disposal;
        lock (_disposeGate)
            disposal = _disposeTask ??= cancellationToken.CanBeCanceled
                ? DisposeCoreAsync(cancellationToken)
                : DisposeWithDefaultBoundAsync();
        await disposal.ConfigureAwait(false);
    }

    private async Task DisposeWithDefaultBoundAsync()
    {
        using var shutdownBound =
            new CancellationTokenSource(_inboundOperationShutdownTimeout);
        await DisposeCoreAsync(shutdownBound.Token).ConfigureAwait(false);
    }

    private async Task DisposeCoreAsync(CancellationToken shutdownToken)
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        Task? receiveLoop;
        lock (_gate)
        {
            _state = MeshNodeState.Stopped;
            _stop?.Cancel();
            receiveLoop = _receiveLoop;
        }
        if (receiveLoop is not null)
            try
            {
                await receiveLoop.WaitAsync(shutdownToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (shutdownToken.IsCancellationRequested)
            {
                // Force-stop must continue to release the socket and poller
                // even when the receive loop does not observe cancellation.
            }

        // The native poller wait is bounded by PollInterval, but the caller's
        // shutdown token can be shorter than that wait. Give the loop one
        // final bounded window to leave the poller before its owner is closed;
        // otherwise poller destruction reports ZLINK_CLOSE_BUSY (401) even
        // though the loop is already on its way out.
        if (receiveLoop is not null && !receiveLoop.IsCompleted)
            try
            {
                await receiveLoop.WaitAsync(TransportShutdownGrace).ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                // The existing shutdown bound still controls the caller-facing
                // operation. Resource cleanup continues through the normal
                // owner disposal path below.
            }

        await CloseInboundOperationAdmissionAsync(shutdownToken).ConfigureAwait(false);

        IRouterSocket? socket;
        ISocketMonitor? socketMonitor;
        IPoller? poller;
        lock (_socketGate)
        {
            _activeSocketGeneration = 0;
            socket = _socket;
            socketMonitor = _socketMonitor;
            poller = _poller;
            _socket = null;
            _socketMonitor = null;
            _poller = null;
        }

        PendingOperation[] pendingOperations;
        lock (_operationGate)
        {
            pendingOperations = _operations.Values.ToArray();
            foreach (var pending in pendingOperations)
                RemoveRelocationReplyOperationUnderLock(
                    pending,
                    rememberTerminal: true);
            _operations.Clear();
            _relocationReplyOperations.Clear();
        }
        foreach (var pending in pendingOperations)
            pending.Cancel();
        _remoteUserSpotOperations.Clear();
        _remoteActorCreateOperations.Clear();
        foreach (var mailbox in _ownedMailboxes.Values)
            mailbox.Dispose();
        _ownedMailboxes.Clear();
        while (_transportDisconnects.TryDequeue(out _))
        {
        }
        foreach (var spot in _spots.Values)
            await spot.DisposeAsync().ConfigureAwait(false);
        _spots.Clear();

        poller?.Dispose();
        socketMonitor?.Dispose();
        socket?.Dispose();
        _stop?.Dispose();
        Publish(MeshMonitorEventKind.StateChanged);
        lock (_gate)
        {
            foreach (var monitor in _monitors)
                monitor.Dispose();
            _monitors.Clear();
        }
    }

    private bool RunInboundOperation(Func<Task> operation)
    {
        ArgumentNullException.ThrowIfNull(operation);
        Task task;
        lock (_inboundOperationGate)
        {
            if (_inboundOperationAdmissionClosed)
                return false;
            task = operation();
            _inboundOperations.Add(task);
        }

        _ = task.ContinueWith(
            static (completed, state) =>
            {
                var owner = (ZLinkManagedMeshNode)state!;
                lock (owner._inboundOperationGate)
                    owner._inboundOperations.Remove(completed);
            },
            this,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
        return true;
    }

    private async Task CloseInboundOperationAdmissionAsync(
        CancellationToken shutdownToken)
    {
        Task[] active;
        lock (_inboundOperationGate)
        {
            _inboundOperationAdmissionClosed = true;
            active = _inboundOperations.ToArray();
        }
        if (active.Length == 0) return;

        try
        {
            await Task.WhenAll(active).WaitAsync(shutdownToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (shutdownToken.IsCancellationRequested)
        {
        }
        catch
        {
            // Inbound handlers report protocol failures themselves. Disposal
            // still has to release the node-owned socket and targets after all
            // callbacks have terminated.
        }
    }

    internal SubmitResult SendToSpot(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata) =>
        SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: false,
            default,
            flags,
            metadata);

    internal SubmitResult RequestToSpot(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TryBeginOperation(
                MeshOperationKind.SpotRequest,
                timeout,
                out var operation))
        {
            operationId = default;
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        operationId = operation.OperationId;
        var result = SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: true,
            operation,
            flags,
            metadata);
        if (result != SubmitResult.Ok)
        {
            RemoveManagedOperation(operation);
            operationId = default;
        }
        return result;
    }

    internal SubmitResult RequestToSpot(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        MeshOperationId correlationId,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TryBeginOperation(
                MeshOperationKind.SpotRequest,
                correlationId,
                timeout,
                out var operation))
        {
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        var result = SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: true,
            operation,
            flags,
            metadata);
        if (result != SubmitResult.Ok)
            RemoveManagedOperation(operation);
        return result;
    }

    internal async ValueTask MessageFollowSendToSpotAsync(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException(
                "Application payload is required.", nameof(parts));

        var route = new SpotMessageFollowRoute(
            operationId,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            messageFollowHopCount,
            0);
        if (targetRid == _routingId)
        {
            var localResult = SubmitSpot(
                targetRid,
                sourceSpotId,
                spotId,
                spotGeneration,
                parts,
                request: false,
                operation: null,
                SendFlags.None,
                metadata,
                route);
            if (localResult != SubmitResult.Ok)
                throw new ZlinkSubmitException(
                    (ZlinkSubmitException.ErrorCode)(int)localResult);
            return;
        }

        var preparation = TryPrepareRemoteSpotWire(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            operationId,
            request: false,
            operation: null,
            metadata,
            route,
            out var peer,
            out var head);
        if (preparation != SubmitResult.Ok)
            throw new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)preparation);
        await SendDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateStatefulWire(head, parts, metadata),
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
    }

    internal SubmitResult MessageFollowRequestToSpot(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        ulong deadlineUnixMs,
        IReadOnlyList<Message> parts,
        out MeshOperationId transportOperationId,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TryBeginOperation(
                MeshOperationKind.SpotRequest,
                timeout,
                out var operation))
        {
            transportOperationId = default;
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        transportOperationId = operation.OperationId;
        var result = SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: true,
            operation,
            flags,
            metadata,
            new SpotMessageFollowRoute(
                operationId,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                messageFollowHopCount,
                deadlineUnixMs));
        if (result != SubmitResult.Ok)
        {
            RemoveManagedOperation(operation);
            transportOperationId = default;
        }
        return result;
    }

    internal SubmitResult MessageFollowRequestToSpot(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        ulong deadlineUnixMs,
        IReadOnlyList<Message> parts,
        MeshOperationId correlationId,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!TryBeginOperation(
                MeshOperationKind.SpotRequest,
                correlationId,
                timeout,
                out var operation))
        {
            Publish(MeshMonitorEventKind.Backpressured);
            return SubmitResult.Backpressured;
        }
        var result = SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: true,
            operation,
            flags,
            metadata,
            new SpotMessageFollowRoute(
                operationId,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                messageFollowHopCount,
                deadlineUnixMs));
        if (result != SubmitResult.Ok)
            RemoveManagedOperation(operation);
        return result;
    }

    internal void ReleaseSpot(ZLinkManagedSpot spot)
    {
        if (spot.ActorCount != 0)
            return;
        _spots.TryRemove(
            new KeyValuePair<ZLinkSpotId, ZLinkManagedSpot>(
                ZLinkSpotId.FromBoundary(spot.SpotId, nameof(spot.SpotId)),
                spot));
    }

    internal void RekeySpot(
        ZLinkManagedSpot spot,
        string previousSpotId,
        string currentSpotId)
    {
        var previousKey = ZLinkSpotId.FromBoundary(previousSpotId, nameof(previousSpotId));
        var currentKey = ZLinkSpotId.FromBoundary(currentSpotId, nameof(currentSpotId));
        if (previousKey == currentKey)
            return;
        if (!_spots.TryRemove(
                new KeyValuePair<ZLinkSpotId, ZLinkManagedSpot>(previousKey, spot))
            || !_spots.TryAdd(currentKey, spot))
            throw new InvalidOperationException(
                $"Managed Spot ID '{currentSpotId}' is already registered.");
    }

    private MeshOperationId BeginJoin(
        ActorRef actorRef,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        bool entry,
        IReadOnlyList<Message>? requestParts,
        TimeSpan timeout,
        MeshOperationId correlationId = default)
    {
        var operation = correlationId == default
            ? BeginOperation(MeshOperationKind.ActorJoin, timeout)
            : TryBeginOperation(
                MeshOperationKind.ActorJoin,
                correlationId,
                timeout,
                out var pending)
                ? pending
                : throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Backpressured);
        if (targetNodeRid != _routingId
            || !TryGetActor(actorRef, out var actor)
            || !_spots.TryGetValue(
                ZLinkSpotId.FromBoundary(targetSpotId, nameof(targetSpotId)),
                out var targetSpot)
            || (targetSpotGeneration != 0
                && targetSpot.LifecycleGeneration != targetSpotGeneration))
        {
            CompleteManagedOperation(
                operation,
                targetNodeRid == _routingId
                    ? RequestResult.NotFound
                    : RequestResult.NotConnected,
                0,
                Array.Empty<Message>());
            return operation.OperationId;
        }

        var previous = actor.Snapshot();
        var parts = CloneParts(requestParts ?? Array.Empty<Message>());
        var control = new ActorControlRecord(
            ActorLifecycleKind.Joined,
            previous.Ref,
            previous.Ref,
            previous.SpotId,
            targetSpot.SpotId,
            previous.SpotGeneration,
            targetSpot.LifecycleGeneration,
            previous.MembershipEpoch,
            checked(previous.MembershipEpoch + 1),
            0);
        var record = new MeshReceiveRecord(
            MeshRecordKind.SpotControl,
            MeshReadyDomains.Application,
            _routingId,
            previous.SpotId,
            _lifecycleGeneration,
            previous.Ref,
            operation.OperationId,
            MeshOperationKind.ActorJoin,
            null,
            null,
            null,
            0,
            parts.Count,
            0,
            0,
            control,
            joinReply: (result, reply, _) =>
                CompleteJoin(
                    operation,
                    actor,
                    previous,
                    targetSpot,
                    result,
                    reply,
                    entry));
        EnqueueOwned(
            MailboxKey.ForSpot(targetSpot, MeshReadyDomains.Application),
            record,
            parts);
        return operation.OperationId;
    }

    internal SubmitResult BindSessionActor(
        ZLinkManagedStreamSessionService service,
        RoutingId sessionRid,
        ActorRef actorRef,
        out MeshOperationId operationId,
        TimeSpan timeout)
    {
        var operation = BeginOperation(MeshOperationKind.StreamBind, timeout);
        operationId = operation.OperationId;
        if (!TryGetActor(actorRef, out var actor))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.NotFound,
                0,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }
        var bindingGeneration = actor.Bind(service, sessionRid);
        service.RecordBinding(
            sessionRid,
            new StreamSessionBinding(
                sessionRid,
                actor.Ref,
                bindingGeneration,
                actor.MembershipEpoch));
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
        return SubmitResult.Ok;
    }

    internal SubmitResult BindSessionActor(
        ZLinkManagedStreamSessionService service,
        RoutingId sessionRid,
        ActorRef actorRef,
        MeshOperationId correlationId,
        TimeSpan timeout)
    {
        if (!TryBeginOperation(
                MeshOperationKind.StreamBind,
                correlationId,
                timeout,
                out var operation))
            return SubmitResult.Backpressured;
        if (!TryGetActor(actorRef, out var actor))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.NotFound,
                0,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }
        var bindingGeneration = actor.Bind(service, sessionRid);
        service.RecordBinding(
            sessionRid,
            new StreamSessionBinding(
                sessionRid,
                actor.Ref,
                bindingGeneration,
                actor.MembershipEpoch));
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
        return SubmitResult.Ok;
    }

    internal SubmitResult UnbindSessionActor(
        ZLinkManagedStreamSessionService service,
        RoutingId sessionRid,
        ActorRef actorRef,
        ulong expectedBindingGeneration,
        out MeshOperationId operationId,
        TimeSpan timeout)
    {
        var operation = BeginOperation(MeshOperationKind.StreamUnbind, timeout);
        operationId = operation.OperationId;
        if (!TryGetActor(actorRef, out var actor)
            || !actor.TryClearBinding(expectedBindingGeneration))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.NotFound,
                0,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }
        service.RemoveBinding(
            sessionRid,
            actorRef.ActorId,
            expectedBindingGeneration);
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
        return SubmitResult.Ok;
    }

    internal SubmitResult UnbindSessionActor(
        ZLinkManagedStreamSessionService service,
        RoutingId sessionRid,
        ActorRef actorRef,
        ulong expectedBindingGeneration,
        MeshOperationId correlationId,
        TimeSpan timeout)
    {
        if (!TryBeginOperation(
                MeshOperationKind.StreamUnbind,
                correlationId,
                timeout,
                out var operation))
            return SubmitResult.Backpressured;
        if (!TryGetActor(actorRef, out var actor)
            || !actor.TryClearBinding(expectedBindingGeneration))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.NotFound,
                0,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }
        service.RemoveBinding(
            sessionRid,
            actorRef.ActorId,
            expectedBindingGeneration);
        CompleteManagedOperation(operation, RequestResult.Ok, 0, Array.Empty<Message>());
        return SubmitResult.Ok;
    }

    internal SubmitResult RelaySessionToActor(
        ZLinkManagedStreamSessionService service,
        RoutingId sessionRid,
        ActorRef actorRef,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        if (!TryGetActor(actorRef, out var actor)
            || actor.Binding is not { } binding
            || !ReferenceEquals(binding.Service, service)
            || binding.SessionRid != sessionRid)
            return SubmitResult.NotFound;
        return SubmitActor(
            actorRef,
            parts,
            request: false,
            default,
            flags,
            out _);
    }

    private SubmitResult CompleteJoin(
        PendingOperation operation,
        ManagedActor actor,
        ActorSnapshot previous,
        ZLinkManagedSpot targetSpot,
        ActorJoinResult result,
        IReadOnlyList<Message> reply,
        bool entry)
    {
        if (!_operations.TryGetValue(operation.OperationId.Low, out var active)
            || !ReferenceEquals(active, operation))
            return SubmitResult.Terminated;

        if (result == ActorJoinResult.Rejected)
        {
            CompleteManagedOperation(
                operation,
                RequestResult.Rejected,
                0,
                CloneParts(reply),
                new ActorJoinCompletion(
                    result,
                    actor.Ref,
                    actor.Location));
            return SubmitResult.Ok;
        }

        if (!actor.TryMove(
                previous,
                targetSpot.SpotId,
                targetSpot.LifecycleGeneration))
        {
            CompleteManagedOperation(
                operation,
                RequestResult.Conflict,
                1,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }

        if (_spots.TryGetValue(
                ZLinkSpotId.FromBoundary(previous.SpotId, nameof(previous.SpotId)),
                out var oldSpot)
            && !ReferenceEquals(oldSpot, targetSpot))
        {
            oldSpot.RemoveActor();
            EnqueueActorLifecycle(
                oldSpot,
                ActorLifecycleKind.Left,
                previous,
                actor,
                Array.Empty<Message>());
        }
        if (!string.Equals(previous.SpotId, targetSpot.SpotId, StringComparison.Ordinal))
            targetSpot.AddActor();
        EnqueueActorLifecycle(
            targetSpot,
            entry ? ActorLifecycleKind.Joined : ActorLifecycleKind.Joined,
            previous,
            actor,
            Array.Empty<Message>());
        CompleteManagedOperation(
            operation,
            RequestResult.Ok,
            0,
            CloneParts(reply),
            new ActorJoinCompletion(
                result,
                actor.Ref,
                actor.Location));
        return SubmitResult.Ok;
    }

    internal async ValueTask SendToSpotDirectAsync(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        if (targetRid == _routingId)
        {
            var result = SubmitSpot(
                targetRid,
                sourceSpotId,
                spotId,
                spotGeneration,
                parts,
                request: false,
                operation: null,
                flags,
                metadata);
            if (result != SubmitResult.Ok)
                throw new ZlinkSubmitException(
                    (ZlinkSubmitException.ErrorCode)(int)result);
            return;
        }
        var peer = RequireDirectSpotPeer(targetRid, spotId, spotGeneration, out var authority);
        var operationId = NextStandaloneOperationId();
        var head = ZLinkServiceWireCodec.EncodeSpot(
            ServiceWireConstants.Command.SpotSend,
            0,
            operationId,
            sourceSpotId,
            spotId,
            spotGeneration,
            targetRid,
            authority.TargetNodeGeneration,
            authority.AuthorityOwnerGeneration,
            authority.OwnerLeaseGeneration,
            !metadata.IsEmpty,
            0,
            0);
        await SendDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateStatefulWire(head, parts, metadata),
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestToSpotDirectAsync(
        string sourceSpotId,
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (targetRid == _routingId)
        {
            var effectiveTimeout = timeout <= TimeSpan.Zero
                ? TimeSpan.FromSeconds(30)
                : timeout;
            if (!TryCreateOperation(
                    MeshOperationKind.SpotRequest,
                    out var correlation,
                    out var operation,
                    awaitCompletion: true))
            {
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Backpressured);
            }
            operation.DeadlineUnixMs = checked((ulong)DateTimeOffset.UtcNow
                .Add(effectiveTimeout)
                .ToUnixTimeMilliseconds());
            var result = SubmitSpot(
                targetRid,
                sourceSpotId,
                spotId,
                spotGeneration,
                parts,
                request: true,
                operation,
                flags,
                metadata);
            if (result != SubmitResult.Ok)
            {
                TryRemoveOperation(correlation, out _);
                operation.Cancel();
                throw new ZlinkSubmitException(
                    (ZlinkSubmitException.ErrorCode)(int)result);
            }
            _ = ExpireOperationAsync(correlation, operation, effectiveTimeout);
            using var cancellation = cancellationToken.Register(() =>
            {
                if (TryRemoveOperation(correlation, out _)) operation.Cancel();
            });
            var completion = await operation.AwaitedCompletion!.Task
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            if (completion.Result != RequestResult.Ok)
            {
                DisposeParts(completion.Parts);
                //  Application terminal from a spot completion — carry the fine
                //  failure code so the source-side classifier can refine it
                //  (spec 32-framework-error-model:81-118).
                throw new ZLinkRequestTerminalException(
                    completion.Result,
                    completion.FailureErrno);
            }
            return completion.Parts;
        }
        var peer = RequireDirectSpotPeer(targetRid, spotId, spotGeneration, out var authority);
        var operationId = NextStandaloneOperationId();
        var head = ZLinkServiceWireCodec.EncodeSpot(
            ServiceWireConstants.Command.SpotRequest,
            operationId.Low,
            operationId,
            sourceSpotId,
            spotId,
            spotGeneration,
            targetRid,
            authority.TargetNodeGeneration,
            authority.AuthorityOwnerGeneration,
            authority.OwnerLeaseGeneration,
            !metadata.IsEmpty,
            0,
            checked((ulong)DateTimeOffset.UtcNow.Add(timeout).ToUnixTimeMilliseconds()));
        var reply = await RequestDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateStatefulWire(head, parts, metadata),
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
        return DecodeDirectApplicationReply(operationId.Low, reply);
    }

    private Peer RequireDirectSpotPeer(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        out ObservedAuthority authority)
    {
        var peer = RequireDirectPeer(targetRid);
        if (spotGeneration == 0
            || !_observedSpotAuthorities.TryGetValue(
                new ObservedSpotAuthorityKey(targetRid, spotId, spotGeneration),
                out authority))
            throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotFound);
        if (peer.LifecycleGeneration != authority.TargetNodeGeneration)
            throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotFound);
        return peer;
    }

    private static IReadOnlyList<ReadOnlyMemory<byte>> CreateStatefulWire(
        byte[] head,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("Application payload is required.", nameof(parts));
        var wire = new List<ReadOnlyMemory<byte>>(metadata.IsEmpty ? 2 : 3) { head };
        if (!metadata.IsEmpty)
            wire.Add(metadata);
        wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        return wire;
    }

    private SubmitResult SubmitSpot(
        RoutingId targetNodeRid,
        string sourceSpotId,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message> parts,
        bool request,
        PendingOperation? operation,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        SpotMessageFollowRoute? messageFollowRoute = null)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("Application payload is required.", nameof(parts));
        var routedOperationId = messageFollowRoute?.OperationId
            ?? operation?.OperationId
            ?? NextStandaloneOperationId();
        if (targetNodeRid != _routingId)
        {
            var preparation = TryPrepareRemoteSpotWire(
                targetNodeRid,
                sourceSpotId,
                targetSpotId,
                targetSpotGeneration,
                routedOperationId,
                request,
                operation,
                metadata,
                messageFollowRoute,
                out var peer,
                out var head);
            if (preparation != SubmitResult.Ok)
                return preparation;
            return SubmitStatefulWire(
                peer,
                head,
                parts,
                flags,
                metadata,
                request ? operation : null);
        }
        if (!_spots.TryGetValue(
                ZLinkSpotId.FromBoundary(targetSpotId, nameof(targetSpotId)),
                out var spot))
            return SubmitResult.NotFound;
        if (targetSpotGeneration != 0
            && spot.LifecycleGeneration != targetSpotGeneration)
            return SubmitResult.InvalidState;
        var hasLocalAuthority = _observedSpotAuthorities.TryGetValue(
                new ObservedSpotAuthorityKey(
                    targetNodeRid,
                    targetSpotId,
                    targetSpotGeneration),
                out var localAuthority);
        if (hasLocalAuthority
            && (localAuthority.TargetNodeGeneration != _lifecycleGeneration
                || localAuthority.AuthorityOwnerGeneration
                   != spot.AuthorityOwnerGeneration
                || localAuthority.OwnerLeaseGeneration
                   != checked((ulong)Volatile.Read(
                       ref _localOwnerLeaseGeneration))))
            return SubmitResult.NotFound;
        if (!hasLocalAuthority)
            localAuthority = new ObservedAuthority(
                _lifecycleGeneration,
                spot.AuthorityOwnerGeneration,
                checked((ulong)Volatile.Read(ref _localOwnerLeaseGeneration)));

        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null;
        if (request && operation is not null)
            reply = (replyParts, _) =>
            {
                CompleteManagedOperation(
                    operation,
                    RequestResult.Ok,
                    0,
                    CloneParts(replyParts));
                return SubmitResult.Ok;
            };
        var retained = CloneParts(parts);
        var record = new MeshReceiveRecord(
            request ? MeshRecordKind.SpotRequest : MeshRecordKind.SpotSend,
            MeshReadyDomains.Application,
            _routingId,
            string.Empty,
            _lifecycleGeneration,
            default,
            routedOperationId,
            request ? MeshOperationKind.SpotRequest : default,
            null,
            null,
            metadata.IsEmpty ? null : metadata.ToArray(),
            0,
            retained.Count,
            0,
            0,
            null,
            reply,
            targetNodeGeneration: localAuthority.TargetNodeGeneration,
            authorityOwnerGeneration: localAuthority.AuthorityOwnerGeneration,
            ownerLeaseGeneration: localAuthority.OwnerLeaseGeneration,
            replyRouteId: request ? operation?.OperationId.Low ?? 0 : 0,
            deadlineUnixMs: request
                ? messageFollowRoute?.DeadlineUnixMs
                  ?? operation?.DeadlineUnixMs
                  ?? throw new InvalidOperationException(
                      "Spot requests require an absolute deadline.")
                : 0);
        EnqueueOwned(
            MailboxKey.ForSpot(spot, MeshReadyDomains.Application),
            record,
            retained);
        return SubmitResult.Ok;
    }

    private SubmitResult TryPrepareRemoteSpotWire(
        RoutingId targetNodeRid,
        string sourceSpotId,
        string targetSpotId,
        ulong targetSpotGeneration,
        MeshOperationId routedOperationId,
        bool request,
        PendingOperation? operation,
        ReadOnlyMemory<byte> metadata,
        SpotMessageFollowRoute? messageFollowRoute,
        out Peer peer,
        out byte[] head)
    {
        head = [];
        peer = null!;
        lock (_gate)
        {
            if (!_peersByRid.TryGetValue(targetNodeRid, out var resolvedPeer)
                || !resolvedPeer.Admitted)
                return SubmitResult.NotConnected;
            peer = resolvedPeer;
        }

        ObservedAuthority authority;
        if (messageFollowRoute is { } messageFollowRouteValue)
        {
            if (messageFollowRouteValue.OperationId == default
                || messageFollowRouteValue.TargetNodeGeneration == 0
                || messageFollowRouteValue.AuthorityOwnerGeneration == 0
                || messageFollowRouteValue.OwnerLeaseGeneration == 0
                || messageFollowRouteValue.MessageFollowHopCount is 0 or > 8)
                return SubmitResult.InvalidState;
            authority = new ObservedAuthority(
                messageFollowRouteValue.TargetNodeGeneration,
                messageFollowRouteValue.AuthorityOwnerGeneration,
                messageFollowRouteValue.OwnerLeaseGeneration);
        }
        else if (!_observedSpotAuthorities.TryGetValue(
                     new ObservedSpotAuthorityKey(
                         targetNodeRid,
                         targetSpotId,
                         targetSpotGeneration),
                     out authority))
        {
            return SubmitResult.NotFound;
        }

        if (targetSpotGeneration == 0
            || peer.LifecycleGeneration != authority.TargetNodeGeneration)
            return SubmitResult.NotFound;
        head = ZLinkServiceWireCodec.EncodeSpot(
            request
                ? ServiceWireConstants.Command.SpotRequest
                : ServiceWireConstants.Command.SpotSend,
            operation?.OperationId.Low ?? 0,
            routedOperationId,
            sourceSpotId,
            targetSpotId,
            targetSpotGeneration,
            targetNodeRid,
            authority.TargetNodeGeneration,
            authority.AuthorityOwnerGeneration,
            authority.OwnerLeaseGeneration,
            !metadata.IsEmpty,
            messageFollowRoute?.MessageFollowHopCount ?? 0,
            request
                ? messageFollowRoute?.DeadlineUnixMs
                  ?? operation?.DeadlineUnixMs
                  ?? throw new InvalidOperationException(
                      "Spot requests require an absolute deadline.")
                : 0);
        return SubmitResult.Ok;
    }

    private readonly record struct SpotMessageFollowRoute(
        MeshOperationId OperationId,
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration,
        byte MessageFollowHopCount,
        ulong DeadlineUnixMs);

    internal async ValueTask SendToActorDirectAsync(
        ActorRef actorRef,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        CancellationToken cancellationToken)
    {
        var peer = RequireDirectPeer(actorRef.NodeRid);
        if (!_observedActorAuthorities.TryGetValue(
                new ObservedActorAuthorityKey(
                    actorRef.NodeRid, actorRef.ActorId, actorRef.ObjectGeneration),
                out var authority)
            || peer.LifecycleGeneration != authority.TargetNodeGeneration)
            throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotFound);
        var operationId = NextStandaloneOperationId();
        var head = ZLinkServiceWireCodec.EncodeActor(
            ServiceWireConstants.Command.ActorSend,
            0,
            operationId,
            actorRef,
            actorRef.NodeRid,
            authority.TargetNodeGeneration,
            authority.AuthorityOwnerGeneration,
            authority.OwnerLeaseGeneration,
            hasMetadata: false,
            deadlineUnixMs: 0);
        await SendDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateStatefulWire(head, parts, default),
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: actorRef.NodeRid);
    }

    private SubmitResult SubmitActor(
        ActorRef actorRef,
        IReadOnlyList<Message> parts,
        bool request,
        PendingOperation? operation,
        SendFlags flags,
        out ulong acceptedSequence)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("Application payload is required.", nameof(parts));
        var routedOperationId = operation?.OperationId
            ?? NextStandaloneOperationId();
        acceptedSequence = 0;
        if (actorRef.NodeRid != _routingId)
        {
            Peer? peer;
            lock (_gate)
                _peersByRid.TryGetValue(actorRef.NodeRid, out peer);
            if (peer is null || !peer.Admitted)
                return SubmitResult.NotConnected;
            if (!_observedActorAuthorities.TryGetValue(
                    new ObservedActorAuthorityKey(
                        actorRef.NodeRid,
                        actorRef.ActorId,
                        actorRef.ObjectGeneration),
                out var authority)
                || peer.LifecycleGeneration != authority.TargetNodeGeneration)
                return SubmitResult.NotFound;
            var head = ZLinkServiceWireCodec.EncodeActor(
                request
                    ? ServiceWireConstants.Command.ActorRequest
                    : ServiceWireConstants.Command.ActorSend,
                operation?.OperationId.Low ?? 0,
                routedOperationId,
                actorRef,
                actorRef.NodeRid,
                authority.TargetNodeGeneration,
                authority.AuthorityOwnerGeneration,
                authority.OwnerLeaseGeneration,
                hasMetadata: false,
                deadlineUnixMs: request
                    ? operation?.DeadlineUnixMs
                      ?? throw new InvalidOperationException(
                          "Actor requests require an absolute deadline.")
                    : 0);
            return SubmitStatefulWire(
                peer,
                head,
                parts,
                flags,
                default,
                request ? operation : null);
        }
        if (!TryGetActor(actorRef, out var actor))
            return SubmitResult.NotFound;
        var hasLocalAuthority = _observedActorAuthorities.TryGetValue(
                new ObservedActorAuthorityKey(
                    actorRef.NodeRid,
                    actorRef.ActorId,
                    actorRef.ObjectGeneration),
                out var localAuthority);
        if (hasLocalAuthority
            && (localAuthority.TargetNodeGeneration != _lifecycleGeneration
                || localAuthority.AuthorityOwnerGeneration
                   != actor.AuthorityOwnerGeneration
                || localAuthority.OwnerLeaseGeneration
                   != checked((ulong)Volatile.Read(
                       ref _localOwnerLeaseGeneration))))
            return SubmitResult.NotFound;
        if (!hasLocalAuthority)
            localAuthority = new ObservedAuthority(
                _lifecycleGeneration,
                actor.AuthorityOwnerGeneration,
                checked((ulong)Volatile.Read(ref _localOwnerLeaseGeneration)));
        acceptedSequence = actor.NextSequence();
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null;
        if (request && operation is not null)
            reply = (replyParts, _) =>
            {
                CompleteManagedOperation(
                    operation,
                    RequestResult.Ok,
                    0,
                    CloneParts(replyParts));
                return SubmitResult.Ok;
            };
        var retained = CloneParts(parts);
        var record = new MeshReceiveRecord(
            request ? MeshRecordKind.ActorRequest : MeshRecordKind.ActorSend,
            MeshReadyDomains.Application,
            _routingId,
            string.Empty,
            _lifecycleGeneration,
            default,
            routedOperationId,
            request ? MeshOperationKind.ActorRequest : default,
            null,
            null,
            null,
            0,
            retained.Count,
            0,
            0,
            null,
            reply,
            targetNodeGeneration: localAuthority.TargetNodeGeneration,
            authorityOwnerGeneration: localAuthority.AuthorityOwnerGeneration,
            ownerLeaseGeneration: localAuthority.OwnerLeaseGeneration,
            replyRouteId: request ? operation?.OperationId.Low ?? 0 : 0,
            deadlineUnixMs: request
                ? operation?.DeadlineUnixMs
                  ?? throw new InvalidOperationException(
                      "Actor requests require an absolute deadline.")
                : 0);
        EnqueueOwned(
            MailboxKey.ForActor(actor, MeshReadyDomains.Application),
            record,
            retained);
        return SubmitResult.Ok;
    }

    private SubmitResult SubmitStatefulWire(
        Peer peer,
        byte[] head,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        PendingOperation? pending = null)
    {
        if (pending is not null)
            return SubmitNativeServiceRequest(
                peer,
                head,
                parts,
                flags,
                metadata,
                pending);

        var wireParts = new List<ReadOnlyMemory<byte>>(3) { head };
        if (!metadata.IsEmpty)
            wireParts.Add(metadata);
        wireParts.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        if (!TryScheduleRoutedSend(peer.PhysicalRoutingId, wireParts))
        {
            Publish(MeshMonitorEventKind.Backpressured, peerRid: peer.RoutingId);
            return SubmitResult.Backpressured;
        }
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: peer.RoutingId);
        return SubmitResult.Ok;
    }

    private SubmitResult SubmitNativeServiceRequest(
        Peer peer,
        byte[] head,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        PendingOperation pending)
    {
        var wire = new List<ReadOnlyMemory<byte>>(metadata.IsEmpty ? 2 : 3)
        {
            head
        };
        if (!metadata.IsEmpty)
            wire.Add(metadata);
        wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));

        var remainingMilliseconds = pending.DeadlineUnixMs
            - Math.Min(
                pending.DeadlineUnixMs,
                checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
        var remaining = TimeSpan.FromMilliseconds(
            Math.Max(1, Math.Min(remainingMilliseconds, (ulong)int.MaxValue)));
        try
        {
            if (!RunInboundOperation(() => CompleteNativeApplicationRequestAsync(
                    peer.PhysicalRoutingId,
                    pending,
                    wire,
                    remaining,
                    _stop?.Token ?? CancellationToken.None)))
                return SubmitResult.Terminated;

            Publish(
                MeshMonitorEventKind.MessageSubmitted,
                peerRid: peer.RoutingId);
            return SubmitResult.Ok;
        }
        catch (ObjectDisposedException)
        {
            return SubmitResult.Terminated;
        }
        catch (ZlinkSubmitException exception)
        {
            return exception.Result == ZlinkSubmitException.ErrorCode.Backpressured
                ? SubmitResult.Backpressured
                : SubmitResult.Terminated;
        }
        catch (ZlinkException)
        {
            return SubmitResult.Terminated;
        }
    }

    private async ValueTask<InstanceSpotActivationTerminal>
        SubmitForwardedInstanceSpotRequestAsync(
            Peer peer,
            InstanceSpotActivationOperation operation,
            byte[] head,
            IReadOnlyList<ReadOnlyMemory<byte>> parts,
            ReadOnlyMemory<byte>? metadata,
            CancellationToken cancellationToken)
    {
        var wire = new Message[metadata is { IsEmpty: false } ? 3 : 2];
        var created = 0;
        var ownershipTransferred = false;
        try
        {
            wire[created++] = Message.From(head);
            if (metadata is { IsEmpty: false } value)
                wire[created++] = Message.From(value);
            wire[created++] = Message.From(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));

            var remainingMilliseconds = operation.DeadlineUnixMs
                - Math.Min(
                    operation.DeadlineUnixMs,
                    checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
            if (remainingMilliseconds == 0)
                return new InstanceSpotActivationTerminal(
                    RequestResult.InternalError,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                    Array.Empty<ReadOnlyMemory<byte>>());
            var remaining = TimeSpan.FromMilliseconds(
                Math.Max(1, Math.Min(remainingMilliseconds, (ulong)int.MaxValue)));
            Task<IReadOnlyList<Message>> request;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ZlinkSubmitException(
                        ZlinkSubmitException.ErrorCode.Terminated);
                var requestOperation = socket.Request(peer.PhysicalRoutingId)
                    .Messages(wire)
                    .Timeout(remaining);
                ownershipTransferred = true;
                request = requestOperation.Async(cancellationToken);
            }

            Publish(
                MeshMonitorEventKind.MessageSubmitted,
                peerRid: peer.RoutingId);
            var replyParts = await request.ConfigureAwait(false);
            try
            {
                return DecodeForwardedInstanceSpotTerminal(
                    operation,
                    RequestResult.Ok,
                    replyParts);
            }
            finally
            {
                DisposeParts(replyParts);
            }
        }
        catch (ZlinkRequestException exception)
        {
            return DecodeForwardedInstanceSpotTerminal(
                operation,
                (RequestResult)(int)exception.Result,
                Array.Empty<Message>());
        }
        catch (ObjectDisposedException)
        {
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated);
        }
        catch (ZlinkException exception)
            when (exception is not ZlinkSubmitException)
        {
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Terminated);
        }
        finally
        {
            if (!ownershipTransferred)
                for (var index = 0; index < created; index++)
                    wire[index].Dispose();
        }
    }

    private static InstanceSpotActivationTerminal
        DecodeForwardedInstanceSpotTerminal(
            InstanceSpotActivationOperation operation,
            RequestResult transportResult,
            IReadOnlyList<Message> replyParts)
    {
        if (transportResult != RequestResult.Ok)
        {
            //  Schema terminal-failure-integrity: a worker timeout is encoded
            //  as internalError+workerTimedOut (19 pairs only with 105; the
            //  fine code still classifies publicly to DeadlineExceeded), a
            //  boundary transport terminal carries None, and any other typed
            //  transport terminal falls back to internalError+requestFailed.
            if (transportResult == RequestResult.TimedOut)
                return new InstanceSpotActivationTerminal(
                    RequestResult.InternalError,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                    Array.Empty<ReadOnlyMemory<byte>>());
            if (ServiceWireConstants.ValidTerminalFailure(
                    unchecked((uint)transportResult), 0u))
                return new InstanceSpotActivationTerminal(
                    transportResult,
                    ServiceWireConstants.FrameworkErrorCode.None,
                    Array.Empty<ReadOnlyMemory<byte>>());
            return new InstanceSpotActivationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                Array.Empty<ReadOnlyMemory<byte>>());
        }
        if (replyParts.Count == 0
            || !ZLinkServiceWireCodec.TryDecodeReply(
                replyParts[0].ToArray(),
                out var reply,
                out _)
            || reply.Correlation != operation.ReplyRouteId)
            return new InstanceSpotActivationTerminal(
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                Array.Empty<ReadOnlyMemory<byte>>());

        var result = (RequestResult)reply.TerminalResult;
        var failureCode = (ServiceWireConstants.FrameworkErrorCode)reply.FailureCode;
        if (result != RequestResult.Ok)
        {
            if (replyParts.Count != 1)
                return new InstanceSpotActivationTerminal(
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<ReadOnlyMemory<byte>>());

            return new InstanceSpotActivationTerminal(
                result,
                failureCode,
                Array.Empty<ReadOnlyMemory<byte>>());
        }

        if (replyParts.Count == 1)
            return new InstanceSpotActivationTerminal(
                result,
                failureCode,
                Array.Empty<ReadOnlyMemory<byte>>());

        if (replyParts.Count != 2
            || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                replyParts[1].AsReadOnlyMemory(),
                out var decodedParts))
            return new InstanceSpotActivationTerminal(
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                Array.Empty<ReadOnlyMemory<byte>>());

        try
        {
            return new InstanceSpotActivationTerminal(
                result,
                failureCode,
                decodedParts
                    .Select(static part => (ReadOnlyMemory<byte>)part.ToArray())
                    .ToArray());
        }
        finally
        {
            DisposeParts(decodedParts);
        }
    }

    private PendingOperation BeginOperation(
        MeshOperationKind kind,
        TimeSpan timeout)
    {
        if (!TryBeginOperation(kind, timeout, out var operation))
            throw new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.Backpressured);
        return operation;
    }

    private bool TryBeginOperation(
        MeshOperationKind kind,
        TimeSpan timeout,
        out PendingOperation operation)
    {
        var operationId = NextStandaloneOperationId();
        return TryBeginOperation(kind, operationId, timeout, out operation);
    }

    private bool TryBeginOperation(
        MeshOperationKind kind,
        MeshOperationId operationId,
        TimeSpan timeout,
        out PendingOperation operation)
    {
        var effectiveTimeout = timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30)
            : timeout;
        if (!TryCreateOperation(kind, operationId, out var correlation, out operation))
            return false;
        operation.DeadlineUnixMs = checked((ulong)DateTimeOffset.UtcNow
            .Add(effectiveTimeout)
            .ToUnixTimeMilliseconds());
        _ = ExpireOperationAsync(
            correlation,
            operation,
            effectiveTimeout);
        return true;
    }

    private bool TryCreateOperation(
        MeshOperationKind kind,
        out ulong correlation,
        out PendingOperation operation,
        bool awaitCompletion = false)
    {
        var operationId = NextStandaloneOperationId();
        return TryCreateOperation(
            kind,
            operationId,
            out correlation,
            out operation,
            awaitCompletion);
    }

    private bool TryCreateOperation(
        MeshOperationKind kind,
        MeshOperationId operationId,
        out ulong correlation,
        out PendingOperation operation,
        bool awaitCompletion = false)
    {
        lock (_operationGate)
        {
            RemoveExpiredRelocationReplyTerminalsUnderLock(DateTimeOffset.UtcNow);
            if (_operations.Count >= _maxPendingOperations)
            {
                correlation = 0;
                operation = null!;
                return false;
            }
            if (operationId.High != _lifecycleGeneration
                || operationId.Low == 0)
                throw new ArgumentException(
                    "The operation id must be allocated by this MeshNode lifecycle.",
                    nameof(operationId));
            correlation = operationId.Low;
            operation = new PendingOperation(
                operationId,
                kind,
                _localRequestSourceFence,
                awaitCompletion);
            if (!_operations.TryAdd(correlation, operation))
                throw new InvalidOperationException(
                    "The operation id was reused.");
            if (IsRelocatableRequest(kind))
            {
                var relocationKey = new RelocationReplyTerminalKey(
                    correlation,
                    operation.OperationId);
                if (!_relocationReplyOperations.TryAdd(
                        relocationKey,
                        operation))
                {
                    _operations.TryRemove(correlation, out _);
                    throw new InvalidOperationException(
                        "The relocation reply operation id was reused.");
                }
            }
            return true;
        }
    }

    public MeshOperationId AllocateOperationId() => NextStandaloneOperationId();

    private bool TryRemoveOperation(
        ulong correlation,
        out PendingOperation operation,
        bool rememberRelocationTerminal = true)
    {
        lock (_operationGate)
        {
            if (!_operations.TryRemove(correlation, out operation!))
                return false;
            RemoveRelocationReplyOperationUnderLock(
                operation,
                rememberRelocationTerminal);
            return true;
        }
    }

    private bool TryRemoveOperation(
        KeyValuePair<ulong, PendingOperation> operation,
        bool rememberRelocationTerminal = true)
    {
        lock (_operationGate)
        {
            if (!_operations.TryRemove(operation))
                return false;
            RemoveRelocationReplyOperationUnderLock(
                operation.Value,
                rememberRelocationTerminal);
            return true;
        }
    }

    private void RemoveManagedOperation(PendingOperation operation)
    {
        TryRemoveOperation(
            new KeyValuePair<ulong, PendingOperation>(
                operation.OperationId.Low,
                operation),
            rememberRelocationTerminal: false);
        operation.Cancel();
    }

    private void CompleteManagedOperation(
        PendingOperation operation,
        RequestResult result,
        int failure,
        IReadOnlyList<Message> parts,
        MeshRecordPayload? kindData = null)
    {
        if (!TryRemoveOperation(
                new KeyValuePair<ulong, PendingOperation>(
                    operation.OperationId.Low,
                    operation))
            || !operation.TryComplete())
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"managed_operation_completion_rejected operation={operation.OperationId.High:x16}{operation.OperationId.Low:x16} "
                + $"reply_route={operation.OperationId.Low:x16} kind={operation.Kind}");
            DisposeParts(parts);
            return;
        }
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"managed_operation_completed operation={operation.OperationId.High:x16}{operation.OperationId.Low:x16} "
            + $"reply_route={operation.OperationId.Low:x16} kind={operation.Kind}");
        if (operation.AwaitedCompletion is { } awaitedCompletion)
        {
            if (!awaitedCompletion.TrySetResult(
                    new ManagedRequestCompletion(result, parts, failure)))
                DisposeParts(parts);
            return;
        }
        EnqueueCompletion(
            operation.OperationId,
            operation.Kind,
            (int)result,
            failure,
            parts,
            kindData,
            publishEvent: false);
    }

    private static async Task DeliverRequestCompletionAsync(
        Task<ManagedRequestCompletion> completionTask,
        ZLinkBackendRequestCallback callback)
    {
        var completion = await completionTask.ConfigureAwait(false);
        try
        {
            callback(completion.Result, completion.Parts);
        }
        catch
        {
            DisposeParts(completion.Parts);
        }
    }

    internal ZLinkRelocationReplyCompletion TryCompleteRelocationReply(
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        IReadOnlyList<Message> payload)
    {
        ArgumentNullException.ThrowIfNull(payload);
        var key = new RelocationReplyTerminalKey(
            relay.ReplyRouteId,
            relay.OperationId);
        PendingOperation? pending = null;
        ZLinkServiceWireCodec.RequestSourceFence requestSource = default;
        var alreadyTerminal = false;
        lock (_operationGate)
        {
            var now = DateTimeOffset.UtcNow;
            RemoveExpiredRelocationReplyTerminalsUnderLock(now);
            if (_relocationReplyTerminals.TryGetValue(key, out var terminal))
            {
                requestSource = terminal.RequestSource;
                alreadyTerminal = true;
            }
            else
            {
                if (!_relocationReplyOperations.TryGetValue(key, out pending)
                    || !IsRelocatableRequest(pending.Kind)
                    || pending.OperationId != relay.OperationId
                    || pending.RequestSource == default
                    || pending.RequestSource != _localRequestSourceFence
                    || !_operations.TryGetValue(
                        relay.ReplyRouteId,
                        out var routedPending)
                    || !ReferenceEquals(routedPending, pending)
                    || !_operations.TryRemove(
                        new KeyValuePair<ulong, PendingOperation>(
                            relay.ReplyRouteId,
                            pending)))
                    return default;

                _relocationReplyOperations.Remove(key);
                requestSource = pending.RequestSource;
                RememberRelocationReplyTerminalUnderLock(
                    key,
                    new RelocationReplyTerminal(
                        now + ZLinkRelocationReplyLifetime.TerminalRetention,
                        requestSource));
            }
        }

        if (alreadyTerminal)
        {
            ZLinkMessageParts.DisposeAll(payload);
            return new ZLinkRelocationReplyCompletion(
                ZLinkRelocationReplyCompletionState.AlreadyTerminal,
                requestSource);
        }

        if (!pending!.TryComplete())
        {
            ZLinkMessageParts.DisposeAll(payload);
            return new ZLinkRelocationReplyCompletion(
                ZLinkRelocationReplyCompletionState.AlreadyTerminal,
                requestSource);
        }
        if (pending.AwaitedCompletion is { } awaitedCompletion)
        {
            if (!awaitedCompletion.TrySetResult(
                    new ManagedRequestCompletion(
                        (RequestResult)relay.TerminalResult,
                        payload,
                        checked((int)relay.FailureCode))))
                ZLinkMessageParts.DisposeAll(payload);
        }
        else
        {
            EnqueueCompletion(
                pending.OperationId,
                pending.Kind,
                checked((int)relay.TerminalResult),
                checked((int)relay.FailureCode),
                payload);
        }
        return new ZLinkRelocationReplyCompletion(
            ZLinkRelocationReplyCompletionState.TerminalReceived,
            requestSource);
    }

    private static bool IsRelocatableRequest(MeshOperationKind kind) =>
        kind is MeshOperationKind.ActorRequest
            or MeshOperationKind.SpotRequest
            or MeshOperationKind.InstanceSpotRequest;

    private void RemoveRelocationReplyOperationUnderLock(
        PendingOperation operation,
        bool rememberTerminal)
    {
        if (!IsRelocatableRequest(operation.Kind))
            return;
        var key = new RelocationReplyTerminalKey(
            operation.OperationId.Low,
            operation.OperationId);
        _relocationReplyOperations.Remove(key);
        if (!rememberTerminal || operation.RequestSource == default)
            return;
        var now = DateTimeOffset.UtcNow;
        RemoveExpiredRelocationReplyTerminalsUnderLock(now);
        RememberRelocationReplyTerminalUnderLock(
            key,
            new RelocationReplyTerminal(
                now + ZLinkRelocationReplyLifetime.TerminalRetention,
                operation.RequestSource));
    }

    private void RemoveExpiredRelocationReplyTerminalsUnderLock(
        DateTimeOffset now)
    {
        while (_relocationReplyTerminalOrder.TryPeek(out var key))
        {
            if (!_relocationReplyTerminals.TryGetValue(key, out var terminal))
            {
                _relocationReplyTerminalOrder.Dequeue();
                continue;
            }
            if (terminal.ExpiresAt > now) break;
            _relocationReplyTerminalOrder.Dequeue();
            _relocationReplyTerminals.Remove(key);
        }
    }

    private void RememberRelocationReplyTerminalUnderLock(
        RelocationReplyTerminalKey key,
        RelocationReplyTerminal terminal)
    {
        if (_relocationReplyTerminals.TryAdd(key, terminal))
            _relocationReplyTerminalOrder.Enqueue(key);
        else
            _relocationReplyTerminals[key] = terminal;

        while (_relocationReplyTerminals.Count > MaxRelocationReplyTerminals
               && _relocationReplyTerminalOrder.TryDequeue(out var oldest))
            _relocationReplyTerminals.Remove(oldest);
    }

    private bool TryGetActor(ActorRef actorRef, out ManagedActor actor) =>
        _actors.TryGetValue(
            ZLinkActorId.FromBoundary(actorRef.ActorId, nameof(actorRef)),
            out actor!)
        && actor.Ref.ObjectGeneration == actorRef.ObjectGeneration
        && actor.Ref.NodeRid == actorRef.NodeRid
        && !actor.Draining;

    private void EnqueueActorLifecycle(
        ZLinkManagedSpot spot,
        ActorLifecycleKind kind,
        ManagedActor previous,
        ManagedActor current,
        IReadOnlyList<Message> parts) =>
        EnqueueActorLifecycle(
            spot,
            kind,
            previous.Snapshot(),
            current,
            parts);

    private void EnqueueActorLifecycle(
        ZLinkManagedSpot spot,
        ActorLifecycleKind kind,
        ActorSnapshot previous,
        ManagedActor current,
        IReadOnlyList<Message> parts)
    {
        var control = new ActorControlRecord(
            kind,
            previous.Ref,
            current.Ref,
            previous.SpotId,
            current.SpotId,
            previous.SpotGeneration,
            current.SpotGeneration,
            previous.MembershipEpoch,
            current.MembershipEpoch,
            0);
        var retained = CloneParts(parts);
        EnqueueOwned(
            MailboxKey.ForSpot(spot, MeshReadyDomains.Application),
            new MeshReceiveRecord(
                MeshRecordKind.SpotControl,
                MeshReadyDomains.Application,
                _routingId,
                previous.SpotId,
                _lifecycleGeneration,
                previous.Ref,
                default,
                default,
                null,
                null,
                null,
                0,
                retained.Count,
                0,
                0,
                control),
            retained);
    }

    private static IReadOnlyList<Message> CloneParts(IReadOnlyList<Message> parts) =>
        ZLinkMessageParts.CopyAll(parts);

    private static void DisposeParts(IReadOnlyList<Message> parts)
    {
        foreach (var part in parts)
            part.Dispose();
    }

    private async Task ReceiveLoop(CancellationToken cancellationToken)
    {
        var events = new PollEvent[1];
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var count = _poller!.Wait(events, PollInterval);
                if (count > 0)
                    await DrainRawSocketAsync(cancellationToken)
                        .ConfigureAwait(false);
                ProcessInfrastructure(Stopwatch.GetTimestamp());
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (ObjectDisposedException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (ZlinkException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception)
            {
                lock (_gate)
                    _state = MeshNodeState.Error;
                Publish(MeshMonitorEventKind.ProtocolError);
            }
            await Task.Yield();
        }
    }

    private async ValueTask DrainRawSocketAsync(
        CancellationToken cancellationToken)
    {
        var startedAt = Stopwatch.GetTimestamp();
        long bytes = 0;
        var maximumRecords = _applicationJobQueue is null
            ? ReceiveBatchSize
            : 1;
        for (var index = 0; index < maximumRecords; index++)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(index, bytes, startedAt))
                return;
            Received? received = null;
            ZLinkApplicationJobQueueLease? admission = null;
            try
            {
                if (_applicationJobQueue is { } applicationJobQueue)
                    admission = await applicationJobQueue
                        .AcquireAsync(cancellationToken)
                        .ConfigureAwait(false);
                received = Received.Create();
                bool available;
                lock (_socketGate)
                    available = _socket!.RecvRetained(
                        received, RecvFlags.DontWait);
                if (!available)
                    return;
                bytes = checked(
                    bytes + ZLinkReceiveBatchBudget.MeasureParts(received.Parts));
                using var ownership = new RawIngressOwnership(
                    received,
                    admission);
                received = null;
                admission = null;
                await ProcessReceivedAsync(
                        ownership,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                received?.Dispose();
                admission?.Dispose();
            }
        }
    }

    private sealed class RawIngressOwnership(
        Received received,
        ZLinkApplicationJobQueueLease? admission) : IDisposable
    {
        private Received? _received = received;
        private ZLinkApplicationJobQueueLease? _admission = admission;

        internal Received Receipt =>
            Volatile.Read(ref _received)
            ?? throw new ObjectDisposedException(nameof(RawIngressOwnership));

        internal ZLinkSharedCreditOwner ShareCoreCredit()
        {
            var coreCreditOwner = Interlocked.Exchange(ref _received, null)
                ?? throw new ObjectDisposedException(nameof(RawIngressOwnership));
            return new ZLinkSharedCreditOwner(coreCreditOwner);
        }

        internal ZLinkApplicationJobQueueLease? TakeAdmission() =>
            Interlocked.Exchange(ref _admission, null);

        internal IDisposable TakeApplicationOwner()
        {
            var coreCreditOwner = Interlocked.Exchange(ref _received, null)
                ?? throw new ObjectDisposedException(nameof(RawIngressOwnership));
            var applicationAdmission =
                Interlocked.Exchange(ref _admission, null);
            return AttachApplicationAdmission(
                coreCreditOwner,
                applicationAdmission);
        }

        public void Dispose()
        {
            try
            {
                Interlocked.Exchange(ref _received, null)?.Dispose();
            }
            finally
            {
                Interlocked.Exchange(ref _admission, null)?.Dispose();
            }
        }
    }

    private bool HasCurrentInfrastructureControlSource(
        RoutingId sourceRid,
        ServiceWireConstants.Command command)
    {
        if (command is ServiceWireConstants.Command.Hello
            or ServiceWireConstants.Command.Admit
            or ServiceWireConstants.Command.Reject
            or ServiceWireConstants.Command.Update)
            return true;

        lock (_gate)
            return _peersByRid.TryGetValue(sourceRid, out var peer)
                && peer.Admitted
                && peer.LifecycleGeneration != 0;
    }

    internal static bool IsAllowedInfrastructureControl(
        IReadOnlyList<Message> parts,
        out ServiceWireConstants.Command command)
    {
        command = default;
        if (parts.Count == 0 || parts.Count > MaxInfrastructureControlParts)
            return false;

        var head = parts[0].ToArray();
        if (head.Length < 5
            || head[0] != ServiceWireConstants.Magic0
            || head[1] != ServiceWireConstants.Magic1
            || head[2] != ServiceWireConstants.WireMajor)
            return false;

        command = (ServiceWireConstants.Command)head[3];
        if (!IsAllowedInfrastructureControlCommand(command))
            return false;

        return IsInfrastructureControlFrameShape(command, parts.Count)
            && IsWithinInfrastructureControlBounds(parts, command)
            && IsValidOptionalApplicationPayload(parts, command);
    }

    private static bool IsAllowedInfrastructureControlCommand(
        ServiceWireConstants.Command command) =>
        command is ServiceWireConstants.Command.Hello
            or ServiceWireConstants.Command.Admit
            or ServiceWireConstants.Command.Reject
            or ServiceWireConstants.Command.Update
            or ServiceWireConstants.Command.LivenessProbe
            or ServiceWireConstants.Command.LivenessAck
            or ServiceWireConstants.Command.Reply
            or ServiceWireConstants.Command.RelocationReady
            or ServiceWireConstants.Command.ReplyRelay
            or ServiceWireConstants.Command.RelocationData
            or ServiceWireConstants.Command.RelocationCutover
            or ServiceWireConstants.Command.RelocationPrepare
            or ServiceWireConstants.Command.ReplyRelayAck
            or ServiceWireConstants.Command.MessageFollow
            or ServiceWireConstants.Command.BoundSessionReplaced
            or ServiceWireConstants.Command.SessionRelocationSeal
            or ServiceWireConstants.Command.SessionRelocationSealed
            or ServiceWireConstants.Command.SessionRelocationRoute;

    private static bool TryGetInfrastructureControlCommand(
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        out ServiceWireConstants.Command command)
    {
        command = default;
        if (parts.Count == 0)
            return false;
        var head = parts[0].Span;
        if (head.Length < 5
            || head[0] != ServiceWireConstants.Magic0
            || head[1] != ServiceWireConstants.Magic1
            || head[2] != ServiceWireConstants.WireMajor)
            return false;
        command = (ServiceWireConstants.Command)head[3];
        return IsAllowedInfrastructureControlCommand(command);
    }

    private static bool IsWithinInfrastructureControlBounds(
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ServiceWireConstants.Command command)
    {
        var maximumBytes = IsPayloadBearingInfrastructureCommand(command)
            ? MaxInfrastructurePayloadBytes
            : MaxInfrastructureControlBytes;
        long totalBytes = 0;
        foreach (var part in parts)
        {
            totalBytes = checked(totalBytes + part.Length);
            if (totalBytes > maximumBytes)
                return false;
        }
        return true;
    }

    private static bool IsWithinInfrastructureControlBounds(
        IReadOnlyList<Message> parts,
        ServiceWireConstants.Command command)
    {
        var maximumBytes = IsPayloadBearingInfrastructureCommand(command)
            ? MaxInfrastructurePayloadBytes
            : MaxInfrastructureControlBytes;
        long totalBytes = 0;
        foreach (var part in parts)
        {
            totalBytes = checked(totalBytes + part.Size);
            if (totalBytes > maximumBytes)
                return false;
        }
        return true;
    }

    private static bool IsPayloadBearingInfrastructureCommand(
        ServiceWireConstants.Command command) =>
        command is ServiceWireConstants.Command.Reply
            or ServiceWireConstants.Command.ReplyRelay
            // RelocationData carries a frozen application payload and uses
            // the structural envelope limit rather than the small
            // infrastructure-control limit.
            or ServiceWireConstants.Command.RelocationData;

    private static bool IsInfrastructureControlFrameShape(
        ServiceWireConstants.Command command,
        int partCount) =>
        partCount > 0
        && command switch
        {
            ServiceWireConstants.Command.Reply => partCount is 1 or 2,
            ServiceWireConstants.Command.ReplyRelay => partCount is 1 or 2,
            _ => partCount == 1
        };

    private static bool IsValidOptionalApplicationPayload(
        IReadOnlyList<Message> parts,
        ServiceWireConstants.Command command)
    {
        if (parts.Count != 2
            || command is not (ServiceWireConstants.Command.Reply
                or ServiceWireConstants.Command.ReplyRelay))
            return true;
        return ZLinkApplicationPayloadEnvelopeCodec.TryDecode(
            parts[1].AsReadOnlyMemory(),
            out _);
    }

    private static bool IsValidOptionalApplicationPayload(
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ServiceWireConstants.Command command)
    {
        if (parts.Count != 2
            || command is not (ServiceWireConstants.Command.Reply
                or ServiceWireConstants.Command.ReplyRelay))
            return true;
        return ZLinkApplicationPayloadEnvelopeCodec.TryDecode(parts[1], out _);
    }

    private bool ProcessInfrastructureControl(
        RoutingId sourceRid,
        IReadOnlyList<Message> parts,
        byte[] head)
    {
        if (ZLinkServiceWireCodec.TryDecodeBoundSessionReplaced(
                head,
                out var boundSessionReplaced,
                out _))
        {
            if (parts.Count != 1
                || !IsValidBoundSessionReplacedSource(
                    sourceRid,
                    boundSessionReplaced))
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return true;
            }

            Action<RoutingId, ZLinkServiceWireCodec.BoundSessionReplacedRecord>?
                handler;
            lock (_gate)
                handler = _boundSessionReplacedNotificationHandler;
            handler?.Invoke(sourceRid, boundSessionReplaced);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeMessageFollow(
                head,
                out var messageFollow,
                out _))
        {
            if (parts.Count != 1
                || !IsValidMessageFollowSource(sourceRid, messageFollow))
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return true;
            }

            Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord>?
                handler;
            lock (_gate)
                handler = _messageFollowNotificationHandler;
            handler?.Invoke(sourceRid, messageFollow);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeLiveness(
                head,
                out var liveness,
                out _))
        {
            ProcessLiveness(sourceRid, liveness);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                head,
                out var admissionCommand,
                out var admission,
                out _))
        {
            ProcessAdmission(sourceRid, admissionCommand, admission);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeReply(head, out var reply, out _))
        {
            CompleteOperation(reply, parts);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeReplyIntegrityViolation(
                head, out var violatedReply))
        {
            //  Schema terminal-failure-integrity (spec 51:43-47): a Reply
            //  whose terminal/failure pair violates the generated table
            //  completes the pending operation as ProtocolError instead of
            //  falling through the command chain and being dropped.
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            CompleteOperation(violatedReply, Array.Empty<Message>());
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeReplyRelayAck(
                head,
                out var relayAck,
                out _))
        {
            ProcessReplyRelayAck(sourceRid, relayAck);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeReplyRelay(
                head,
                out var relay,
                out _))
        {
            if (!IsReplyRelayPayloadAllowed(relay.TerminalResult, parts.Count - 1))
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return true;
            }
            ProcessReplyRelay(sourceRid, relay, parts);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
                head, out var relocationPrepare, out _))
        {
            ProcessRelocationPrepare(sourceRid, relocationPrepare);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRelocationReady(
                head, out var relocationReady, out var relocationReadyError))
        {
            ProcessRelocationReady(sourceRid, relocationReady);
            return true;
        }
        if (!_pendingRelocationPrepares.IsEmpty)
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_ready_decode_skipped source={sourceRid} error={relocationReadyError}");
        if (ZLinkServiceWireCodec.TryDecodeRelocationData(
                head, out var relocationData, out _))
        {
            if (parts.Count != 1)
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return true;
            }
            ProcessRelocationData(sourceRid, relocationData);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRelocationCutover(
                head, out var relocationCutover, out _))
        {
            ProcessRelocationCutover(sourceRid, relocationCutover);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal(
                head, out var sessionRelocationSeal, out _))
        {
            ProcessSessionRelocationSeal(sourceRid, sessionRelocationSeal);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed(
                head, out var sessionRelocationSealed, out _))
        {
            ProcessSessionRelocationSealed(sourceRid, sessionRelocationSealed);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(
                head, out var sessionRelocationRoute, out _))
        {
            ProcessSessionRelocationRoute(sourceRid, sessionRelocationRoute);
            return true;
        }
        return false;
    }

    private bool IsValidMessageFollowSource(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.MessageFollowRecord record)
    {
        if (record.Source.TargetNodeRid != sourceRid)
            return false;

        lock (_gate)
        {
            if (!_peersByRid.TryGetValue(
                    record.Source.TargetNodeRid,
                    out var sourcePeer)
                || !sourcePeer.Admitted
                || sourcePeer.LifecycleGeneration
                   != record.Source.TargetNodeGeneration)
                return false;
            if (record.Target.TargetNodeRid == _routingId)
                return record.Target.TargetNodeGeneration == _lifecycleGeneration;
            return _peersByRid.TryGetValue(
                       record.Target.TargetNodeRid,
                       out var targetPeer)
                   && targetPeer.Admitted
                   && targetPeer.LifecycleGeneration
                      == record.Target.TargetNodeGeneration;
        }
    }

    private bool IsValidBoundSessionReplacedSource(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.BoundSessionReplacedRecord record)
    {
        if (sourceRid != record.ActorAuthority.TargetNodeRid
            || record.RetiredSession.SessionOwnerNodeRid != _routingId
            || record.RetiredSession.SessionOwnerNodeGeneration
               != _lifecycleGeneration)
            return false;

        lock (_gate)
        {
            if (!_peersByRid.TryGetValue(sourceRid, out var sourcePeer)
                || !sourcePeer.Admitted
                || sourcePeer.LifecycleGeneration
                   != record.ActorAuthority.TargetNodeGeneration)
                return false;
        }
        return true;
    }

    private async ValueTask<bool> ProcessReceivedAsync(
        RawIngressOwnership ownership,
        CancellationToken cancellationToken)
    {
        var received = ownership.Receipt;
        if (received.RoutingId is not { } sourceRid || received.Parts.Count == 0)
        {
            Publish(MeshMonitorEventKind.ProtocolError);
            return false;
        }

        var head = received.Parts[0].ToArray();
        if (head.Length >= 5
            && head[0] == ServiceWireConstants.Magic0
            && head[1] == ServiceWireConstants.Magic1
            && head[2] == ServiceWireConstants.WireMajor
            && IsAllowedInfrastructureControlCommand(
                (ServiceWireConstants.Command)head[3]))
        {
            if (!IsAllowedInfrastructureControl(received.Parts, out var command)
                || !HasCurrentInfrastructureControlSource(sourceRid, command)
                || !ProcessInfrastructureControl(
                    sourceRid,
                    received.Parts,
                    head))
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return false;
        }
        if (ZLinkServiceWireCodec.TryDecodeStateful(
                head,
                _meshName,
                out var stateful,
                out _))
        {
            return ProcessStateful(
                sourceRid,
                stateful,
                ownership);
        }
        if (ZLinkServiceWireCodec.TryDecodeInstanceSpotActivation(
                head,
                out var instanceActivation,
                out _))
        {
            ProcessInstanceSpotActivation(sourceRid, instanceActivation, received);
            return false;
        }
        if (ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
                head,
                out var userSpotOperation,
                out _))
        {
            ProcessUserSpotOperation(sourceRid, userSpotOperation);
            return false;
        }
        if (ZLinkServiceWireCodec.TryDecodeActorCreateOperation(
                head,
                out var actorCreateOperation,
                out _))
        {
            ProcessActorCreateOperation(sourceRid, actorCreateOperation);
            return false;
        }
        if (ZLinkServiceWireCodec.TryDecodeActorDestroy(
                head,
                _meshName,
                out var actorDestroyOperation,
                out _))
        {
            ProcessActorDestroyOperation(
                sourceRid,
                actorDestroyOperation,
                received.Parts.Count);
            return false;
        }
        if (ZLinkServiceWireCodec.TryDecodeLogicalMulticast(
                head,
                out var logicalMulticast,
                out _))
        {
            var multicastPayloadOffset = logicalMulticast.HasMetadata ? 2 : 1;
            if (received.Parts.Count != multicastPayloadOffset + 1
                || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                    received.Parts[multicastPayloadOffset].AsReadOnlyMemory(),
                    out var decodedMulticastParts))
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return false;
            }
            var multicastMetadata = logicalMulticast.HasMetadata
                ? received.Parts[1].ToArray()
                : null;
            var matchingSpots = _spots.Values
                .Where(spot => spot.Matches(
                    logicalMulticast.ChannelName,
                    logicalMulticast.Topic))
                .OrderBy(
                    static spot => spot.RoutingId.ToHex(),
                    StringComparer.Ordinal)
                .ToArray();
            var creditOwner = ownership.ShareCoreCredit();
            var delivered = false;
            try
            {
                for (var index = 0; index < matchingSpots.Length; index++)
                {
                    var spot = matchingSpots[index];
                    ZLinkApplicationJobQueueLease? childAdmission = null;
                    IDisposable? childCreditOwner = null;
                    try
                    {
                        childAdmission = index == 0
                            ? ownership.TakeAdmission()
                            : _applicationJobQueue is { } applicationJobQueue
                                ? await applicationJobQueue
                                    .AcquireAsync(cancellationToken)
                                    .ConfigureAwait(false)
                                : null;
                        childCreditOwner = AttachApplicationAdmission(
                            creditOwner.Retain(),
                            childAdmission);
                        childAdmission = null;
                        var multicastParts = CloneParts(decodedMulticastParts);
                        if (EnqueueOwned(
                                MailboxKey.ForSpot(spot, MeshReadyDomains.Application),
                                new MeshReceiveRecord(
                                    MeshRecordKind.SpotMulticast,
                                    MeshReadyDomains.Application,
                                    sourceRid,
                                    logicalMulticast.SourceSpotId,
                                    ResolvePeerGeneration(sourceRid),
                                    default,
                                    default,
                                    default,
                                    logicalMulticast.ChannelName,
                                    logicalMulticast.Topic,
                                    multicastMetadata,
                                    0,
                                    multicastParts.Count,
                                    0,
                                    0,
                                    null),
                                multicastParts,
                                true,
                                childCreditOwner))
                        {
                            delivered = true;
                            childCreditOwner = null;
                        }
                    }
                    finally
                    {
                        childCreditOwner?.Dispose();
                        childAdmission?.Dispose();
                    }
                }
            }
            finally
            {
                creditOwner.Dispose();
                DisposeParts(decodedMulticastParts);
            }
            return delivered;
        }
        if (!ZLinkServiceWireCodec.TryDecodeApplication(
            head,
            out var application,
            out _))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return false;
        }

        var payloadOffset = application.HasMetadata ? 2 : 1;
        if (received.Parts.Count != payloadOffset + 1
            || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                received.Parts[payloadOffset].AsReadOnlyMemory(),
                out var parts))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return false;
        }
        var metadata = application.HasMetadata
            ? received.Parts[1].ToArray()
            : null;
        var request = application.Command is ServiceWireConstants.Command.NodeRequest
            or ServiceWireConstants.Command.ChannelRequest;
        if (request
            && (received.MessageType != ReceivedMessageType.Request
                || received.RequestSeq is null))
        {
            // RouteMesh 11 requests use Core's request window. Accepting the old
            // raw request envelope would put its reply back on the Application
            // connection and reintroduce the ingress/completion dependency.
            DisposeParts(parts);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return false;
        }
        var kind = application.Command switch
        {
            ServiceWireConstants.Command.NodeSend => MeshRecordKind.NodeSend,
            ServiceWireConstants.Command.NodeRequest => MeshRecordKind.NodeRequest,
            ServiceWireConstants.Command.ChannelSend => MeshRecordKind.ChannelSend,
            ServiceWireConstants.Command.ChannelRequest => MeshRecordKind.ChannelRequest,
            _ => throw new InvalidOperationException()
        };
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? replyHandler = null;
        if (request)
        {
            var requestSeq = received.RequestSeq!.Value;
            var replied = 0;
            replyHandler = (replyParts, _) =>
            {
                if (Interlocked.CompareExchange(ref replied, 1, 0) != 0)
                    return SubmitResult.InvalidState;
                var result = SendNativeApplicationReply(
                    sourceRid,
                    requestSeq,
                    application.Correlation,
                    replyParts);
                if (result != SubmitResult.Ok)
                    Volatile.Write(ref replied, 0);
                return result;
            };
        }
        return EnqueueOwned(
            MailboxKey.ForNode(MeshReadyDomains.Application),
            new MeshReceiveRecord(
                kind,
                MeshReadyDomains.Application,
                sourceRid,
                string.Empty,
                ResolvePeerGeneration(sourceRid),
                default,
                request ? new MeshOperationId(0, application.Correlation) : default,
                application.Command == ServiceWireConstants.Command.NodeRequest
                    ? MeshOperationKind.NodeRequest
                    : application.Command == ServiceWireConstants.Command.ChannelRequest
                        ? MeshOperationKind.ChannelRequest
                        : default,
                application.ChannelName,
                null,
                metadata,
                0,
                parts.Length,
                0,
                0,
                null,
                replyHandler),
            parts,
            true,
            ownership.TakeApplicationOwner());
    }

    private void ProcessRelocationPrepare(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        ICanonicalRelocationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        if (target is null || peer is null || !peer.Admitted
            || peer.LifecycleGeneration != prepare.SourceNodeGeneration)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => ProcessRelocationPrepareAsync(target, peer, sourceNodeRid, prepare));
    }

    private async Task ProcessRelocationPrepareAsync(
        ICanonicalRelocationTarget target,
        Peer peer,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        var lease = new ZLinkCanonicalRelocationPreparationLease();
        try
        {
            var ready = await target.PrepareAsync(prepare, sourceNodeRid,
                    lease,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    ZLinkServiceWireCodec.EncodeRelocationReady(ready),
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            target.ReadySubmitted(prepare, sourceNodeRid);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_ready_sent relocation={ready.RelocationId.High:x16}{ready.RelocationId.Low:x16} "
                + $"attempt={ready.TargetAttemptGeneration} kind={ready.Object.Kind}");
        }
        catch (Exception exception)
        {
            try
            {
                if (lease.IsPrepared)
                    await target.AbortPreparedAsync(
                            prepare,
                            sourceNodeRid)
                        .ConfigureAwait(false);
            }
            catch (Exception abortFailure)
            {
                ZLinkFrameworkDebugLog.TaskFailure(
                    "canonical-relocation-prepare-abort",
                    abortFailure);
            }
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-prepare",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationReady(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationReadyRecord ready)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_ready_received relocation={ready.RelocationId.High:x16}{ready.RelocationId.Low:x16} "
            + $"attempt={ready.TargetAttemptGeneration} role={ready.SenderRole} kind={ready.Object.Kind}");
        var key = new PendingRelocationPrepareKey(sourceNodeRid,
            ready.RelocationId, ready.TargetAttemptGeneration,
            ready.Coordinator);
        if (ready.SenderRole == 2
            && _pendingRelocationPrepares.TryGetValue(key, out var pending)
            && pending.Matches(ready))
            pending.Ready.TrySetResult(ready);
        else
            Publish(MeshMonitorEventKind.ProtocolError,
                peerRid: sourceNodeRid);
    }

    private void ProcessRelocationData(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data)
    {
        ICanonicalRelocationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        if (target is null || peer is null || !peer.Admitted
            || data.SenderRole != 1
            || data.Coordinator.NodeRid != sourceNodeRid
            || data.Coordinator.NodeGeneration != peer.LifecycleGeneration)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => ProcessRelocationDataAsync(target, peer, sourceNodeRid, data));
    }

    private async Task ProcessRelocationDataAsync(
        ICanonicalRelocationTarget target,
        Peer peer,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data)
    {
        try
        {
            await target.StageDataAsync(data, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationCutover(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover)
    {
        ICanonicalRelocationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        if (target is null || peer is null || !peer.Admitted
            || cutover.SenderRole != 1
            || cutover.Coordinator.NodeRid != sourceNodeRid
            || cutover.Coordinator.NodeGeneration
            != peer.LifecycleGeneration)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => ProcessRelocationCutoverAsync(
                target,
                sourceNodeRid,
                cutover));
    }

    private async Task ProcessRelocationCutoverAsync(
        ICanonicalRelocationTarget target,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover)
    {
        try
        {
            await target.CutoverAsync(
                    cutover,
                    sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-cutover",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessSessionRelocationSeal(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.SessionRelocationSealRecord seal)
    {
        ISessionRelocationBarrierTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _sessionRelocationBarrierTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        if (target is null
            || peer is null
            || !peer.Admitted
            || seal.SenderRole != 1
            || seal.Coordinator.NodeRid != sourceNodeRid
            || seal.Coordinator.NodeGeneration != peer.LifecycleGeneration
            || seal.Session.SessionOwnerNodeRid != _routingId
            || seal.Session.SessionOwnerNodeGeneration != _lifecycleGeneration)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(async () =>
        {
            try
            {
                var response = await target.SealAsync(
                        seal,
                        sourceNodeRid,
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
                await SendCanonicalRelocationRecordAsync(
                        peer,
                        ZLinkServiceWireCodec.EncodeSessionRelocationSealed(
                            response),
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                ZLinkFrameworkDebugLog.TaskFailure(
                    "session-relocation-seal",
                    exception);
                Publish(MeshMonitorEventKind.ProtocolError,
                    peerRid: sourceNodeRid);
            }
        });
    }

    private void ProcessSessionRelocationSealed(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.SessionRelocationSealedRecord sealedRecord)
    {
        var key = PendingSessionRelocationKey.Create(
            sourceNodeRid,
            sealedRecord);
        if (_pendingSessionRelocationSeals.TryGetValue(key, out var pending)
            && pending.TryComplete(sealedRecord))
            return;
        if (_sessionRelocationSealReplyTerminals.TryGetValue(
                key,
                out var terminal)
            && terminal == sealedRecord)
            return;
        Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
    }

    private void ProcessSessionRelocationRoute(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.SessionRelocationRouteRecord route)
    {
        ISessionRelocationBarrierTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _sessionRelocationBarrierTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        var expectedNodeGeneration = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.Route.TargetNodeGeneration
                : route.Coordinator.NodeGeneration;
        var expectedAuthorityOwnerGeneration = route.Route.Action
            == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
                ? route.Route.TargetAuthorityOwnerGeneration
                : route.Route.CurrentAuthorityOwnerGeneration;
        var authenticated = target is not null
                            && peer is not null
                            && peer.Admitted
                            && route.Session.SessionOwnerNodeRid == _routingId
                            && route.Session.SessionOwnerNodeGeneration
                            == _lifecycleGeneration
                            && (route.Route.Action
                                    == ZLinkServiceWireCodec
                                        .SessionRelocationRouteAction.Commit
                                ? route.SenderRole == 2
                                  && route.Route.TargetNodeRid == sourceNodeRid
                                  && route.Route.TargetNodeGeneration
                                  == peer.LifecycleGeneration
                                : route.SenderRole == 1
                                  && route.Coordinator.NodeRid == sourceNodeRid
                                  && route.Coordinator.NodeGeneration
                                  == peer.LifecycleGeneration);
        if (!authenticated)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(async () =>
        {
            try
            {
                await target!.RouteAsync(
                        route,
                        new ZLinkSessionRelocationAuthenticatedRoute(
                            sourceNodeRid,
                            expectedNodeGeneration,
                            _meshName,
                            expectedAuthorityOwnerGeneration,
                            0),
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                ZLinkFrameworkDebugLog.TaskFailure(
                    "session-relocation-route",
                    exception);
                Publish(MeshMonitorEventKind.ProtocolError,
                    peerRid: sourceNodeRid);
            }
        });
    }

    private void ProcessInstanceSpotActivation(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.InstanceSpotActivationRecord record,
        Received received)
    {
        Peer? peer;
        IInstanceSpotActivationTarget? target;
        lock (_gate)
        {
            _peersByRid.TryGetValue(sourceRid, out peer);
            target = _instanceSpotActivationTarget;
        }

        var operation = record.Operation;
        var request = operation.IsRequest;
        if (request
            && (received.MessageType != ReceivedMessageType.Request
                || received.RequestSeq is null))
        {
            // Command 39 requests own a Core request window. Accepting a raw
            // envelope would put the terminal back on the Application path.
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return;
        }
        var nativeReply = request ? received.Reply() : null;
        ReplySubmitOperation? preparedReply = null;
        Message[]? preparedReplyWire = null;
        var replied = 0;
        SubmitResult Reply(
            RequestResult result,
            uint failureCode,
            IReadOnlyList<Message> replyParts)
        {
            if (!request
                || Interlocked.CompareExchange(ref replied, 1, 0) != 0)
                return SubmitResult.InvalidState;
            if (preparedReply is null)
                (preparedReply, preparedReplyWire) = PrepareNativeTerminalReply(
                    sourceRid,
                    nativeReply!,
                    operation.ReplyRouteId,
                    result,
                    failureCode,
                    replyParts);
            var submit = SubmitNativeTerminalReply(preparedReply);
            if (submit == SubmitResult.Backpressured)
            {
                Volatile.Write(ref replied, 0);
                return submit;
            }

            if (preparedReplyWire is not null)
            {
                ZLinkMessageParts.DisposeAll(preparedReplyWire);
                preparedReplyWire = null;
            }
            return submit;
        }
        if (peer is null
            || !peer.Admitted
            || (operation.SourceNodeRid == sourceRid
                && operation.SourceNodeGeneration != peer.LifecycleGeneration)
            || operation.Target.TargetNodeRid != _routingId
            || operation.Target.TargetNodeGeneration != _lifecycleGeneration)
        {
            if (request)
                Reply(
                    RequestResult.Conflict,
                    (uint)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale,
                    Array.Empty<Message>());
            return;
        }

        var payloadOffset = record.HasMetadata ? 2 : 1;
        if (received.Parts.Count != payloadOffset + 1 || target is null)
        {
            if (request)
                //  Schema terminal-failure-integrity: requestFailed(17) pairs
                //  only with internalError(105); a boundary terminal cannot
                //  carry a fine code.
                Reply(
                    target is null ? RequestResult.InternalError : RequestResult.ProtocolError,
                    (uint)(target is null
                        ? ServiceWireConstants.FrameworkErrorCode.RequestFailed
                        : ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
                    Array.Empty<Message>());
            return;
        }

        if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                received.Parts[payloadOffset].AsReadOnlyMemory(),
                out var decodedPayload))
        {
            if (request)
                Reply(
                    RequestResult.ProtocolError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
            return;
        }
        var metadata = record.HasMetadata
            ? received.Parts[1].ToArray()
            : (ReadOnlyMemory<byte>?)null;
        var payload = decodedPayload
            .Select(static part => (ReadOnlyMemory<byte>)part.ToArray())
            .ToArray();
        DisposeParts(decodedPayload);
        RunInboundOperation(
            () => CompleteInstanceSpotActivationAsync(
                operation,
                target,
                metadata,
                payload,
                Reply));
    }

    private async Task CompleteInstanceSpotActivationAsync(
        InstanceSpotActivationOperation operation,
        IInstanceSpotActivationTarget target,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        Func<RequestResult, uint, IReadOnlyList<Message>, SubmitResult> reply)
    {
        var remaining = checked((long)operation.DeadlineUnixMs)
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        InstanceSpotActivationTerminal terminal;
        if (remaining <= 0)
        {
            terminal = new InstanceSpotActivationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                Array.Empty<ReadOnlyMemory<byte>>());
        }
        else
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
                _stop?.Token ?? CancellationToken.None);
            deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
            try
            {
                terminal = await target.ActivateAsync(
                        operation, metadata, payload, deadline.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (deadline.IsCancellationRequested)
            {
                terminal = new InstanceSpotActivationTerminal(
                    RequestResult.InternalError,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                    Array.Empty<ReadOnlyMemory<byte>>());
            }
            catch (ZLinkFrameworkException framework)
                when (framework.Kind == ZLinkFrameworkErrorKind.CapacityExceeded)
            {
                //  Spec 32-framework-error-model:104-108 — placement/admission
                //  capacity is CapacityExceeded, encoded as Backpressured(113).
                terminal = new InstanceSpotActivationTerminal(
                    RequestResult.Backpressured,
                    ServiceWireConstants.FrameworkErrorCode.None,
                    Array.Empty<ReadOnlyMemory<byte>>());
            }
            catch
            {
                terminal = new InstanceSpotActivationTerminal(
                    RequestResult.InternalError,
                    ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                    Array.Empty<ReadOnlyMemory<byte>>());
            }
        }

        if (!operation.IsRequest || terminal.Forwarded) return;
        var replyMessages = terminal.ReplyParts.Select(Message.From).ToArray();
        try
        {
            reply(
                terminal.Result,
                (uint)terminal.FailureCode,
                replyMessages);
        }
        finally
        {
            DisposeParts(replyMessages);
        }
    }

    private bool ProcessStateful(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.StatefulRecord stateful,
        RawIngressOwnership ownership)
    {
        var received = ownership.Receipt;
        lock (_gate)
            if (!_peersByRid.TryGetValue(sourceRid, out var peer)
                || !peer.Admitted)
            {
                //  A request dropped here never reaches any staleness check and
                //  simply times out at the caller.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"stateful_dropped reason=peer_not_admitted actor={stateful.TargetActor}");
                return false;
            }
        var request = stateful.Command is ServiceWireConstants.Command.SpotRequest
            or ServiceWireConstants.Command.ActorRequest;
        if (request
            && (received.MessageType != ReceivedMessageType.Request
                || received.RequestSeq is null))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return false;
        }
        var requestSeq = received.RequestSeq ?? 0;
        var replied = 0;
        SubmitResult Reply(
            RequestResult result,
            uint failureCode,
            IReadOnlyList<Message> replyParts)
        {
            if (!request
                || Interlocked.CompareExchange(ref replied, 1, 0) != 0)
                return SubmitResult.InvalidState;
            var submit = SendNativeTerminalReply(
                sourceRid,
                requestSeq,
                stateful.Correlation,
                result,
                failureCode,
                replyParts);
            if (submit == SubmitResult.Backpressured)
                Volatile.Write(ref replied, 0);
            return submit;
        }
        if (stateful.TargetNodeRid != _routingId
            || stateful.TargetNodeGeneration != _lifecycleGeneration)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"stateful_route_rejected reason=node_generation "
                + $"source={sourceRid} target={stateful.TargetNodeRid} "
                + $"wire_node_gen={stateful.TargetNodeGeneration} "
                + $"local_node_gen={_lifecycleGeneration}");
            if (request)
                Reply(
                    RequestResult.Conflict,
                    (uint)ServiceWireConstants.FrameworkErrorCode.ActorLocationStale,
                    Array.Empty<Message>());
            return false;
        }
        if (request
            && stateful.DeadlineUnixMs
               <= checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
        {
            Reply(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                Array.Empty<Message>());
            return false;
        }
        var payloadOffset = stateful.HasMetadata ? 2 : 1;
        if (received.Parts.Count != payloadOffset + 1)
        {
            if (request)
                Reply(
                    RequestResult.ProtocolError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
            return false;
        }
        MailboxKey owner;
        MeshRecordKind kind;
        MeshOperationKind operationKind;
        ActorRef sourceActor = default;
        string sourceSpotId = stateful.SourceSpotId;
        ManagedActor? admittedActor = null;
        if (stateful.Command is ServiceWireConstants.Command.SpotSend
            or ServiceWireConstants.Command.SpotRequest)
        {
            var hasTargetSpot = _spots.TryGetValue(
                ZLinkSpotId.FromBoundary(
                    stateful.TargetSpotId,
                    nameof(stateful.TargetSpotId)),
                out var spot);
            var localOwnerLeaseGeneration = checked((ulong)Volatile.Read(
                ref _localOwnerLeaseGeneration));
            if (!hasTargetSpot
                || spot!.LifecycleGeneration != stateful.TargetSpotGeneration
                || spot.AuthorityOwnerGeneration
                    != stateful.AuthorityOwnerGeneration
                || stateful.OwnerLeaseGeneration != localOwnerLeaseGeneration)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"stateful_spot_rejected source={sourceRid} "
                    + $"target_node={stateful.TargetNodeRid} "
                    + $"spot={stateful.TargetSpotId} "
                    + $"wire_spot_gen={stateful.TargetSpotGeneration} "
                    + $"wire_authority_gen={stateful.AuthorityOwnerGeneration} "
                    + $"wire_lease_gen={stateful.OwnerLeaseGeneration} "
                    + $"has_spot={hasTargetSpot} "
                    + $"local_spot_gen={(hasTargetSpot ? spot!.LifecycleGeneration : 0)} "
                    + $"local_authority_gen={(hasTargetSpot ? spot!.AuthorityOwnerGeneration : 0)} "
                    + $"local_lease_gen={localOwnerLeaseGeneration}");
                if (request)
                {
                    //  Schema terminal-failure-integrity: a genuinely missing
                    //  Spot is spotRouteNotFound(6) which pairs with
                    //  notFound(102); a present-but-stale Spot (generation or
                    //  lease mismatch) is spotGenerationStale(33) which pairs
                    //  with conflict(107).
                    if (hasTargetSpot)
                        Reply(
                            RequestResult.Conflict,
                            (uint)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale,
                            Array.Empty<Message>());
                    else
                        Reply(
                            RequestResult.NotFound,
                            (uint)ServiceWireConstants.FrameworkErrorCode.SpotRouteNotFound,
                            Array.Empty<Message>());
                }
                return false;
            }
            owner = MailboxKey.ForSpot(spot, MeshReadyDomains.Application);
            kind = request ? MeshRecordKind.SpotRequest : MeshRecordKind.SpotSend;
            operationKind = MeshOperationKind.SpotRequest;
        }
        else
        {
            var actorPresent = TryGetActor(stateful.TargetActor, out var actor);
            if (!actorPresent
                || actor.AuthorityOwnerGeneration
                    != stateful.AuthorityOwnerGeneration
                || stateful.OwnerLeaseGeneration
                    != checked((ulong)Volatile.Read(
                        ref _localOwnerLeaseGeneration)))
            {
                IActorMessageFollowIngressTarget? messageFollowTarget;
                lock (_gate)
                    messageFollowTarget = _actorMessageFollowIngressTarget;
                Func<IReadOnlyList<Message>, SendFlags, SubmitResult>?
                    messageFollowReply = request
                        ? (replyParts, _) =>
                            Reply(
                                RequestResult.Ok,
                                0,
                                replyParts)
                        : null;
                var followed = messageFollowTarget?.TryFollow(
                    new ActorMessageFollowIngress(
                            sourceRid,
                            ResolvePeerGeneration(sourceRid),
                            sourceSpotId,
                            stateful.TargetActor,
                            stateful.OperationId,
                            stateful.Correlation,
                            stateful.TargetNodeGeneration,
                            stateful.AuthorityOwnerGeneration,
                            stateful.OwnerLeaseGeneration,
                            stateful.MessageFollowHopCount,
                            stateful.DeadlineUnixMs,
                            ReadOnlyMemory<byte>.Empty,
                            Array.Empty<Message>(),
                            messageFollowReply)
                    {
                        EncodedPayload = received.Parts[payloadOffset]
                            .AsReadOnlyMemory(),
                        ApplicationMetadataSource = stateful.HasMetadata
                            ? received.Parts[1]
                            : null
                    });
                //  Follow taking responsibility without replying and the stale
                //  reply below are indistinguishable from the caller's side.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor_stale_path actor={stateful.TargetActor} "
                    + $"followed={followed} has_follow_target={messageFollowTarget is not null} "
                    + $"request={request}");
                if (followed == true)
                    return false;
                //  Spec 06 §13.1 separates two outcomes. No Actor incarnation
                //  under this reference means the authority is gone, which is
                //  `NotFound`. An incarnation that is here under a different
                //  authority or owner lease generation is a stale reference the
                //  caller can re-resolve, which stays `ActorLocationStale`. A
                //  relocation in flight is carried by the follow target above,
                //  so reaching here with no Actor is a real absence.
                if (request)
                    Reply(
                        actorPresent ? RequestResult.Conflict : RequestResult.NotFound,
                        (uint)(actorPresent
                            ? ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                            : ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound),
                        Array.Empty<Message>());
                return false;
            }
            admittedActor = actor;
            owner = MailboxKey.ForActor(actor, MeshReadyDomains.Application);
            kind = request ? MeshRecordKind.ActorRequest : MeshRecordKind.ActorSend;
            operationKind = MeshOperationKind.ActorRequest;
        }

        var metadata = stateful.HasMetadata
            ? received.Parts[1].ToArray()
            : null;
        if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                received.Parts[payloadOffset].AsReadOnlyMemory(),
                out var parts))
        {
            if (request)
                Reply(
                    RequestResult.ProtocolError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
            return false;
        }
        if (stateful.Command is not (ServiceWireConstants.Command.SpotSend
            or ServiceWireConstants.Command.SpotRequest))
            admittedActor!.NextSequence();

        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null;
        if (request)
            reply = (replyParts, _) =>
                Reply(
                    RequestResult.Ok,
                    0,
                    replyParts);
        return EnqueueOwned(
            owner,
            new MeshReceiveRecord(
                kind,
                MeshReadyDomains.Application,
                sourceRid,
                sourceSpotId,
                ResolvePeerGeneration(sourceRid),
                sourceActor,
                stateful.OperationId,
                request ? operationKind : default,
                null,
                null,
                metadata,
                0,
                parts.Length,
                0,
                0,
                null,
                reply,
                targetNodeGeneration: stateful.TargetNodeGeneration,
                authorityOwnerGeneration: stateful.AuthorityOwnerGeneration,
                ownerLeaseGeneration: stateful.OwnerLeaseGeneration,
                messageFollowHopCount: stateful.MessageFollowHopCount,
                replyRouteId: stateful.Correlation,
                deadlineUnixMs: stateful.DeadlineUnixMs),
            parts,
            true,
            ownership.TakeApplicationOwner());
    }

    private void ProcessUserSpotOperation(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.UserSpotOperationRecord record)
    {
        Peer? peer;
        IUserSpotOperationTarget? target;
        lock (_gate)
        {
            _peersByRid.TryGetValue(sourceRid, out peer);
            target = _userSpotOperationTarget;
        }

        var operation = record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? record.Create.OperationId
            : record.Close.OperationId;
        var correlation = record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? record.Create.Correlation
            : record.Close.Correlation;
        var sourceNodeRid = record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? record.Create.SourceNodeRid
            : record.Close.SourceNodeRid;
        var sourceNodeGeneration =
            record.Command == ServiceWireConstants.Command.UserSpotCreate
                ? record.Create.SourceNodeGeneration
                : record.Close.SourceNodeGeneration;
        var deadlineUnixMs = record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? record.Create.DeadlineUnixMs
            : record.Close.DeadlineUnixMs;
        var targetNodeRid = record.Command == ServiceWireConstants.Command.UserSpotCreate
            ? record.Create.Reservation.TargetNodeRid
            : record.Close.Target.TargetNodeRid;
        var targetNodeGeneration =
            record.Command == ServiceWireConstants.Command.UserSpotCreate
                ? record.Create.Reservation.TargetNodeGeneration
                : record.Close.Target.TargetNodeGeneration;

        if (peer is null
            || !peer.Admitted
            || sourceNodeRid != sourceRid
            || sourceNodeGeneration != peer.LifecycleGeneration)
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
            return;
        }
        if (targetNodeRid != _routingId
            || targetNodeGeneration != _lifecycleGeneration)
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.Conflict,
                ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale);
            return;
        }
        var key = new RemoteUserSpotOperationKey(
            sourceRid,
            sourceNodeGeneration,
            operation);
        if (_remoteUserSpotOperations.TryGetValue(key, out var retained))
        {
            if (!SameUserSpotOperation(retained.Record, record))
            {
                SendUserSpotFailure(
                    sourceRid,
                    correlation,
                    record.Command,
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
                return;
            }
            RunInboundOperation(
                () => ReplyUserSpotOperationAsync(
                    sourceNodeRid,
                    correlation,
                    record.Command,
                    key,
                    retained));
            return;
        }
        if (deadlineUnixMs <= checked((ulong)_deadlineClock.GetUnixTimeMilliseconds()))
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
            return;
        }
        if (target is null)
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed);
            return;
        }

        RemoteUserSpotInvocation invocation;
        var candidate = new RemoteUserSpotInvocation(
            record,
            () => ExecuteUserSpotOperationAsync(target, record, deadlineUnixMs));
        lock (_remoteUserSpotGate)
        {
            if (!_remoteUserSpotOperations.TryGetValue(key, out invocation!))
            {
                if (_remoteUserSpotOperations.Count
                    >= MaxRemoteUserSpotOperations)
                {
                    SendUserSpotFailure(
                        sourceRid,
                        correlation,
                        record.Command,
                        RequestResult.Rejected,
                        ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull);
                    return;
                }
                _remoteUserSpotOperations[key] = candidate;
                invocation = candidate;
            }
        }
        if (!ReferenceEquals(invocation, candidate))
        {
            if (!SameUserSpotOperation(invocation.Record, record))
            {
                SendUserSpotFailure(
                    sourceRid,
                    correlation,
                    record.Command,
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
                return;
            }
        }

        RunInboundOperation(
            () => ReplyUserSpotOperationAsync(
                sourceNodeRid, correlation, record.Command, key, invocation));
    }

    private async Task<UserSpotOperationTerminal> ExecuteUserSpotOperationAsync(
        IUserSpotOperationTarget target,
        ZLinkServiceWireCodec.UserSpotOperationRecord record,
        ulong deadlineUnixMs)
    {
        var remaining = checked((long)deadlineUnixMs)
                        - _deadlineClock.GetUnixTimeMilliseconds();
        if (remaining <= 0)
            return new UserSpotOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            _stop?.Token ?? CancellationToken.None);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
        try
        {
            var terminal = record.Command == ServiceWireConstants.Command.UserSpotCreate
                ? await target.CreateAsync(record.Create, deadline.Token).ConfigureAwait(false)
                : await target.CloseAsync(record.Close, deadline.Token).ConfigureAwait(false);
            return ValidateUserSpotTerminal(record, terminal);
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested)
        {
            return new UserSpotOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
        }
        catch (ZLinkFrameworkException exception)
        {
            return MapUserSpotException(exception);
        }
        catch
        {
            return new UserSpotOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed);
        }
    }

    private static bool SameUserSpotOperation(
        ZLinkServiceWireCodec.UserSpotOperationRecord left,
        ZLinkServiceWireCodec.UserSpotOperationRecord right)
    {
        if (left.Command != right.Command) return false;
        return left.Command == ServiceWireConstants.Command.UserSpotCreate
            ? left.Create with { Correlation = 0 }
                == right.Create with { Correlation = 0 }
            : left.Close with { Correlation = 0 }
                == right.Close with { Correlation = 0 };
    }

    private static UserSpotOperationTerminal ValidateUserSpotTerminal(
        ZLinkServiceWireCodec.UserSpotOperationRecord operation,
        UserSpotOperationTerminal terminal)
    {
        if (terminal.Result != RequestResult.Ok)
        {
            if (terminal.Completion is not null
                || (terminal.FailureCode == ServiceWireConstants.FrameworkErrorCode.None
                    && terminal.Result != RequestResult.Backpressured))
                throw new InvalidOperationException(
                    "A failed User Spot operation requires one failure code (or the "
                    + "Backpressured admission terminal) and no success completion.");
            if (terminal.FailureCode is ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                or ServiceWireConstants.FrameworkErrorCode.SpotMoving
                && terminal.Result != RequestResult.Conflict)
                throw new InvalidOperationException(
                    "Stale-generation and moving failures map to conflict.");
            return terminal;
        }
        if (terminal.FailureCode != ServiceWireConstants.FrameworkErrorCode.None)
            throw new InvalidOperationException(
                "A successful User Spot operation cannot carry a failure code.");

        if (operation.Command == ServiceWireConstants.Command.UserSpotCreate)
        {
            if (terminal.Completion is not UserSpotCreateCompletion create
                || create.SpotId != operation.Create.SpotId
                || create.ObjectGeneration != operation.Create.Reservation.ObjectGeneration)
                throw new InvalidOperationException(
                    "The User Spot create completion does not match its reservation.");
        }
        else if (terminal.Completion is not UserSpotCloseCompletion)
        {
            throw new InvalidOperationException(
                "The User Spot close completion is missing.");
        }
        return terminal;
    }

    private async Task ReplyUserSpotOperationAsync(
        RoutingId sourceRid,
        ulong correlation,
        ServiceWireConstants.Command command,
        RemoteUserSpotOperationKey key,
        RemoteUserSpotInvocation invocation)
    {
        var terminal = await invocation.Task.ConfigureAwait(false);
        var head = command == ServiceWireConstants.Command.UserSpotCreate
            ? ZLinkServiceWireCodec.EncodeUserSpotCreateReply(
                correlation,
                terminal.Result,
                terminal.FailureCode,
                terminal.Completion as UserSpotCreateCompletion)
            : ZLinkServiceWireCodec.EncodeUserSpotCloseReply(
                correlation,
                terminal.Result,
                terminal.FailureCode,
                terminal.Completion as UserSpotCloseCompletion);
        var replyParts = ReencodeActorCreateReply(
            terminal.ReplyParts,
            correlation);
        var wire = new List<ReadOnlyMemory<byte>>(replyParts.Count == 0 ? 1 : 2) { head };
        if (replyParts.Count != 0)
            wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(replyParts));
        await SendServiceTerminalAsync(sourceRid, wire).ConfigureAwait(false);
        _ = ExpireRemoteUserSpotOperationAsync(key, invocation);
    }

    private static IReadOnlyList<ReadOnlyMemory<byte>> ReencodeActorCreateReply(
        IReadOnlyList<ReadOnlyMemory<byte>>? replyParts,
        ulong correlation)
    {
        if (replyParts is not { Count: > 0 })
            return Array.Empty<ReadOnlyMemory<byte>>();
        var messages = replyParts
            .Select(static part => Message.From(part.Span))
            .ToArray();
        try
        {
            ZLinkEnvelopeHeader header;
            try
            {
                header = ZLinkEnvelopeCodec.DecodeHeader(messages);
            }
            catch (ZLinkEnvelopeProtocolException)
            {
                // Raw terminal payloads carry no envelope header, so there is no
                // correlation to rewrite. They travel unchanged.
                return replyParts;
            }
            using var encodedHeader = ZLinkEnvelopeCodec.EncodeHeader(
                header with
                {
                    CorrelationId = correlation.ToString(
                        System.Globalization.CultureInfo.InvariantCulture)
                });
            var result = new ReadOnlyMemory<byte>[replyParts.Count];
            result[0] = encodedHeader.AsReadOnlyMemory().ToArray();
            for (var i = 1; i < replyParts.Count; i++)
                result[i] = replyParts[i];
            return result;
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(messages);
        }
    }

    private async Task<bool> SendServiceTerminalAsync(
        RoutingId sourceRid,
        IReadOnlyList<ReadOnlyMemory<byte>> wire)
    {
        var stopToken = _stop?.Token ?? CancellationToken.None;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            stopToken);
        deadline.CancelAfter(ServiceTerminalRetryTimeout);
        RoutingId target;
        lock (_gate)
            target = _peersByRid.TryGetValue(sourceRid, out var peer)
                && peer.Admitted
                ? peer.PhysicalRoutingId
                : sourceRid;

        try
        {
            await SendRoutedAsync(target, wire, deadline.Token)
                .ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch (ObjectDisposedException)
        {
            return false;
        }
        catch (ZlinkException)
        {
            return false;
        }
    }

    private async Task ExpireRemoteUserSpotOperationAsync(
        RemoteUserSpotOperationKey key,
        RemoteUserSpotInvocation invocation)
    {
        try
        {
            var deadline = invocation.Record.Command
                == ServiceWireConstants.Command.UserSpotCreate
                    ? invocation.Record.Create.DeadlineUnixMs
                    : invocation.Record.Close.DeadlineUnixMs;
            var retentionDeadline = checked(
                (long)deadline
                + (long)_remoteUserSpotTerminalRetention.TotalMilliseconds);
            await DelayUntilRemoteOperationExpiryAsync(
                    retentionDeadline,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        _remoteUserSpotOperations.TryRemove(
            new KeyValuePair<RemoteUserSpotOperationKey, RemoteUserSpotInvocation>(
                key, invocation));
    }

    private void SendUserSpotFailure(
        RoutingId sourceRid,
        ulong correlation,
        ServiceWireConstants.Command command,
        RequestResult result,
        ServiceWireConstants.FrameworkErrorCode failureCode)
    {
        var head = command == ServiceWireConstants.Command.UserSpotCreate
            ? ZLinkServiceWireCodec.EncodeUserSpotCreateReply(
                correlation,
                result,
                failureCode,
                null)
            : ZLinkServiceWireCodec.EncodeUserSpotCloseReply(
                correlation,
                result,
                failureCode,
                null);
        RunInboundOperation(async () =>
        {
            if (!await SendServiceTerminalAsync(sourceRid, [head])
                    .ConfigureAwait(false))
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
        });
    }

    private static UserSpotOperationTerminal MapUserSpotException(
        ZLinkFrameworkException exception)
    {
        return exception.Kind switch
        {
            ZLinkFrameworkErrorKind.InvalidOperation =>
                new UserSpotOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale),
            ZLinkFrameworkErrorKind.Unavailable =>
                new UserSpotOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.SpotMoving),
            ZLinkFrameworkErrorKind.TypeMismatch =>
                new UserSpotOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.SpotTypeMismatch),
            ZLinkFrameworkErrorKind.AlreadyExists =>
                new UserSpotOperationTerminal(
                    RequestResult.InternalError,
                    ServiceWireConstants.FrameworkErrorCode.SpotCreateFailed),
            //  Spec 32-framework-error-model:104-108 — target placement/admission
            //  capacity is CapacityExceeded, wire-encoded as the bounded-admission
            //  terminal Backpressured(113) with no fine code (distinct from a
            //  genuine remote queue/table saturation, which stays Busy+WorkerQueueFull).
            ZLinkFrameworkErrorKind.CapacityExceeded =>
                new UserSpotOperationTerminal(
                    RequestResult.Backpressured,
                    ServiceWireConstants.FrameworkErrorCode.None),
            ZLinkFrameworkErrorKind.ProtocolError =>
                new UserSpotOperationTerminal(
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
            //  Schema terminal-failure-integrity: requestFailed(17) pairs
            //  only with internalError(105); the retry hint is conveyed by the
            //  fine code's public classification, not the wire terminal.
            _ => new UserSpotOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed)
        };
    }

    private void ProcessActorCreateOperation(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.ActorCreateOperationRecord record)
    {
        Peer? peer;
        IActorCreateOperationTarget? target;
        lock (_gate)
        {
            _peersByRid.TryGetValue(sourceRid, out peer);
            target = _actorCreateOperationTarget;
        }

        var operation = record.Operation;
        if (peer is null || !peer.Admitted)
        {
            // The command crossed the transport before the reverse admission
            // was visible in the target peer table. This is a route-readiness
            // outcome, not a malformed Actor reservation; the source must be
            // allowed to abort and retry the reservation within its deadline.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_create_rejected reason=peer_not_admitted source={sourceRid} "
                + $"operation={operation.OperationId}");
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RouteNotConnected);
            return;
        }
        if (operation.SourceNodeRid != sourceRid
            || operation.SourceNodeGeneration != peer.LifecycleGeneration)
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
            return;
        }
        if (operation.Reservation.TargetNodeRid != _routingId
            || operation.Reservation.TargetNodeGeneration != _lifecycleGeneration)
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.Conflict,
                ServiceWireConstants.FrameworkErrorCode.ActorLocationStale);
            return;
        }

        var key = new RemoteActorCreateOperationKey(
            sourceRid,
            operation.SourceNodeGeneration,
            operation.OperationId);
        if (_remoteActorCreateOperations.TryGetValue(key, out var retained))
        {
            if (!SameActorCreateOperation(retained.Record, record))
            {
                SendActorCreateFailure(
                    sourceRid,
                    operation.Correlation,
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
                return;
            }
            RunInboundOperation(
                () => ReplyActorCreateOperationAsync(
                    operation.SourceNodeRid,
                    operation.Correlation,
                    key,
                    retained));
            return;
        }
        if (operation.DeadlineUnixMs
            <= checked((ulong)_deadlineClock.GetUnixTimeMilliseconds()))
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
            return;
        }
        if (target is null)
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed);
            return;
        }

        RemoteActorCreateInvocation invocation;
        var candidate = new RemoteActorCreateInvocation(
            record,
            () => ExecuteActorCreateOperationAsync(
                target,
                operation,
                operation.DeadlineUnixMs));
        lock (_remoteActorCreateGate)
        {
            if (!_remoteActorCreateOperations.TryGetValue(key, out invocation!))
            {
                if (_remoteActorCreateOperations.Count
                    >= MaxRemoteActorCreateOperations)
                {
                    SendActorCreateFailure(
                        sourceRid,
                        operation.Correlation,
                        RequestResult.Rejected,
                        ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull);
                    return;
                }
                _remoteActorCreateOperations[key] = candidate;
                invocation = candidate;
            }
        }
        if (!ReferenceEquals(invocation, candidate)
            && !SameActorCreateOperation(invocation.Record, record))
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
            return;
        }

        RunInboundOperation(
            () => ReplyActorCreateOperationAsync(
                operation.SourceNodeRid,
                operation.Correlation,
                key,
                invocation));
    }

    private void ProcessActorDestroyOperation(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.ActorDestroyOperationRecord record,
        int partCount)
    {
        Peer? peer;
        IActorDestroyOperationTarget? target;
        lock (_gate)
        {
            _peersByRid.TryGetValue(sourceRid, out peer);
            target = _actorDestroyOperationTarget;
        }
        var operation = record.Operation;
        if (partCount != 1)
        {
            SendActorDestroyFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.ProtocolError,
                ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
            return;
        }
        if (peer is null
            || !peer.Admitted
            || operation.TargetNodeRid != _routingId
            || operation.TargetNodeGeneration != _lifecycleGeneration)
        {
            SendActorDestroyFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.Conflict,
                ServiceWireConstants.FrameworkErrorCode.ActorLocationStale);
            return;
        }
        if (target is null)
        {
            SendActorDestroyFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.InvalidState,
                ServiceWireConstants.FrameworkErrorCode.None);
            return;
        }
        RunInboundOperation(
            () => ReplyActorDestroyOperationAsync(
                sourceRid,
                operation.Correlation,
                target,
                operation));
    }

    private async Task ReplyActorDestroyOperationAsync(
        RoutingId sourceRid,
        ulong correlation,
        IActorDestroyOperationTarget target,
        ActorDestroyOperation operation)
    {
        ActorDestroyOperationTerminal terminal;
        try
        {
            terminal = await target.DestroyAsync(
                    operation,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException exception)
        {
            terminal = exception.Kind switch
            {
                ZLinkFrameworkErrorKind.NotFound =>
                    new ActorDestroyOperationTerminal(
                        RequestResult.NotFound,
                        ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound),
                _ => new ActorDestroyOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.ActorLocationStale)
            };
        }
        catch (OperationCanceledException)
        {
            terminal = new ActorDestroyOperationTerminal(
                RequestResult.Terminated,
                ServiceWireConstants.FrameworkErrorCode.None);
        }
        catch
        {
            terminal = new ActorDestroyOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.RequestFailed);
        }
        var head = ZLinkServiceWireCodec.EncodeActorDestroyReply(
            correlation,
            terminal.Result,
            terminal.FailureCode,
            terminal.Completion);
        await SendServiceTerminalAsync(sourceRid, [head]).ConfigureAwait(false);
    }

    private void SendActorDestroyFailure(
        RoutingId sourceRid,
        ulong correlation,
        RequestResult result,
        ServiceWireConstants.FrameworkErrorCode failure)
    {
        var head = ZLinkServiceWireCodec.EncodeActorDestroyReply(
            correlation,
            result,
            failure,
            null);
        RunInboundOperation(async () =>
        {
            if (!await SendServiceTerminalAsync(sourceRid, [head])
                    .ConfigureAwait(false))
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
        });
    }

    private async Task<ActorCreateOperationTerminal> ExecuteActorCreateOperationAsync(
        IActorCreateOperationTarget target,
        ActorCreateOperation operation,
        ulong deadlineUnixMs)
    {
        var remaining = checked((long)deadlineUnixMs)
                        - _deadlineClock.GetUnixTimeMilliseconds();
        if (remaining <= 0)
            return new ActorCreateOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            _stop?.Token ?? CancellationToken.None);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
        try
        {
            var terminal = await target.CreateAsync(operation, deadline.Token)
                .ConfigureAwait(false);
            return ValidateActorCreateTerminal(operation, terminal);
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested)
        {
            return new ActorCreateOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
        }
        catch (ZLinkFrameworkException exception)
        {
            return MapActorCreateException(exception);
        }
        catch
        {
            return new ActorCreateOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.ActorCreateFailed);
        }
    }

    private static bool SameActorCreateOperation(
        ZLinkServiceWireCodec.ActorCreateOperationRecord left,
        ZLinkServiceWireCodec.ActorCreateOperationRecord right) =>
        left.Operation with { Correlation = 0 }
        == right.Operation with { Correlation = 0 };

    private ActorCreateOperationTerminal ValidateActorCreateTerminal(
        ActorCreateOperation operation,
        ActorCreateOperationTerminal terminal)
    {
        if (terminal.Result != RequestResult.Ok)
        {
            if (terminal.Completion is not null
                || (terminal.FailureCode == ServiceWireConstants.FrameworkErrorCode.None
                    && terminal.Result != RequestResult.Backpressured))
                throw new InvalidOperationException(
                    "A failed Actor create operation requires one failure code (or the "
                    + "Backpressured admission terminal) and no success completion.");
            return terminal;
        }
        if (terminal.FailureCode != ServiceWireConstants.FrameworkErrorCode.None
            || terminal.Completion is not { } completion)
            throw new InvalidOperationException(
                "A successful Actor create operation requires one completion and no failure code.");

        if (completion.Result is ActorCreateResult.Existing
            or ActorCreateResult.Created)
        {
            if (completion.Actor.ActorId != operation.ActorId
                || completion.Actor.ObjectGeneration
                    != operation.Reservation.ObjectGeneration
                || !string.Equals(
                    completion.Actor.MeshName,
                    _meshName,
                    StringComparison.Ordinal)
                || completion.Actor.NodeRid
                    != operation.Reservation.TargetNodeRid)
                throw new InvalidOperationException(
                    "The Actor create completion does not match its reservation.");
        }
        else if (!string.IsNullOrEmpty(completion.Actor.ActorId)
                 || completion.Actor.ObjectGeneration != 0
                 || !string.IsNullOrEmpty(completion.Actor.MeshName)
                 || !completion.Actor.NodeRid.IsEmpty)
        {
            throw new InvalidOperationException(
                "A rejected Actor create completion cannot select an Actor.");
        }
        return terminal;
    }

    private async Task ReplyActorCreateOperationAsync(
        RoutingId sourceRid,
        ulong correlation,
        RemoteActorCreateOperationKey key,
        RemoteActorCreateInvocation invocation)
    {
        var terminal = await invocation.Task.ConfigureAwait(false);
        var head = ZLinkServiceWireCodec.EncodeActorCreateReply(
            correlation,
            terminal.Result,
            terminal.FailureCode,
            terminal.Completion);
        var replyParts = terminal.ReplyParts ?? Array.Empty<ReadOnlyMemory<byte>>();
        var wire = new List<ReadOnlyMemory<byte>>(replyParts.Count == 0 ? 1 : 2) { head };
        if (replyParts.Count != 0)
            wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(replyParts));
        await SendServiceTerminalAsync(sourceRid, wire).ConfigureAwait(false);
        _ = ExpireRemoteActorCreateOperationAsync(key, invocation);
    }

    private async Task ExpireRemoteActorCreateOperationAsync(
        RemoteActorCreateOperationKey key,
        RemoteActorCreateInvocation invocation)
    {
        try
        {
            var retentionDeadline = checked(
                (long)invocation.Record.Operation.DeadlineUnixMs
                + (long)_remoteUserSpotTerminalRetention.TotalMilliseconds);
            await DelayUntilRemoteOperationExpiryAsync(
                    retentionDeadline,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        _remoteActorCreateOperations.TryRemove(
            new KeyValuePair<RemoteActorCreateOperationKey, RemoteActorCreateInvocation>(
                key,
                invocation));
    }

    private async Task DelayUntilRemoteOperationExpiryAsync(
        long retentionDeadlineUnixMs,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            var now = _deadlineClock.GetUnixTimeMilliseconds();
            if (now >= retentionDeadlineUnixMs)
                return;

            var remaining = retentionDeadlineUnixMs - now;
            await Task.Delay(
                    TimeSpan.FromMilliseconds(Math.Min(remaining, int.MaxValue)),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private void SendActorCreateFailure(
        RoutingId sourceRid,
        ulong correlation,
        RequestResult result,
        ServiceWireConstants.FrameworkErrorCode failureCode)
    {
        var head = ZLinkServiceWireCodec.EncodeActorCreateReply(
            correlation,
            result,
            failureCode,
            null);
        RunInboundOperation(async () =>
        {
            if (!await SendServiceTerminalAsync(sourceRid, [head])
                    .ConfigureAwait(false))
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
        });
    }

    private static ActorCreateOperationTerminal MapActorCreateException(
        ZLinkFrameworkException exception)
    {
        return exception.Kind switch
        {
            ZLinkFrameworkErrorKind.TypeMismatch =>
                new ActorCreateOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch),
            ZLinkFrameworkErrorKind.AlreadyExists =>
                new ActorCreateOperationTerminal(
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.ActorAlreadyExists),
            //  Spec 32-framework-error-model:104-108 — target placement/admission
            //  capacity is CapacityExceeded, wire-encoded as Backpressured(113)
            //  with no fine code (distinct from remote queue/table saturation).
            ZLinkFrameworkErrorKind.CapacityExceeded =>
                new ActorCreateOperationTerminal(
                    RequestResult.Backpressured,
                    ServiceWireConstants.FrameworkErrorCode.None),
            ZLinkFrameworkErrorKind.ProtocolError =>
                new ActorCreateOperationTerminal(
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
            //  Schema terminal-failure-integrity: actorCreateFailed(2) pairs
            //  only with internalError(105); the retry hint is conveyed by the
            //  fine code's public classification, not the wire terminal.
            _ => new ActorCreateOperationTerminal(
                RequestResult.InternalError,
                ServiceWireConstants.FrameworkErrorCode.ActorCreateFailed)
        };
    }

    private void ProcessAdmission(
        RoutingId sourceRid,
        ServiceWireConstants.Command command,
        ZLinkServiceWireCodec.AdmissionRecord admission)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"mesh_peer_admission_received local={_routingId} peer={sourceRid} "
            + $"command={command} endpoint={admission.AdvertisedEndpoint} "
            + $"lifecycle={admission.LifecycleGeneration} "
            + $"revision={admission.DescriptorRevision}");
        if (!string.Equals(admission.MeshName, _meshName, StringComparison.Ordinal))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"mesh_peer_admission_rejected local={_routingId} peer={sourceRid} "
                + $"reason=mesh_mismatch expected={_meshName} actual={admission.MeshName}");
            Publish(MeshMonitorEventKind.PeerRejected, peerRid: sourceRid);
            return;
        }

        Peer peer;
        ZLinkServiceAdmissionDecision decision;
        lock (_gate)
        {
            var matchedPeer = _peerAdmission.FindForAdmission(
                       _peersByRid,
                       _peersByIntent.Values,
                       sourceRid,
                       command,
                       admission.AdvertisedEndpoint);
            if (matchedPeer is null
                && command != ServiceWireConstants.Command.Hello)
            {
                // An Admit/Update arriving after its intent was removed is a
                // stale transport message. It must not create a new inbound
                // peer, otherwise a retired connection can re-enter the
                // public mesh status during a same-endpoint handover.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"mesh_peer_admission_rejected local={_routingId} peer={sourceRid} "
                    + $"reason=stale_{command.ToString().ToLowerInvariant()} "
                    + $"endpoint={admission.AdvertisedEndpoint}");
                Publish(MeshMonitorEventKind.PeerRejected, peerRid: sourceRid);
                return;
            }

            peer = matchedPeer
                   ?? new Peer(
                       checked(++_nextIntent),
                       admission.AdvertisedEndpoint,
                       sourceRid,
                       _peerExpectations.TryGetValue(
                           sourceRid,
                           out var expectedInbound)
                           ? expectedInbound.SecurityIdentity
                           : ZLinkServiceSecurityIdentity.Plaintext,
                       ZLinkServiceConnectionDirection.Inbound,
                       checked(++_nextPeerConnectionGeneration));
            if (peer.ExpectedRid is { } expected && expected != sourceRid)
            {
                peer.State = MeshPeerState.Error;
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"mesh_peer_admission_rejected local={_routingId} peer={sourceRid} "
                    + $"reason=rid_mismatch expected={expected} actual={sourceRid}");
                Publish(MeshMonitorEventKind.PeerRejected, peerRid: sourceRid);
                return;
            }
            var hasExpectedRoute = _peerExpectations.TryGetValue(
                sourceRid,
                out var expectedRoute);
            if (!ZLinkServiceAdmissionGuard.MatchesExpectedTransportRoute(
                    hasExpectedRoute
                        ? expectedRoute.Endpoint
                        : peer.Endpoint,
                    hasExpectedRoute
                        ? expectedRoute.SecurityIdentity
                        : peer.ExpectedSecurityIdentity,
                    // MeshNode currently uses the plaintext ROUTER transport;
                    // the binding exposes no authenticated TLS peer identity
                    // for this socket, so a non-plaintext expectation cannot
                    // be admitted by trusting the descriptor field.
                    ZLinkServiceSecurityIdentity.Plaintext,
                    hasExpectedRoute
                        ? expectedRoute.LifecycleGeneration
                        : 0,
                    admission))
            {
                peer.State = MeshPeerState.Error;
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"mesh_peer_admission_rejected local={_routingId} peer={sourceRid} "
                    + $"reason=route_mismatch expected_endpoint="
                    + $"{(hasExpectedRoute ? expectedRoute.Endpoint : peer.Endpoint)} "
                    + $"actual_endpoint={admission.AdvertisedEndpoint} "
                    + $"expected_lifecycle="
                    + $"{(hasExpectedRoute ? expectedRoute.LifecycleGeneration : 0)} "
                    + $"actual_lifecycle={admission.LifecycleGeneration}");
                Publish(MeshMonitorEventKind.PeerRejected, peerRid: sourceRid);
                return;
            }

            if (ZLinkRouteMeshConnectionPolicy.IsNotRequired(
                    _objectRole,
                    _channels.Count != 0,
                    (ZLinkMeshNodeObjectRole)admission.ObjectRole,
                    admission.Channels.Count != 0))
            {
                var publishNotRequired =
                    peer.State != MeshPeerState.NotRequired;
                peer.ConnectionGeneration =
                    checked(++_nextPeerConnectionGeneration);
                peer.RoutingId = sourceRid;
                if (peer.Direction == ZLinkServiceConnectionDirection.Inbound
                    || peer.PhysicalRoutingId.IsEmpty)
                    peer.PhysicalRoutingId = sourceRid;
                peer.LifecycleGeneration = admission.LifecycleGeneration;
                peer.DescriptorRevision = admission.DescriptorRevision;
                peer.Admission = admission;
                peer.Admitted = false;
                peer.State = MeshPeerState.NotRequired;
                peer.LastChangedMs = checked((ulong)Environment.TickCount64);
                if (command == ServiceWireConstants.Command.Hello)
                    SendAdmission(peer, ServiceWireConstants.Command.Admit);
                var notRequiredDuplicate =
                    ZLinkMeshPeerAdmission.FindNotRequiredDuplicate(
                        _peersByIntent.Values,
                        peer,
                        sourceRid);
                var keepPeer = peer;
                if (notRequiredDuplicate is not null)
                {
                    if (notRequiredDuplicate.Direction
                        == ZLinkServiceConnectionDirection.Outbound
                        && peer.Direction == ZLinkServiceConnectionDirection.Inbound)
                    {
                        RetireDuplicatePeer(peer, notRequiredDuplicate, sourceRid);
                        keepPeer = notRequiredDuplicate;
                        publishNotRequired = false;
                    }
                    else
                    {
                        RetireDuplicatePeer(notRequiredDuplicate, peer, sourceRid);
                    }
                }
                if (keepPeer.State == MeshPeerState.NotRequired
                    && !_peersByIntent.ContainsKey(keepPeer.Intent))
                    _peersByIntent.Add(keepPeer.Intent, keepPeer);
                if (command == ServiceWireConstants.Command.Admit
                    && keepPeer.Direction == ZLinkServiceConnectionDirection.Outbound
                    && _socket is not null)
                    try
                    {
                        lock (_socketGate)
                            _socket.Disconnect(keepPeer.Endpoint);
                    }
                    catch (ZlinkException)
                    {
                    }
                if (publishNotRequired)
                {
                    RebuildChannelSelectionPlansUnderLock();
                    Publish(MeshMonitorEventKind.PeerNotRequired, peerRid: sourceRid);
                }
                return;
            }

            var duplicate = _peerAdmission.FindDuplicate(
                _peersByRid,
                _peersByIntent.Values,
                sourceRid,
                peer);
            if (duplicate is not null)
            {
                var duplicateDecision = ZLinkServiceAdmissionGuard.SelectConnection(
                    _routingId,
                    sourceRid,
                    duplicate.LifecycleGeneration,
                    duplicate.Direction,
                    duplicate.Discriminator,
                    admission.LifecycleGeneration,
                    peer.Direction,
                    peer.Discriminator);
                if (duplicateDecision
                    == ZLinkServiceDuplicateConnectionDecision.KeepCurrent)
                {
                    RetireDuplicatePeer(peer, duplicate, sourceRid);
                    if (command == ServiceWireConstants.Command.Hello
                        && duplicate.Admitted)
                        SendAdmission(
                            duplicate,
                            ServiceWireConstants.Command.Admit);
                    Publish(
                        MeshMonitorEventKind.PeerRejected,
                        peerRid: sourceRid);
                    return;
                }
                if (duplicateDecision
                    == ZLinkServiceDuplicateConnectionDecision.UseIncoming)
                    RetireDuplicatePeer(duplicate, peer, sourceRid);
            }

            decision = ZLinkServiceAdmissionGuard.Evaluate(
                peer.Admission,
                command,
                admission);
            if (decision == ZLinkServiceAdmissionDecision.Reject)
            {
                if (_peersByIntent.ContainsKey(peer.Intent))
                {
                    peer.State = MeshPeerState.Error;
                    peer.Admitted = false;
                    if (_peersByRid.TryGetValue(sourceRid, out var indexed)
                        && ReferenceEquals(indexed, peer))
                        _peersByRid.Remove(sourceRid);
                    RebuildChannelSelectionPlansUnderLock();
                }
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"mesh_peer_admission_rejected local={_routingId} peer={sourceRid} "
                    + $"reason=guard decision={decision} command={command}");
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                Publish(MeshMonitorEventKind.PeerRejected, peerRid: sourceRid);
                return;
            }
            if (decision == ZLinkServiceAdmissionDecision.Idempotent)
            {
                if (command == ServiceWireConstants.Command.Hello)
                    SendAdmission(peer, ServiceWireConstants.Command.Admit);
                return;
            }
            if (!_peersByIntent.ContainsKey(peer.Intent))
                _peersByIntent.Add(peer.Intent, peer);
            peer.ConnectionGeneration =
                checked(++_nextPeerConnectionGeneration);
            peer.RoutingId = sourceRid;
            if (peer.Direction == ZLinkServiceConnectionDirection.Inbound
                || peer.PhysicalRoutingId.IsEmpty)
                peer.PhysicalRoutingId = sourceRid;
            peer.LifecycleGeneration = admission.LifecycleGeneration;
            peer.DescriptorRevision = admission.DescriptorRevision;
            peer.Channels = admission.Channels;
            peer.Admission = admission;
            //  Peer가 draining을 알려오면 그대로 표시한다. 이 값이 없으면
            //  MeshPeerState.Draining은 어디에서도 대입되지 않고, 그것을 읽는
            //  status·selection·monitoring이 전부 죽은 코드가 된다.
            peer.State = admission.RuntimeState == 2
                ? MeshPeerState.Draining
                : MeshPeerState.Admitted;
            peer.Admitted = true;
            peer.Liveness = new ZLinkServiceLiveness(
                Stopwatch.GetTimestamp(),
                peer.ConnectionGeneration);
            peer.LastChangedMs = checked((ulong)Environment.TickCount64);
            _peersByRid[sourceRid] = peer;
            _state = MeshNodeState.Ready;
            RebuildChannelSelectionPlansUnderLock();
        }

        if (command == ServiceWireConstants.Command.Hello)
            SendAdmission(peer, ServiceWireConstants.Command.Admit);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"mesh_peer_admission_accepted local={_routingId} peer={sourceRid} "
            + $"command={command} endpoint={admission.AdvertisedEndpoint} "
            + $"lifecycle={admission.LifecycleGeneration} "
            + $"revision={admission.DescriptorRevision}");
        Publish(MeshMonitorEventKind.PeerAdmitted, peerRid: sourceRid);
        Publish(MeshMonitorEventKind.StateChanged);
    }

    private void ProcessLiveness(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.LivenessRecord record)
    {
        if (record.Command == ServiceWireConstants.Command.LivenessProbe)
        {
            Peer? peer;
            lock (_gate)
                _peersByRid.TryGetValue(sourceRid, out peer);
            if (peer is null || !peer.Admitted)
            {
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
                return;
            }
            SendControl(
                peer.PhysicalRoutingId,
                peer.ConnectionGeneration,
                ServiceWireConstants.Command.LivenessAck,
                ZLinkServiceWireCodec.EncodeLiveness(
                    ServiceWireConstants.Command.LivenessAck,
                    record.ProbeId));
            return;
        }

        lock (_gate)
            if (_peersByRid.TryGetValue(sourceRid, out var peer))
                _ = peer.Liveness?.Acknowledge(
                    record.ProbeId,
                    Stopwatch.GetTimestamp()) == true;
    }

    private void ProcessInfrastructure(long now)
    {
        DrainSocketMonitorEvents();
        DrainTransportDisconnects(now);
        Peer[] peers;
        lock (_gate)
            peers = _peersByIntent.Values.ToArray();

        foreach (var peer in peers)
        {
            if (peer.State == MeshPeerState.NotRequired)
                continue;
            if (!peer.Admitted)
            {
                if (peer.Direction != ZLinkServiceConnectionDirection.Outbound
                    || peer.State != MeshPeerState.Connecting)
                    continue;
                if (now >= peer.NextAdmissionTimestamp)
                {
                    lock (_gate)
                    {
                        if (!_peersByIntent.TryGetValue(peer.Intent, out var current)
                            || !ReferenceEquals(current, peer)
                            || current.Direction
                                != ZLinkServiceConnectionDirection.Outbound
                            || current.State != MeshPeerState.Connecting)
                            continue;
                        current.NextAdmissionTimestamp =
                            Add(now, AdmissionRetryInterval);
                    }
                    SendAdmission(peer, ServiceWireConstants.Command.Hello);
                }
                continue;
            }
            var liveness = peer.Liveness;
            if (liveness is null)
                continue;
            if (liveness.IsExpired(now))
            {
                lock (_gate)
                {
                    peer.Admitted = false;
                    peer.State = MeshPeerState.Connecting;
                    // A liveness failure ends the current admission epoch. The
                    // next Hello must be evaluated as a fresh admission so the
                    // peer can receive a new liveness deadline.
                    peer.Admission = null;
                    peer.Liveness = null;
                    if (_peersByRid.TryGetValue(peer.RoutingId, out var indexed)
                        && ReferenceEquals(indexed, peer))
                        _peersByRid.Remove(peer.RoutingId);
                    RebuildChannelSelectionPlansUnderLock();
                    peer.NextAdmissionTimestamp = now;
                    _state = _peersByRid.Count == 0
                        ? MeshNodeState.Started
                        : MeshNodeState.PartialReady;
                }
                Publish(MeshMonitorEventKind.PeerClosed, peerRid: peer.RoutingId);
                continue;
            }
            if (liveness.TryGetProbe(now, out var probeId))
            {
                SendControl(
                    peer.PhysicalRoutingId,
                    peer.ConnectionGeneration,
                    ServiceWireConstants.Command.LivenessProbe,
                    ZLinkServiceWireCodec.EncodeLiveness(
                        ServiceWireConstants.Command.LivenessProbe,
                        probeId));
            }
        }

    }

    private void OnSocketMonitorEvent(MonitorEvent value)
    {
        if (value.Event is not (MonitorEventType.Disconnected or MonitorEventType.Closed)
            || value.RoutingId is not { } routingId
            || routingId.IsEmpty)
            return;

        // Monitor callbacks run on the binding dispatch thread. Only enqueue
        // the identity here; peer state remains owned by ReceiveLoop.
        _transportDisconnects.Enqueue(routingId);
    }

    private void DrainSocketMonitorEvents()
    {
        ISocketMonitor? monitor;
        lock (_socketGate)
            monitor = _socketMonitor;
        if (monitor is null)
            return;

        for (var index = 0; index < ReceiveBatchSize; index++)
        {
            MonitorEvent? value;
            lock (_socketGate)
                value = monitor.Recv(RecvFlags.DontWait);
            if (value is null)
                return;
            OnSocketMonitorEvent(value);
        }
    }

    private void DrainTransportDisconnects(long now)
    {
        while (_transportDisconnects.TryDequeue(out var routingId))
        {
            Peer? peer;
            lock (_gate)
            {
                peer = _peersByRid.TryGetValue(routingId, out var indexed)
                    ? indexed
                    : _peersByIntent.Values.FirstOrDefault(candidate =>
                        candidate.PhysicalRoutingId == routingId);
                if (peer is null)
                    continue;

                if (peer.Direction == ZLinkServiceConnectionDirection.Inbound)
                {
                    // An inbound transport has no local retry intent. Once
                    // its physical pipe closes, remove the peer instead of
                    // converting it into a locally reconnecting candidate.
                    //
                    // Admitted peers are exempt: the queued event carries only
                    // a routing id, so it cannot tell WHICH pipe closed.
                    // During a same-endpoint handover the retired pipe's
                    // Disconnected lands after the replacement connection was
                    // admitted and used to evict the live peer for ~15s until
                    // its re-Hello. Liveness probes the actual current pipe
                    // and removes a genuinely dead admitted peer on expiry.
                    if (!peer.Admitted)
                    {
                        RemovePeer(peer, disconnect: false);
                        continue;
                    }
                    continue;
                }

                if (!peer.Admitted)
                    continue;

                peer.Admitted = false;
                peer.State = MeshPeerState.Connecting;
                peer.Admission = null;
                peer.Liveness = null;
                if (!peer.RoutingId.IsEmpty
                    && _peersByRid.TryGetValue(peer.RoutingId, out var current)
                    && ReferenceEquals(current, peer))
                    _peersByRid.Remove(peer.RoutingId);
                RebuildChannelSelectionPlansUnderLock();
                peer.NextAdmissionTimestamp = now;
                _state = _peersByRid.Count == 0
                    ? MeshNodeState.Started
                    : MeshNodeState.PartialReady;
            }

            Publish(MeshMonitorEventKind.PeerClosed, peerRid: peer.RoutingId);
        }
    }

    private SubmitResult SubmitRequest(
        RoutingId targetRid,
        ServiceWireConstants.Command command,
        string? channelName,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        out MeshOperationId operationId) =>
        SubmitRequest(
            targetRid,
            command,
            channelName,
            parts,
            timeout,
            flags,
            metadata,
            out operationId,
            awaitCompletion: false,
            out _);

    private SubmitResult SubmitRequest(
        RoutingId targetRid,
        ServiceWireConstants.Command command,
        string? channelName,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        out MeshOperationId operationId,
        bool awaitCompletion,
        out Task<ManagedRequestCompletion>? completion)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException(
                "Application payload is required.",
                nameof(parts));
        var effectiveTimeout = timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30)
            : timeout;
        var kind = command == ServiceWireConstants.Command.NodeRequest
            ? MeshOperationKind.NodeRequest
            : MeshOperationKind.ChannelRequest;
        if (!TryCreateOperation(
                kind,
                out var correlation,
                out var pending,
                awaitCompletion))
        {
            operationId = default;
            completion = null;
            Publish(MeshMonitorEventKind.Backpressured, peerRid: targetRid);
            return SubmitResult.Backpressured;
        }
        operationId = pending.OperationId;
        completion = pending.AwaitedCompletion?.Task;

        var result = targetRid == _routingId
            ? SubmitApplication(
                targetRid,
                command,
                correlation,
                channelName,
                parts,
                flags,
                metadata)
            : SubmitNativeApplicationRequest(
                targetRid,
                command,
                channelName,
                parts,
                effectiveTimeout,
                flags,
                metadata,
                pending);
        if (result != SubmitResult.Ok)
        {
            TryRemoveOperation(correlation, out _);
            pending.Cancel();
            operationId = default;
            completion = null;
            return result;
        }

        // Local requests have no native request window, so the managed timeout
        // remains their terminal owner. Remote requests complete from the
        // binding request Task.
        if (targetRid == _routingId)
            _ = ExpireOperationAsync(correlation, pending, effectiveTimeout);
        return SubmitResult.Ok;
    }

    private SubmitResult SubmitNativeApplicationRequest(
        RoutingId targetRid,
        ServiceWireConstants.Command command,
        string? channelName,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        PendingOperation pending)
    {
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetRid, out peer);
        if (peer is null || !peer.Admitted)
            return SubmitResult.NotConnected;
        var wire = new List<ReadOnlyMemory<byte>>(metadata.IsEmpty ? 2 : 3);
        try
        {
            var head = ZLinkServiceWireCodec.EncodeApplication(
                command,
                pending.OperationId.Low,
                channelName,
                !metadata.IsEmpty);
            wire.Add(head);
            if (!metadata.IsEmpty)
                wire.Add(metadata);
            wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));

            if (!RunInboundOperation(() => CompleteNativeApplicationRequestAsync(
                    peer.PhysicalRoutingId,
                    pending,
                    wire,
                    timeout,
                    _stop?.Token ?? CancellationToken.None)))
                return SubmitResult.Terminated;

            Publish(
                MeshMonitorEventKind.MessageSubmitted,
                peerRid: targetRid,
                channelName: channelName ?? string.Empty);
            return SubmitResult.Ok;
        }
        catch (ObjectDisposedException)
        {
            return SubmitResult.Terminated;
        }
        catch (ZlinkSubmitException exception)
        {
            return NormalizeNativeSubmitFailure(
                exception.Result,
                AcceptsApplicationOperations);
        }
        catch (ZlinkException)
        {
            return SubmitResult.Terminated;
        }
    }

    internal async ValueTask SendToNodeDirectAsync(
        RoutingId targetRid,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        var peer = RequireDirectPeer(targetRid);
        await SendDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateApplicationWire(
                    ServiceWireConstants.Command.NodeSend,
                    0,
                    null,
                    parts,
                    metadata),
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestToNodeDirectAsync(
        RoutingId targetRid,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var peer = RequireDirectPeer(targetRid);
        var operationId = NextStandaloneOperationId();
        var reply = await RequestDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateApplicationWire(
                    ServiceWireConstants.Command.NodeRequest,
                    operationId.Low,
                    null,
                    parts,
                    metadata),
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
        return DecodeDirectApplicationReply(operationId.Low, reply);
    }

    internal async ValueTask SendToChannelDirectAsync(
        string sourceSpotId,
        string channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        if (!TrySelectChannelTarget(channelName, out var targetRid))
            //  Spec 07-channel-topology:414-415 — when no admitted remote
            //  Server has positive weight the call "ends with no target", and
            //  spec 32:87 classifies a target that doesn't exist as NotFound.
            //  Reuse the sync path's three-way selection-failure classification
            //  (draining -> Terminated, known-but-not-ready -> NotConnected,
            //  no target/member -> NotFound) instead of collapsing every
            //  selection failure to NotConnected/Unavailable.
            throw new ZlinkSubmitException(ChannelSelectionFailureResult(channelName) switch
            {
                SubmitResult.Terminated => ZlinkSubmitException.ErrorCode.Terminated,
                SubmitResult.NotConnected => ZlinkSubmitException.ErrorCode.NotConnected,
                _ => ZlinkSubmitException.ErrorCode.NotFound
            });
        var peer = RequireDirectPeer(targetRid);
        await SendDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateApplicationWire(
                    ServiceWireConstants.Command.ChannelSend,
                    0,
                    channelName,
                    parts,
                    metadata),
                cancellationToken)
            .ConfigureAwait(false);
        Publish(
            MeshMonitorEventKind.MessageSubmitted,
            peerRid: targetRid,
            channelName: channelName);
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestToChannelDirectAsync(
        string sourceSpotId,
        string channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (!TrySelectChannelTarget(channelName, out var targetRid))
            //  Spec 07-channel-topology:414-415 — when no admitted remote
            //  Server has positive weight the call "ends with no target", and
            //  spec 32:87 classifies a target that doesn't exist as NotFound.
            //  Reuse the sync path's three-way selection-failure classification
            //  (draining -> Terminated, known-but-not-ready -> NotConnected,
            //  no target/member -> NotFound) instead of collapsing every
            //  selection failure to NotConnected/Unavailable.
            throw new ZlinkSubmitException(ChannelSelectionFailureResult(channelName) switch
            {
                SubmitResult.Terminated => ZlinkSubmitException.ErrorCode.Terminated,
                SubmitResult.NotConnected => ZlinkSubmitException.ErrorCode.NotConnected,
                _ => ZlinkSubmitException.ErrorCode.NotFound
            });
        var peer = RequireDirectPeer(targetRid);
        var operationId = NextStandaloneOperationId();
        var reply = await RequestDirectWireAsync(
                peer.PhysicalRoutingId,
                CreateApplicationWire(
                    ServiceWireConstants.Command.ChannelRequest,
                    operationId.Low,
                    channelName,
                    parts,
                    metadata),
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
        Publish(
            MeshMonitorEventKind.MessageSubmitted,
            peerRid: targetRid,
            channelName: channelName);
        return DecodeDirectApplicationReply(operationId.Low, reply);
    }

    private Peer RequireDirectPeer(RoutingId targetRid)
    {
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetRid, out peer);
        if (peer is null || !peer.Admitted)
            throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        return peer;
    }

    private static IReadOnlyList<ReadOnlyMemory<byte>> CreateApplicationWire(
        ServiceWireConstants.Command command,
        ulong correlation,
        string? channelName,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("Application payload is required.", nameof(parts));
        var wire = new List<ReadOnlyMemory<byte>>(metadata.IsEmpty ? 2 : 3)
        {
            ZLinkServiceWireCodec.EncodeApplication(
                command, correlation, channelName, !metadata.IsEmpty)
        };
        if (!metadata.IsEmpty)
            wire.Add(metadata);
        wire.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        return wire;
    }

    private async ValueTask SendDirectWireAsync(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> wire,
        CancellationToken cancellationToken)
    {
        var messages = wire.Select(Message.From).ToArray();
        var ownershipTransferred = false;
        try
        {
            Task admission;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ObjectDisposedException(nameof(ZLinkManagedMeshNode));
                admission = socket.Send(target).Messages(messages).Async(cancellationToken);
                ownershipTransferred = true;
            }
            await admission.ConfigureAwait(false);
        }
        finally
        {
            if (!ownershipTransferred)
                DisposeParts(messages);
        }
    }

    private async ValueTask<IReadOnlyList<Message>> RequestDirectWireAsync(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> wire,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var messages = wire.Select(Message.From).ToArray();
        var ownershipTransferred = false;
        try
        {
            Task<IReadOnlyList<Message>> request;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ObjectDisposedException(nameof(ZLinkManagedMeshNode));
                request = socket.Request(target)
                    .Messages(messages)
                    .Timeout(timeout)
                    .Async(cancellationToken);
                ownershipTransferred = true;
            }
            return await request.ConfigureAwait(false);
        }
        finally
        {
            if (!ownershipTransferred)
                DisposeParts(messages);
        }
    }

    private static IReadOnlyList<Message> DecodeDirectApplicationReply(
        ulong correlation,
        IReadOnlyList<Message> replyParts)
    {
        try
        {
            if (replyParts.Count == 0
                || !ZLinkServiceWireCodec.TryDecodeReply(
                    replyParts[0].ToArray(), out var reply, out _)
                || reply.Correlation != correlation)
                throw new ZlinkRequestException(
                    ZlinkRequestException.ErrorCode.ProtocolError);
            if (reply.TerminalResult != (int)RequestResult.Ok)
            {
                //  Spec 51-internal-service-wire-protocol:97 — an unexpected
                //  conditional tail is rejected as a protocol error: a failed
                //  reply carries exactly the header frame, so an attached
                //  payload makes the reply unprocessable (spec 32:91-92)
                //  rather than a carrier of its semantic terminal.
                if (replyParts.Count != 1)
                    throw new ZlinkRequestException(
                        ZlinkRequestException.ErrorCode.ProtocolError);
                //  Application terminal from a node/channel reply header — carry the
                //  fine failure code for ownership-aware refinement. (The
                //  ProtocolError throws in this method are local decode failures
                //  with no fine code and stay plain ZlinkRequestExceptions.)
                throw new ZLinkRequestTerminalException(
                    (RequestResult)reply.TerminalResult,
                    checked((int)reply.FailureCode));
            }
            if (replyParts.Count != 2
                || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                    replyParts[1].AsReadOnlyMemory(), out var decoded))
                throw new ZlinkRequestException(
                    ZlinkRequestException.ErrorCode.ProtocolError);
            return decoded;
        }
        finally
        {
            DisposeParts(replyParts);
        }
    }

    private async Task CompleteNativeApplicationRequestAsync(
        RoutingId target,
        PendingOperation pending,
        IReadOnlyList<ReadOnlyMemory<byte>> wire,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var messages = new Message[wire.Count];
        var created = 0;
        var ownershipTransferred = false;
        try
        {
            for (; created < messages.Length; created++)
                messages[created] = Message.From(wire[created]);
            Task<IReadOnlyList<Message>> request;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ObjectDisposedException(nameof(ZLinkManagedMeshNode));
                var operation = socket.Request(target)
                    .Messages(messages)
                    .Timeout(timeout);
                ownershipTransferred = true;
                request = operation.Async(cancellationToken);
            }

            var replies = await request.ConfigureAwait(false);
            CompleteNativeApplicationRequest(pending, RequestResult.Ok, replies);
        }
        catch (ZlinkRequestException exception)
        {
            CompleteNativeApplicationRequest(
                pending,
                NormalizeNativeRequestFailure(
                    exception.Result,
                    AcceptsApplicationOperations),
                Array.Empty<Message>());
        }
        catch (OperationCanceledException)
        {
            CompleteNativeApplicationRequest(
                pending,
                RequestResult.Terminated,
                Array.Empty<Message>());
        }
        catch (ObjectDisposedException)
        {
            CompleteNativeApplicationRequest(
                pending,
                NormalizeNativeRequestFailure(
                    ZlinkRequestException.ErrorCode.Terminated,
                    AcceptsApplicationOperations),
                Array.Empty<Message>());
        }
        catch (ZlinkException)
        {
            CompleteNativeApplicationRequest(
                pending,
                NormalizeNativeRequestFailure(
                    ZlinkRequestException.ErrorCode.Terminated,
                    AcceptsApplicationOperations),
                Array.Empty<Message>());
        }
        finally
        {
            if (!ownershipTransferred)
                for (var index = 0; index < created; index++)
                    messages[index].Dispose();
        }
    }

    internal static SubmitResult NormalizeNativeSubmitFailure(
        ZlinkSubmitException.ErrorCode result,
        bool sourceAcceptsApplicationOperations)
    {
        if (result == ZlinkSubmitException.ErrorCode.Terminated
            && sourceAcceptsApplicationOperations)
            return SubmitResult.NotConnected;

        return (SubmitResult)(int)result;
    }

    internal static RequestResult NormalizeNativeRequestFailure(
        ZlinkRequestException.ErrorCode result,
        bool sourceAcceptsApplicationOperations)
    {
        if (result == ZlinkRequestException.ErrorCode.Terminated
            && sourceAcceptsApplicationOperations)
            return RequestResult.NotConnected;

        return (RequestResult)(int)result;
    }

    private bool AcceptsApplicationOperations
    {
        get
        {
            lock (_gate)
                return _state is MeshNodeState.Started
                    or MeshNodeState.PartialReady
                    or MeshNodeState.Ready;
        }
    }

    private void CompleteNativeApplicationRequest(
        PendingOperation pending,
        RequestResult result,
        IReadOnlyList<Message> replyParts)
    {
        try
        {
            if (result != RequestResult.Ok)
            {
                CompleteManagedOperation(
                    pending,
                    result,
                    0,
                    Array.Empty<Message>());
                return;
            }

            if (replyParts.Count == 0
                || !ZLinkServiceWireCodec.TryDecodeReply(
                    replyParts[0].ToArray(),
                    out var reply,
                    out _)
                || reply.Correlation != pending.OperationId.Low)
            {
                CompleteManagedOperation(
                    pending,
                    RequestResult.ProtocolError,
                    (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
                return;
            }

            if (reply.TerminalResult == (int)RequestResult.Ok)
            {
                if (replyParts.Count != 2
                    || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                        replyParts[1].AsReadOnlyMemory(),
                        out var decodedReplyParts))
                {
                    CompleteManagedOperation(
                        pending,
                        RequestResult.ProtocolError,
                        (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                        Array.Empty<Message>());
                    return;
                }

                CompleteManagedOperation(
                    pending,
                    (RequestResult)reply.TerminalResult,
                    checked((int)reply.FailureCode),
                    decodedReplyParts);
                return;
            }

            if (replyParts.Count != 1)
            {
                CompleteManagedOperation(
                    pending,
                    RequestResult.ProtocolError,
                    (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
                return;
            }

            CompleteManagedOperation(
                pending,
                (RequestResult)reply.TerminalResult,
                checked((int)reply.FailureCode),
                Array.Empty<Message>());
        }
        finally
        {
            DisposeParts(replyParts);
        }
    }

    private SubmitResult SubmitInfrastructureOperation(
        RoutingId targetRid,
        ulong targetNodeGeneration,
        MeshOperationKind kind,
        Func<ulong, MeshOperationId, byte[]> encode,
        out MeshOperationId operationId,
        TimeSpan timeout,
        MeshOperationId correlationId = default)
    {
        if (kind is not (MeshOperationKind.UserSpotCreate
            or MeshOperationKind.UserSpotClose
            or MeshOperationKind.ActorCreate
            or MeshOperationKind.ActorDestroy))
            throw new ArgumentOutOfRangeException(nameof(kind));

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetRid, out peer);
        if (peer is null
            || !peer.Admitted
            || peer.LifecycleGeneration != targetNodeGeneration)
        {
            operationId = default;
            return SubmitResult.NotConnected;
        }

        var created = correlationId == default
            ? TryCreateOperation(kind, out var correlation, out var pending)
            : TryCreateOperation(
                kind,
                correlationId,
                out correlation,
                out pending);
        if (!created)
        {
            operationId = default;
            Publish(MeshMonitorEventKind.Backpressured, peerRid: targetRid);
            return SubmitResult.Backpressured;
        }
        operationId = pending.OperationId;

        byte[] head;
        try
        {
            head = encode(correlation, operationId);
        }
        catch
        {
            TryRemoveOperation(correlation, out _);
            pending.Cancel();
            operationId = default;
            throw;
        }

        if (!TryScheduleRoutedSend(peer.PhysicalRoutingId, [head]))
        {
            TryRemoveOperation(correlation, out _);
            pending.Cancel();
            operationId = default;
            return SubmitResult.Backpressured;
        }

        _ = ExpireOperationAsync(
            correlation,
            pending,
            timeout <= TimeSpan.Zero ? TimeSpan.FromSeconds(30) : timeout);
        Publish(MeshMonitorEventKind.MessageSubmitted, peerRid: targetRid);
        return SubmitResult.Ok;
    }

    private SubmitResult SubmitApplication(
        RoutingId targetRid,
        ServiceWireConstants.Command command,
        ulong correlation,
        string? channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("Application payload is required.", nameof(parts));

        var head = ZLinkServiceWireCodec.EncodeApplication(
            command,
            correlation,
            channelName,
            !metadata.IsEmpty);
        if (targetRid == _routingId)
        {
            var recordKind = command switch
            {
                ServiceWireConstants.Command.NodeSend => MeshRecordKind.NodeSend,
                ServiceWireConstants.Command.NodeRequest => MeshRecordKind.NodeRequest,
                ServiceWireConstants.Command.ChannelSend => MeshRecordKind.ChannelSend,
                ServiceWireConstants.Command.ChannelRequest => MeshRecordKind.ChannelRequest,
                _ => throw new ArgumentOutOfRangeException(nameof(command))
            };
            var retained = CloneParts(parts);
            Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null;
            if (correlation != 0)
                reply = (replyParts, _) =>
                {
                    CompleteLocalOperation(correlation, replyParts);
                    return SubmitResult.Ok;
                };
            EnqueueOwned(
                MailboxKey.ForNode(MeshReadyDomains.Application),
                new MeshReceiveRecord(
                    recordKind,
                    MeshReadyDomains.Application,
                    _routingId,
                    string.Empty,
                    _lifecycleGeneration,
                    default,
                    correlation == 0 ? default : new MeshOperationId(0, correlation),
                    command == ServiceWireConstants.Command.NodeRequest
                        ? MeshOperationKind.NodeRequest
                        : command == ServiceWireConstants.Command.ChannelRequest
                            ? MeshOperationKind.ChannelRequest
                            : default,
                    channelName,
                    null,
                    metadata.IsEmpty ? null : metadata.ToArray(),
                    0,
                    retained.Count,
                    0,
                    0,
                    null,
                    reply),
                retained);
            return SubmitResult.Ok;
        }

        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(targetRid, out peer);
        if (peer is null || !peer.Admitted)
            return SubmitResult.NotConnected;

        var wireParts = new List<ReadOnlyMemory<byte>>(3) { head };
        if (!metadata.IsEmpty)
            wireParts.Add(metadata);
        wireParts.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        var sent = TryScheduleRoutedSend(peer.PhysicalRoutingId, wireParts);
        if (!sent)
        {
            Publish(MeshMonitorEventKind.Backpressured, peerRid: targetRid);
            return SubmitResult.Backpressured;
        }
        Publish(
            MeshMonitorEventKind.MessageSubmitted,
            peerRid: targetRid,
            channelName: channelName ?? string.Empty);
        return SubmitResult.Ok;
    }

    private SubmitResult SendNativeApplicationReply(
        RoutingId targetRid,
        ulong requestSeq,
        ulong correlation,
        IReadOnlyList<Message> parts) =>
        SendNativeTerminalReply(
            targetRid,
            requestSeq,
            correlation,
            RequestResult.Ok,
            0,
            parts);

    private (ReplySubmitOperation Submit, Message[] Wire)
        PrepareNativeTerminalReply(
        RoutingId targetRid,
        ReplyOperation reply,
        ulong correlation,
        RequestResult result,
        uint failureCode,
        IReadOnlyList<Message> parts)
    {
        var wire = new Message[parts.Count == 0 ? 1 : 2];
        var created = 0;
        try
        {
            wire[created++] = Message.From(
                ZLinkServiceWireCodec.EncodeReply(
                    correlation,
                    (int)result,
                    failureCode));
            if (parts.Count != 0)
                wire[created++] = Message.From(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
            return (reply.Messages(wire), wire);
        }
        catch
        {
            for (var index = 0; index < created; index++)
                wire[index].Dispose();
            throw;
        }
    }

    private static SubmitResult SubmitNativeTerminalReply(
        ReplySubmitOperation reply)
    {
        try
        {
            reply.Submit();
            return SubmitResult.Ok;
        }
        catch (ObjectDisposedException)
        {
            return SubmitResult.Terminated;
        }
        catch (ZlinkConfigException exception)
        {
            return exception.Result == ZlinkConfigException.ErrorCode.InvalidState
                ? SubmitResult.InvalidState
                : SubmitResult.Terminated;
        }
        catch (ZlinkSubmitException exception)
        {
            return exception.Result == ZlinkSubmitException.ErrorCode.Backpressured
                ? SubmitResult.Backpressured
                : SubmitResult.Terminated;
        }
        catch (ZlinkException)
        {
            return SubmitResult.Terminated;
        }
    }

    private SubmitResult SendNativeTerminalReply(
        RoutingId targetRid,
        ulong requestSeq,
        ulong correlation,
        RequestResult result,
        uint failureCode,
        IReadOnlyList<Message> parts)
    {
        var wire = new Message[parts.Count == 0 ? 1 : 2];
        var created = 0;
        try
        {
            wire[created++] = Message.From(
                ZLinkServiceWireCodec.EncodeReply(
                    correlation,
                    (int)result,
                    failureCode));
            if (parts.Count != 0)
                wire[created++] = Message.From(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));

            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    return SubmitResult.Terminated;
                socket.Reply(targetRid, requestSeq).Messages(wire).Submit();
            }
            return SubmitResult.Ok;
        }
        catch (ObjectDisposedException)
        {
            return SubmitResult.Terminated;
        }
        catch (ZlinkSubmitException exception)
        {
            return exception.Result == ZlinkSubmitException.ErrorCode.Backpressured
                ? SubmitResult.Backpressured
                : SubmitResult.Terminated;
        }
        catch (ZlinkException)
        {
            return SubmitResult.Terminated;
        }
        catch (Exception)
        {
            return SubmitResult.Terminated;
        }
        finally
        {
            for (var index = 0; index < created; index++)
                wire[index].Dispose();
        }
    }

    private SubmitResult SendTerminalReply(
        RoutingId targetRid,
        ulong correlation,
        RequestResult result,
        uint failureCode,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        var wireParts = new List<ReadOnlyMemory<byte>>(parts.Count == 0 ? 1 : 2)
        {
            ZLinkServiceWireCodec.EncodeReply(correlation, (int)result, failureCode)
        };
        if (parts.Count != 0)
            wireParts.Add(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts));
        return TryScheduleRoutedSend(targetRid, wireParts)
            ? SubmitResult.Ok
            : SubmitResult.Backpressured;
    }

    private void CompleteOperation(
        ZLinkServiceWireCodec.ReplyRecord reply,
        IReadOnlyList<Message> receivedParts)
    {
        if (!TryRemoveOperation(reply.Correlation, out var pending)
            || !pending.TryComplete())
            return;
        MeshRecordPayload? completion = null;
        bool decoded;
        if (pending.Kind == MeshOperationKind.ActorCreate)
        {
            decoded = ZLinkServiceWireCodec.TryDecodeActorCreateReply(
                reply,
                _meshName,
                out var actorCreateCompletion,
                out _);
            completion = actorCreateCompletion;
        }
        else if (pending.Kind == MeshOperationKind.ActorDestroy)
        {
            decoded = ZLinkServiceWireCodec.TryDecodeActorDestroyReply(
                reply,
                out var actorDestroyCompletion,
                out _);
            completion = actorDestroyCompletion;
        }
        else
        {
            decoded = ZLinkServiceWireCodec.TryDecodeUserSpotReply(
                reply,
                pending.Kind,
                out completion,
                out _);
        }
        if (!decoded)
        {
            EnqueueCompletion(
                pending.OperationId,
                pending.Kind,
                (int)RequestResult.ProtocolError,
                (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                Array.Empty<Message>());
            return;
        }
        if (reply.TerminalResult == (int)RequestResult.Ok)
        {
            if (receivedParts.Count == 1)
            {
                EnqueueCompletion(
                    pending.OperationId,
                    pending.Kind,
                    reply.TerminalResult,
                    checked((int)reply.FailureCode),
                    Array.Empty<Message>(),
                    completion);
                return;
            }

            if (receivedParts.Count != 2
                || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                    receivedParts[1].AsReadOnlyMemory(),
                    out var decodedParts))
            {
                EnqueueCompletion(
                    pending.OperationId,
                    pending.Kind,
                    (int)RequestResult.ProtocolError,
                    (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
                return;
            }

            EnqueueCompletion(
                pending.OperationId,
                pending.Kind,
                reply.TerminalResult,
                checked((int)reply.FailureCode),
                decodedParts,
                completion);
            return;
        }

        if (receivedParts.Count != 1)
        {
            EnqueueCompletion(
                pending.OperationId,
                pending.Kind,
                (int)RequestResult.ProtocolError,
                (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                Array.Empty<Message>());
            return;
        }

        EnqueueCompletion(
            pending.OperationId,
            pending.Kind,
            reply.TerminalResult,
            checked((int)reply.FailureCode),
            Array.Empty<Message>(),
            completion);
    }

    private void ProcessReplyRelayAck(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayAckRecord ack)
    {
        var key = PendingReplyRelayKey.Create(sourceNodeRid, ack);
        if (!_pendingReplyRelays.TryGetValue(key, out var pending))
            return;
        Peer? peer;
        lock (_gate)
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        if (peer is null
            || !peer.Admitted
            || !IsExactReplyRelayAckSource(
                sourceNodeRid,
                peer.LifecycleGeneration,
                pending.ExpectedSource,
                ack.RequestSource))
            return;
        pending.Completion.TrySetResult(ack);
    }

    internal static bool IsExactReplyRelayAckSource(
        RoutingId authenticatedSourceRid,
        ulong authenticatedSourceGeneration,
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        ZLinkServiceWireCodec.RequestSourceFence acknowledgedSource) =>
        acknowledgedSource == expectedSource
        && acknowledgedSource.NodeRid == authenticatedSourceRid
        && acknowledgedSource.NodeGeneration == authenticatedSourceGeneration;

    internal static bool IsReplyRelayPayloadAllowed(
        uint terminalResult,
        int payloadPartCount) =>
        payloadPartCount is 0 or 1
        && (terminalResult == 0 || payloadPartCount == 0);

    private void ProcessReplyRelay(
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        IReadOnlyList<Message> receivedParts)
    {
        IRelocationReplyRelayTarget? target;
        ulong sourceNodeGeneration;
        lock (_gate)
        {
            target = _relocationReplyRelayTarget;
            sourceNodeGeneration = _peersByRid.TryGetValue(
                    sourceNodeRid,
                    out var peer)
                && peer.Admitted
                ? peer.LifecycleGeneration
                : 0;
        }
        Message[] payload;
        if (receivedParts.Count == 1)
            payload = [];
        else if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                     receivedParts[1].AsReadOnlyMemory(),
                     out payload))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        if (target is null || sourceNodeGeneration == 0)
        {
            DisposeParts(payload);
            return;
        }
        if (!RunInboundOperation(
                () => ProcessReplyRelayAsync(
                    target,
                    sourceNodeRid,
                    sourceNodeGeneration,
                    relay,
                    payload)))
            DisposeParts(payload);
    }

    private async Task ProcessReplyRelayAsync(
        IRelocationReplyRelayTarget target,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        IReadOnlyList<Message> payload)
    {
        try
        {
            var ack = await target.RelayAsync(
                    relay,
                    sourceNodeRid,
                    sourceNodeGeneration,
                    payload,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            if (ack is { } accepted)
            {
                var delivered = await SendServiceTerminalAsync(
                        sourceNodeRid,
                        [ZLinkServiceWireCodec.EncodeReplyRelayAck(accepted)])
                    .ConfigureAwait(false);
                if (!delivered)
                    Publish(MeshMonitorEventKind.ProtocolError,
                        peerRid: sourceNodeRid);
            }
        }
        catch (OperationCanceledException) when (_stop?.IsCancellationRequested == true)
        {
        }
        catch
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void CompleteLocalOperation(
        ulong correlation,
        IReadOnlyList<Message> replyParts)
    {
        if (!_operations.TryGetValue(correlation, out var pending))
            return;
        CompleteManagedOperation(
            pending,
            RequestResult.Ok,
            0,
            replyParts.Select(Message.From).ToArray());
    }

    private async Task ExpireOperationAsync(
        ulong correlation,
        PendingOperation pending,
        TimeSpan timeout)
    {
        try
        {
            await Task.Delay(timeout, pending.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        var removed = false;
        lock (_operationGate)
        {
            if (_operations.TryGetValue(correlation, out var current)
                && ReferenceEquals(current, pending)
                && _operations.TryRemove(
                    new KeyValuePair<ulong, PendingOperation>(
                        correlation,
                        current)))
            {
                removed = true;
                RemoveRelocationReplyOperationUnderLock(
                    pending,
                    rememberTerminal: true);
            }
        }
        if (removed && pending.TryComplete())
        {
            if (pending.AwaitedCompletion is { } awaitedCompletion)
                awaitedCompletion.TrySetResult(
                    new ManagedRequestCompletion(
                        RequestResult.TimedOut,
                        Array.Empty<Message>()));
            else
                EnqueueCompletion(
                    pending.OperationId,
                    pending.Kind,
                    (int)RequestResult.TimedOut,
                    0,
                    Array.Empty<Message>());
        }
    }

    private void EnqueueCompletion(
        MeshOperationId operationId,
        MeshOperationKind operationKind,
        int result,
        int failure,
        IReadOnlyList<Message> parts,
        MeshRecordPayload? kindData = null,
        bool publishEvent = true)
    {
        var completionRecord = new MeshReceiveRecord(
            MeshRecordKind.Completion,
            MeshReadyDomains.Infrastructure,
            default,
            string.Empty,
            0,
            default,
            operationId,
            operationKind,
            null,
            null,
            null,
            0,
            parts.Count,
            result,
            failure,
            kindData);
        var queued = new QueuedRecord(
            completionRecord,
            parts,
            GetApplicationPayloadBytes(completionRecord, parts));
        if (TryEnqueueInfrastructureCompletion(queued))
        {
            SignalReadyIfNeeded();
        }
        else
        {
            // A completion is a binding operation terminal, not an application
            // admission record. Do not retain, evict, or replace it in a
            // Framework-owned queue when the diagnostic mailbox is full.
            var terminal = Volatile.Read(ref _completionOverflowHandler);
            if (terminal is null)
            {
                queued.Dispose();
            }
            else
            {
                try
                {
                    terminal(completionRecord, parts);
                }
                catch (Exception exception)
                {
                    queued.Dispose();
                    ZLinkFrameworkDebugLog.TaskFailure(
                        "completion-terminal-handler",
                        exception);
                }
            }
        }
        if (publishEvent)
            Publish(
                MeshMonitorEventKind.OperationCompleted,
                operationId: operationId,
                resultCode: result,
                failureErrno: failure);
    }

    private bool TryEnqueueInfrastructureCompletion(QueuedRecord queued)
    {
        var mailbox = _ownedMailboxes.GetOrAdd(
            MailboxKey.ForNode(MeshReadyDomains.Infrastructure),
            _ => new OwnedMailbox(
                RecordOwnedRecordEnqueued,
                RecordOwnedRecordDequeued));
        return mailbox.TryEnqueue(
            queued,
            MailboxMessageBudget,
            MailboxByteBudget);
    }

    private static IDisposable AttachApplicationAdmission(
        IDisposable coreCreditOwner,
        ZLinkApplicationJobQueueLease? admission) =>
        admission is null
            ? coreCreditOwner
            : new ZLinkApplicationJobQueueCreditOwner(
                coreCreditOwner,
                admission);

    private bool EnqueueOwned(
        MailboxKey key,
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        bool admitApplication = false,
        IDisposable? creditOwner = null)
    {
        var payloadBytes = GetApplicationPayloadBytes(record, parts);
        record.ApplicationPayloadBytes = payloadBytes;
        var queued = new QueuedRecord(record, parts, payloadBytes, creditOwner);
        try
        {
            var mailbox = _ownedMailboxes.GetOrAdd(
                key,
                _ => new OwnedMailbox(
                    RecordOwnedRecordEnqueued,
                    RecordOwnedRecordDequeued));
            if (admitApplication
                && creditOwner is ZLinkApplicationJobQueueCreditOwner
                {
                    Admission: { } applicationAdmission
                })
                applicationAdmission.MarkQueued();
            if (!mailbox.TryEnqueue(
                    queued,
                    MailboxMessageBudget,
                    MailboxByteBudget))
            {
                RecordInboundBackpressureDrop(record.Kind);
                queued.Dispose();
                Publish(MeshMonitorEventKind.Backpressured);
                return false;
            }
            SignalReadyIfNeeded();
            return true;
        }
        catch
        {
            queued.Dispose();
            throw;
        }
    }

    private static ulong GetApplicationPayloadBytes(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts) =>
        record.ApplicationPayloadBytes
        ?? (parts is IZLinkApplicationPayloadSized sized
            ? sized.ApplicationPayloadBytes
            : ZLinkEnvelopeCodec.MeasureApplicationPayloadBytes(parts));

    private void RecordOwnedRecordEnqueued(ulong pendingBytes)
    {
        Interlocked.Increment(ref _queuedMessages);
        Interlocked.Add(ref _queuedBytes, checked((long)pendingBytes));
    }

    private void RecordOwnedRecordDequeued(ulong pendingBytes)
    {
        Interlocked.Decrement(ref _queuedMessages);
        Interlocked.Add(ref _queuedBytes, -checked((long)pendingBytes));
    }

    private void RecordInboundBackpressureDrop(MeshRecordKind kind)
    {
        var classification = kind switch
        {
            MeshRecordKind.NodeSend => ("node", "send"),
            MeshRecordKind.ChannelSend => ("channel", "send"),
            MeshRecordKind.SpotSend => ("spot", "send"),
            MeshRecordKind.ActorSend => ("actor", "send"),
            _ => default
        };
        if (classification == default)
            return;
        ZLinkRuntimeMetrics.RecordMessageDropped(
            _meshName,
            classification.Item1,
            classification.Item2,
            "backpressure");
    }

    private bool DrainOwnedQueue(
        OwnedMailbox mailbox,
        MeshReceiveBatch batch,
        RecvFlags flags)
    {
        var count = 0;
        var maximumRecords = Math.Min(ReceiveBatchSize, batch.MaximumRecords);
        while (count < maximumRecords
               && mailbox.TryDequeue(batch, out var queued))
        {
            batch.Add(
                queued.Record,
                queued.TakeParts(),
                queued.TakeCreditOwner());
            count++;
        }
        return count > 0;
    }

    private void ReleaseOwnedMailbox(OwnedMailbox mailbox)
    {
        mailbox.Release();
        if (mailbox.HasRecords)
            SignalReadyIfNeeded();
    }

    private void SignalReadyIfNeeded()
    {
        if (!_ownedMailboxes.Values.Any(static mailbox => mailbox.HasRecords)
            || Interlocked.CompareExchange(ref _readyPosted, 1, 0) != 0)
            return;
        _readyHandler?.Invoke(MeshReadyDomains.All);
    }

    private bool TrySelectChannelTarget(string channelName, out RoutingId targetRid)
    {
        lock (_gate)
            return _channelSelection.TrySelect(channelName, out targetRid);
    }

    //  Spec 08 §7 treats a ChannelName with no selectable target as NotFound.
    //  A weight-zero or draining member is excluded before submission, so it
    //  is not a transport connection failure. The declaration check is kept
    //  separately for monitoring and failure metrics.
    private SubmitResult ChannelSelectionFailureResult(string channelName)
    {
        var reason = ChannelSelectionFailureReason(channelName);
        ZLinkRuntimeMetrics.RecordChannelSelectionFailure(
            _meshName,
            channelName,
            reason);
        return reason switch
        {
            "draining" => SubmitResult.Terminated,
            "not_ready" => SubmitResult.NotConnected,
            _ => SubmitResult.NotFound
        };
    }

    private string ChannelSelectionFailureReason(string channelName)
    {
        lock (_gate)
        {
            if (_state == MeshNodeState.Draining)
                return "draining";
            //  Spec 08 §3.2 step 4 removes a weight-zero target from selection,
            //  but the target remains a declared member for monitoring and
            //  connection intent reconciliation. A previously admitted peer
            //  that is now connecting is a known target whose route is not
            //  ready, so it maps to Unavailable instead of NotFound.
            if (!_channelSelection.IsDeclared(channelName))
                return "no_member";
            return _peersByIntent.Values.Any(peer =>
                    !peer.Admitted
                    && peer.State != MeshPeerState.Closed
                    && peer.Channels.ContainsKey(channelName))
                ? "not_ready"
                : "no_target";
        }
    }

    private void RebuildChannelSelectionPlansUnderLock()
    {
        var channelNames = new HashSet<string>(
            StringComparer.Ordinal);
        // RouteMesh selects an admitted remote Server. The local Server role
        // advertises membership but is never a candidate for its own route.
        // Keep peer declarations, including the last admitted declaration on
        // a connection that is currently being re-established. This preserves
        // the known-target distinction for Unavailable while still keeping
        // only admitted peers as actual selection candidates.
        foreach (var peer in _peersByIntent.Values)
            channelNames.UnionWith(peer.Channels.Keys);
        _channelSelection.Rebuild(channelNames, BuildChannelTargetsUnderLock);
    }

    private ZLinkMeshChannelTarget[] BuildChannelTargetsUnderLock(
        string channelName)
    {
        var targets = new List<ZLinkMeshChannelTarget>();
        foreach (var peer in _peersByRid.Values)
        {
            if (!peer.Admitted
                || peer.State == MeshPeerState.Draining
                || !peer.Channels.TryGetValue(channelName, out var weight)
                || weight == 0)
                continue;
            targets.Add(new ZLinkMeshChannelTarget(
                peer.RoutingId,
                checked((int)weight)));
        }
        return targets
            .OrderBy(
                static target => target.SelectionKey,
                StringComparer.Ordinal)
            .ToArray();
    }

    private void ConnectPeerCore(Peer peer)
    {
        peer.State = MeshPeerState.Connecting;
        peer.PhysicalRoutingId = peer.ExpectedRid
            ?? RoutingId.From($"zlink-intent-{peer.Intent:x16}");
        lock (_socketGate)
        {
            _socket!.Options.SetConnectRoutingId(peer.PhysicalRoutingId);
            _socket.Connect(peer.Endpoint);
        }
        peer.NextAdmissionTimestamp = 0;
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"mesh_peer_connect local={_routingId} peer={peer.ExpectedRid?.ToString() ?? "<unknown>"} "
            + $"endpoint={peer.Endpoint} intent={peer.Intent}");
        Publish(MeshMonitorEventKind.PeerConnecting, peerRid: peer.ExpectedRid ?? default);
    }

    private void SendAdmission(
        Peer peer,
        ServiceWireConstants.Command command)
    {
        if (_socket is null)
            return;
        var target = peer.PhysicalRoutingId;
        if (target.IsEmpty)
            return;
        ulong descriptorRevision;
        Dictionary<string, uint> channels;
        byte runtimeState;
        lock (_gate)
        {
            descriptorRevision = _descriptorRevision;
            channels = _channels.ToDictionary(
                static entry => entry.Key.Value,
                static entry => entry.Value,
                StringComparer.Ordinal);
            //  Spec 28 §567: draining node는 그 사실을 descriptor로 알려 새
            //  selection과 placement에서 빠진다. runtime-state.draining = 2.
            runtimeState = _state == MeshNodeState.Draining ? (byte)2 : (byte)1;
        }
        var descriptor = ZLinkServiceWireCodec.EncodeRouteAdmission(
            command,
            _meshName,
            _advertisedEndpoint,
            _lifecycleGeneration,
            descriptorRevision,
            channels,
            (byte)_objectRole,
            runtimeState,
            ZLinkServiceSecurityIdentity.Plaintext);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"mesh_peer_admission_sent local={_routingId} target={peer.RoutingId} "
            + $"command={command} endpoint={_advertisedEndpoint} "
            + $"lifecycle={_lifecycleGeneration} revision={descriptorRevision}");
        SendControl(
            target,
            peer.ConnectionGeneration,
            command,
            descriptor);
    }

    private bool SendControl(
        RoutingId target,
        ulong connectionGeneration,
        ServiceWireConstants.Command command,
        byte[] head)
    {
        if (!TryGetInfrastructureControlCommand([head], out var encodedCommand)
            || encodedCommand != command
            || !IsInfrastructureControlFrameShape(command, 1)
            || !IsWithinInfrastructureControlBounds([head], command))
            return false;

        return RunInboundOperation(() => SendControlAsync(
            target,
            connectionGeneration,
            head,
            _stop?.Token ?? CancellationToken.None));
    }

    private async Task SendControlAsync(
        RoutingId target,
        ulong connectionGeneration,
        byte[] head,
        CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            var current = _peersByIntent.Values.FirstOrDefault(peer =>
                peer.PhysicalRoutingId == target
                && peer.ConnectionGeneration == connectionGeneration
                && peer.State != MeshPeerState.Closed);
            if (current is null)
                return;
        }

        try
        {
            await SendRoutedAsync(target, [head], cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException)
        {
            ClosePeerAfterControlSendFailure(target, connectionGeneration);
        }
        catch (ZlinkSubmitException submit)
            when (submit.Result != ZlinkSubmitException.ErrorCode.NotConnected
                  && submit.Result != ZlinkSubmitException.ErrorCode.NotFound
                  && submit.Result != ZlinkSubmitException.ErrorCode.Terminated)
        {
            //  Spec 13-mesh-node:331 (condition 3) — only route/lifecycle
            //  terminal evidence ("the previous pipe has ended") may demote
            //  the peer epoch. Backpressure (queue/HWM/submit timeout),
            //  NotAdmitted (a LIVE route rejecting admission, e.g. zero
            //  weight), and caller-contract or internal submit failures are
            //  not that evidence: keep the admission/liveness generation
            //  fence intact and let the admission retry pump try again.
            //  NotConnected/NotFound/Terminated fall through to the demotion
            //  below.
        }
        catch (ZlinkSubmitException)
        {
            //  Only the route/lifecycle-terminal submit codes reach here
            //  (NotConnected/NotFound/Terminated — everything else was
            //  consumed above): terminal evidence, demote the epoch.
            ClosePeerAfterControlSendFailure(target, connectionGeneration);
        }
        catch (ZlinkException)
        {
            //  Spec 13-mesh-node:331 (condition 3) — non-submit failures on
            //  this path (ZlinkConfigException from native message
            //  allocation/copy, ZlinkHandlerException from lazy routed-
            //  admission handler registration) are local configuration/
            //  allocation faults, not confirmation that the previous peer
            //  pipe ended. Keep the admission/liveness generation fence
            //  intact; the admission retry pump owns the retry.
        }
    }

    private void ClosePeerAfterControlSendFailure(
        RoutingId physicalRoutingId,
        ulong connectionGeneration)
    {
        var publishClosed = false;
        RoutingId closedRid = default;
        lock (_gate)
        {
            var peer = _peersByIntent.Values.FirstOrDefault(candidate =>
                candidate.PhysicalRoutingId == physicalRoutingId
                && candidate.ConnectionGeneration == connectionGeneration
                && candidate.State != MeshPeerState.Closed);
            if (peer is null)
                return;
            //  Spec 13-mesh-node:331-337 (and 07-channel-topology:571-576) —
            //  in a manual fixed-RID topology a peer whose previous pipe ended
            //  is reconnected as long as "the application configuration has
            //  intent to connect to that peer". An outbound peer carries that
            //  standing intent, so a failed control send may end only the
            //  current connection epoch — mirroring DrainTransportDisconnects
            //  — never the intent itself. Removing it here let the admission
            //  retry Hello (whose routed send fails while the route is down)
            //  erase the reconnect candidate, so the peer vanished from
            //  status instead of reporting `connecting`.
            if (peer.Direction == ZLinkServiceConnectionDirection.Outbound)
            {
                if (!peer.Admitted)
                    //  Already demoted to a retrying epoch (for example by the
                    //  transport-disconnect drain); the admission retry pump
                    //  owns the next attempt.
                    return;
                peer.Admitted = false;
                peer.State = MeshPeerState.Connecting;
                peer.Admission = null;
                peer.Liveness = null;
                if (!peer.RoutingId.IsEmpty
                    && _peersByRid.TryGetValue(peer.RoutingId, out var current)
                    && ReferenceEquals(current, peer))
                    _peersByRid.Remove(peer.RoutingId);
                RebuildChannelSelectionPlansUnderLock();
                peer.NextAdmissionTimestamp = Stopwatch.GetTimestamp();
                _state = _peersByRid.Count == 0
                    ? MeshNodeState.Started
                    : MeshNodeState.PartialReady;
                publishClosed = true;
                closedRid = peer.RoutingId;
            }
            else
            {
                RemovePeer(peer, disconnect: true);
            }
        }
        if (publishClosed)
            Publish(MeshMonitorEventKind.PeerClosed, peerRid: closedRid);
    }

    private bool TryScheduleRoutedSend(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        try
        {
            return RunInboundOperation(() => SendRoutedBestEffortAsync(
                target,
                parts,
                _stop?.Token ?? CancellationToken.None));
        }
        catch (ObjectDisposedException)
        {
            return false;
        }
        catch (ZlinkException)
        {
            return false;
        }
    }

    private async Task SendRoutedBestEffortAsync(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        CancellationToken cancellationToken)
    {
        try
        {
            await SendRoutedAsync(target, parts, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException)
        {
        }
        catch (ZlinkException)
        {
        }
    }

    private async Task SendRoutedAsync(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        CancellationToken cancellationToken)
    {
        if (parts.Count == 0)
            throw new ArgumentException("A routed send requires at least one part.", nameof(parts));
        if (TryGetInfrastructureControlCommand(parts, out var command)
            && (parts.Count > MaxInfrastructureControlParts
                || !IsInfrastructureControlFrameShape(command, parts.Count)
                || !IsWithinInfrastructureControlBounds(parts, command)
                || !IsValidOptionalApplicationPayload(parts, command)))
            throw new ArgumentException(
                "The infrastructure control record is invalid.",
                nameof(parts));

        var messages = new Message[parts.Count];
        var created = 0;
        var ownershipTransferred = false;
        try
        {
            for (; created < messages.Length; created++)
                messages[created] = Message.From(parts[created]);
            Task admission;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ObjectDisposedException(nameof(ZLinkManagedMeshNode));
                var operation = socket.Send(target).Messages(messages);
                ownershipTransferred = true;
                admission = operation.Async(cancellationToken);
            }
            await admission.ConfigureAwait(false);
        }
        finally
        {
            if (!ownershipTransferred)
                for (var index = 0; index < created; index++)
                    messages[index].Dispose();
        }
    }

    private void RetireDuplicatePeer(
        Peer peer,
        Peer survivor,
        RoutingId logicalRoutingId)
    {
        var wasAdmitted = peer.Admitted;
        var physicalRoutingId = peer.PhysicalRoutingId;
        var nativeHandoverOwnsRoute = SharesNativeHandoverRoute(
            peer,
            survivor,
            logicalRoutingId);
        if (peer.Admitted
            || peer.State != MeshPeerState.Configured
            || _peersByIntent.ContainsKey(peer.Intent))
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"mesh_peer_duplicate_retire local={_routingId} peer={peer.RoutingId} "
                + $"expected={peer.ExpectedRid?.ToString() ?? "<unknown>"} "
                + $"endpoint={peer.Endpoint} intent={peer.Intent} state={peer.State}");
        _peersByIntent.Remove(peer.Intent);
        if (!peer.RoutingId.IsEmpty
            && _peersByRid.TryGetValue(peer.RoutingId, out var indexed)
            && ReferenceEquals(indexed, peer))
            _peersByRid.Remove(peer.RoutingId);
        peer.Admitted = false;
        peer.State = MeshPeerState.Closed;
        RebuildChannelSelectionPlansUnderLock();
        if (peer.Direction == ZLinkServiceConnectionDirection.Outbound
            && _socket is not null
            && !nativeHandoverOwnsRoute)
            DisconnectTransport(
                peer,
                wasAdmitted,
                physicalRoutingId);
        else if (nativeHandoverOwnsRoute)
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"mesh_peer_duplicate_retire_skip_transport local={_routingId} "
                + $"peer={peer.RoutingId} logical={logicalRoutingId} "
                + $"survivor={survivor.RoutingId}");
    }

    // Core's ROUTER handover keeps the losing reciprocal pipe as a standby
    // route. DisconnectRid addresses the logical RID selected by that
    // handover, so using it here would close the survivor rather than the
    // retired Framework intent.
    private static bool SharesNativeHandoverRoute(
        Peer retired,
        Peer survivor,
        RoutingId logicalRoutingId) =>
        !logicalRoutingId.IsEmpty
        && (retired.RoutingId == logicalRoutingId
            || retired.PhysicalRoutingId == logicalRoutingId)
        && (survivor.RoutingId == logicalRoutingId
            || survivor.ExpectedRid == logicalRoutingId
            || survivor.PhysicalRoutingId == logicalRoutingId);

    private ulong ResolvePeerGeneration(RoutingId sourceRid)
    {
        lock (_gate)
            return _peersByRid.TryGetValue(sourceRid, out var peer)
                ? peer.LifecycleGeneration
                : 0;
    }

    private void RemovePeer(Peer peer, bool disconnect)
    {
        var wasAdmitted = peer.Admitted;
        var physicalRoutingId = peer.PhysicalRoutingId;
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"mesh_peer_remove local={_routingId} peer={peer.RoutingId} "
            + $"expected={peer.ExpectedRid?.ToString() ?? "<unknown>"} "
            + $"endpoint={peer.Endpoint} intent={peer.Intent} state={peer.State} "
            + $"disconnect={disconnect}");
        _peersByIntent.Remove(peer.Intent);
        if (!peer.RoutingId.IsEmpty
            && _peersByRid.TryGetValue(peer.RoutingId, out var indexed)
            && ReferenceEquals(indexed, peer))
            _peersByRid.Remove(peer.RoutingId);
        peer.Admitted = false;
        peer.State = MeshPeerState.Closed;
        RebuildChannelSelectionPlansUnderLock();
        if (disconnect && _socket is not null)
            DisconnectTransport(
                peer,
                wasAdmitted,
                physicalRoutingId);
        Publish(MeshMonitorEventKind.PeerClosed, peerRid: peer.RoutingId);
    }

    private void DisconnectTransport(
        Peer peer,
        bool wasAdmitted,
        RoutingId physicalRoutingId)
    {
        try
        {
            lock (_socketGate)
            {
                var replacementUsesEndpoint = _peersByIntent.Values.Any(
                    otherPeer =>
                        !ReferenceEquals(otherPeer, peer)
                        && string.Equals(
                            otherPeer.Endpoint,
                            peer.Endpoint,
                            StringComparison.Ordinal)
                        && otherPeer.State != MeshPeerState.Closed);
                if (wasAdmitted || replacementUsesEndpoint)
                {
                    if (!physicalRoutingId.IsEmpty)
                        _socket!.DisconnectRid(physicalRoutingId);
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"mesh_peer_transport_disconnect local={_routingId} "
                        + $"peer={peer.RoutingId} mode=rid physical={physicalRoutingId} "
                        + $"replacement={replacementUsesEndpoint}");
                    return;
                }

                // With no replacement, endpoint disconnect also cancels the
                // binding's reconnect intent. This is required for a removed
                // Connecting peer whose physical RID is still pending.
                _socket!.Disconnect(peer.Endpoint);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"mesh_peer_transport_disconnect local={_routingId} "
                    + $"peer={peer.RoutingId} mode=endpoint endpoint={peer.Endpoint}");
            }
        }
        catch (ZlinkException)
        {
        }
    }

    private void Publish(
        MeshMonitorEventKind kind,
        RoutingId peerRid = default,
        string channelName = "",
        MeshOperationId operationId = default,
        int resultCode = 0,
        int failureErrno = 0)
    {
        RawMeshMonitor[] monitors;
        MeshNodeState state;
        lock (_gate)
        {
            monitors = _monitors.ToArray();
            state = _state;
        }
        foreach (var monitor in monitors)
            monitor.Publish(
                kind,
                state,
                peerRid,
                channelName,
                operationId,
                resultCode,
                failureErrno);
    }

    private void ThrowIfStarted()
    {
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_state != MeshNodeState.Created)
                throw new InvalidOperationException("The MeshNode has already started.");
        }
    }

    private void ThrowIfDisposed() =>
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);

    private static ulong NewNonZeroToken()
    {
        Span<byte> bytes = stackalloc byte[sizeof(ulong)];
        do
        {
            RandomNumberGenerator.Fill(bytes);
        } while (BinaryPrimitives.ReadUInt64BigEndian(bytes) == 0);
        return BinaryPrimitives.ReadUInt64BigEndian(bytes);
    }

    private ulong NextAuthorityOwnerGeneration()
    {
        var generation = Interlocked.Increment(ref _nextAuthorityOwnerGeneration);
        if (generation == 0 || generation > long.MaxValue)
            throw new InvalidOperationException(
                "The authority owner generation space was exhausted.");
        return generation;
    }

    private MeshOperationId NextStandaloneOperationId()
    {
        lock (_operationGate)
        {
            var low = ++_nextOperation;
            if (low == 0)
                throw new InvalidOperationException(
                    "The operation id space was exhausted.");
            return new MeshOperationId(_lifecycleGeneration, low);
        }
    }

    private static void ValidateObservedAuthority(
        RoutingId targetNodeRid,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        if (targetNodeRid.IsEmpty)
            throw new ArgumentException(
                "The observed owner node routing id is required.",
                nameof(targetNodeRid));
        // Internal route fences carry the native ulong generation space. The
        // public SpotRef/ActorRef object-generation contract is bounded by
        // long.MaxValue, but an Entry Spot uses the MeshNode lifecycle
        // generation and can legitimately occupy the upper half of ulong.
        if (objectGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(objectGeneration));
        if (authorityOwnerGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(authorityOwnerGeneration));
        if (ownerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(ownerLeaseGeneration));
    }

    private static long Add(long timestamp, TimeSpan duration)
    {
        var delta = (long)Math.Ceiling(duration.TotalSeconds * Stopwatch.Frequency);
        return checked(timestamp + delta);
    }

    private readonly record struct MailboxKey(
        MeshOwnerKind OwnerKind,
        string Identity,
        ulong Generation,
        MeshReadyDomains Domain,
        string SpotId,
        ActorRef Actor)
    {
        internal static MailboxKey ForNode(MeshReadyDomains domain) =>
            new(MeshOwnerKind.Node, string.Empty, 0, domain, string.Empty, default);

        internal static MailboxKey ForSpot(
            ZLinkManagedSpot spot,
            MeshReadyDomains domain) =>
            new(
                MeshOwnerKind.Spot,
                spot.SpotId,
                spot.LifecycleGeneration,
                domain,
                spot.SpotId,
                default);

        internal static MailboxKey ForActor(
            ManagedActor actor,
            MeshReadyDomains domain) =>
            new(
                MeshOwnerKind.Actor,
                actor.Ref.ActorId,
                actor.Ref.ObjectGeneration,
                domain,
                string.Empty,
                actor.Ref);
    }

    private readonly record struct ObservedSpotAuthorityKey(
        RoutingId NodeRid,
        string SpotId,
        ulong ObjectGeneration);

    private readonly record struct ObservedActorAuthorityKey(
        RoutingId NodeRid,
        string ActorId,
        ulong ObjectGeneration);

    private readonly record struct ObservedAuthority(
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration);

    private readonly record struct ActorSnapshot(
        ActorRef Ref,
        string SpotId,
        ulong SpotGeneration,
        ulong MembershipEpoch);

    private sealed class ManagedActor(
        ActorRef actorRef,
        string spotId,
        ulong spotGeneration,
        ulong membershipEpoch,
        ulong authorityOwnerGeneration)
    {
        private readonly object _gate = new();
        private long _sequence;
        private bool _draining;
        private ActorBinding? _binding;

        internal ActorRef Ref { get; } = actorRef;
        internal string SpotId { get; private set; } = spotId;
        internal ulong SpotGeneration { get; private set; } = spotGeneration;
        internal ulong MembershipEpoch { get; private set; } = membershipEpoch;
        internal ulong AuthorityOwnerGeneration { get; private set; } =
            authorityOwnerGeneration;
        internal bool Draining
        {
            get
            {
                lock (_gate)
                    return _draining;
            }
        }
        internal ActorBinding? Binding
        {
            get
            {
                lock (_gate)
                    return _binding;
            }
        }

        internal void SetAuthorityOwnerGeneration(ulong value)
        {
            if (value == 0)
                throw new ArgumentOutOfRangeException(nameof(value));
            lock (_gate)
                AuthorityOwnerGeneration = value;
        }
        internal ActorLocation Location =>
            new(Ref, SpotId, SpotGeneration, MembershipEpoch);

        internal ActorSnapshot Snapshot()
        {
            lock (_gate)
                return new ActorSnapshot(
                    Ref,
                    SpotId,
                    SpotGeneration,
                    MembershipEpoch);
        }

        internal ulong NextSequence() =>
            checked((ulong)Interlocked.Increment(ref _sequence));

        internal bool TryDrain()
        {
            lock (_gate)
            {
                if (_draining)
                    return false;
                _draining = true;
                _binding = null;
                return true;
            }
        }

        internal bool TryMove(
            ActorSnapshot expected,
            string targetSpotId,
            ulong targetSpotGeneration)
        {
            lock (_gate)
            {
                if (_draining
                    || SpotId != expected.SpotId
                    || SpotGeneration != expected.SpotGeneration
                    || MembershipEpoch != expected.MembershipEpoch)
                    return false;
                SpotId = targetSpotId;
                SpotGeneration = targetSpotGeneration;
                MembershipEpoch = checked(MembershipEpoch + 1);
                return true;
            }
        }

        internal ulong Bind(
            ZLinkManagedStreamSessionService service,
            RoutingId sessionRid)
        {
            lock (_gate)
            {
                var generation = checked((_binding?.Generation ?? 0) + 1);
                _binding = new ActorBinding(service, sessionRid, generation);
                return generation;
            }
        }

        internal bool TryClearBinding(ulong expectedGeneration)
        {
            lock (_gate)
            {
                if (_binding is null
                    || (expectedGeneration != 0
                        && _binding.Generation != expectedGeneration))
                    return false;
                _binding = null;
                return true;
            }
        }
    }

    private sealed record ActorBinding(
        ZLinkManagedStreamSessionService Service,
        RoutingId SessionRid,
        ulong Generation);

    private sealed class PendingOperation(
        MeshOperationId operationId,
        MeshOperationKind kind,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        bool awaitCompletion = false)
    {
        private readonly CancellationTokenSource _timeout = new();
        private int _terminal;
        internal MeshOperationId OperationId { get; } = operationId;
        internal MeshOperationKind Kind { get; } = kind;
        internal ZLinkServiceWireCodec.RequestSourceFence RequestSource { get; } =
            requestSource;
        internal TaskCompletionSource<ManagedRequestCompletion>?
            AwaitedCompletion { get; } = awaitCompletion
                ? new(TaskCreationOptions.RunContinuationsAsynchronously)
                : null;
        internal ulong DeadlineUnixMs { get; set; }
        internal CancellationToken Token => _timeout.Token;
        internal bool TryComplete()
        {
            if (Interlocked.CompareExchange(ref _terminal, 1, 0) != 0)
                return false;
            _timeout.Cancel();
            _timeout.Dispose();
            return true;
        }
        internal void Cancel()
        {
            if (Interlocked.Exchange(ref _terminal, 1) == 0)
            {
                _timeout.Cancel();
                AwaitedCompletion?.TrySetResult(
                    new ManagedRequestCompletion(
                        RequestResult.Terminated,
                        Array.Empty<Message>()));
            }
            _timeout.Dispose();
        }
    }

    private readonly record struct ManagedRequestCompletion(
        RequestResult Result,
        IReadOnlyList<Message> Parts,
        int FailureErrno = 0);

    private readonly record struct RelocationReplyTerminalKey(
        ulong ReplyRouteId,
        MeshOperationId OperationId);

    private readonly record struct RelocationReplyTerminal(
        DateTimeOffset ExpiresAt,
        ZLinkServiceWireCodec.RequestSourceFence RequestSource);

    internal readonly record struct PendingReplyRelayKey(
        RoutingId TargetNodeRid,
        MeshOperationId OperationId,
        ulong ReplyRouteId,
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator)
    {
        internal static PendingReplyRelayKey Create(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.ReplyRelayRecord relay) =>
            new(
                targetNodeRid,
                relay.OperationId,
                relay.ReplyRouteId,
                relay.RelocationId,
                relay.Coordinator);

        internal static PendingReplyRelayKey Create(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.ReplyRelayAckRecord ack) =>
            new(
                targetNodeRid,
                ack.OperationId,
                ack.ReplyRouteId,
                ack.RelocationId,
                ack.Coordinator);
    }

    private readonly record struct PendingRelocationPrepareKey(
        RoutingId TargetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator);

    private void RememberSessionRelocationSealReply(
        PendingSessionRelocationKey key,
        ZLinkServiceWireCodec.SessionRelocationSealedRecord response) =>
        RememberSessionRelocationReply(
            _sessionRelocationSealReplyTerminals,
            _sessionRelocationSealReplyTerminalOrder,
            key,
            response);

    private static void RememberSessionRelocationReply<T>(
        ConcurrentDictionary<PendingSessionRelocationKey, T> terminals,
        ConcurrentQueue<PendingSessionRelocationKey> order,
        PendingSessionRelocationKey key,
        T response)
        where T : struct, IEquatable<T>
    {
        if (!terminals.TryAdd(key, response))
        {
            if (!terminals.TryGetValue(key, out var existing)
                || !existing.Equals(response))
                throw new InvalidDataException(
                    "A session relocation reply changed after completion.");
            return;
        }
        order.Enqueue(key);
        while (terminals.Count > MaxRelocationReplyTerminals
               && order.TryDequeue(out var oldest))
            terminals.TryRemove(oldest, out _);
    }

    private readonly record struct PendingSessionRelocationKey(
        RoutingId SessionOwnerNodeRid,
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator,
        string ActorId,
        RoutingId SessionRid,
        ulong BindingGeneration,
        byte Action)
    {
        internal static PendingSessionRelocationKey Create(
            RoutingId owner,
            ZLinkServiceWireCodec.SessionRelocationSealRecord record) =>
            new(
                owner,
                record.RelocationId,
                record.Coordinator,
                record.Actor.Actor.ActorId,
                record.Session.SessionRid,
                record.Session.BindingGeneration,
                0);

        internal static PendingSessionRelocationKey Create(
            RoutingId owner,
            ZLinkServiceWireCodec.SessionRelocationSealedRecord record) =>
            new(
                owner,
                record.RelocationId,
                record.Coordinator,
                record.Actor.Actor.ActorId,
                record.Session.SessionRid,
                record.Session.BindingGeneration,
                0);

    }

    private sealed class PendingSessionRelocationSeal(
        byte[] fingerprint,
        ZLinkServiceWireCodec.SessionRelocationSealRecord request)
    {
        internal byte[] Fingerprint { get; } = fingerprint;
        internal TaskCompletionSource<
            ZLinkServiceWireCodec.SessionRelocationSealedRecord> Completion
            { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal bool TryComplete(
            ZLinkServiceWireCodec.SessionRelocationSealedRecord response)
        {
            if (response.RelocationId != request.RelocationId
                || response.Coordinator != request.Coordinator
                || response.Actor != request.Actor
                || response.Session != request.Session)
                return false;
            return Completion.TrySetResult(response)
                   || Completion.Task.IsCompletedSuccessfully
                      && Completion.Task.Result == response;
        }
    }

    private sealed class PendingRelocationPrepare(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        byte[] fingerprint)
    {
        private int _waiters = 1;
        private int _closing;
        internal byte[] Fingerprint { get; } = fingerprint;
        internal TaskCompletionSource<ZLinkServiceWireCodec.RelocationReadyRecord>
            Ready { get; } = new(
                TaskCreationOptions.RunContinuationsAsynchronously);

        internal bool Matches(
            ZLinkServiceWireCodec.RelocationReadyRecord value) =>
            value.SenderRole == 2
            && value.RelocationId == prepare.RelocationId
            && value.TargetAttemptGeneration
               == prepare.TargetAttemptGeneration
            && value.Coordinator == prepare.Coordinator
            && value.Target == prepare.Target
            && value.Object == prepare.Object;

        internal bool TryAcquireWaiter()
        {
            while (Volatile.Read(ref _closing) == 0)
            {
                var waiters = Volatile.Read(ref _waiters);
                if (waiters == 0)
                    return false;
                if (Interlocked.CompareExchange(
                        ref _waiters,
                        checked(waiters + 1),
                        waiters) == waiters)
                    return true;
            }
            return false;
        }

        internal bool ReleaseWaiter()
        {
            var remaining = Interlocked.Decrement(ref _waiters);
            if (remaining < 0)
                throw new InvalidOperationException(
                    "A canonical relocation prepare waiter was released twice.");
            if (remaining != 0)
                return false;
            return Interlocked.CompareExchange(ref _closing, 1, 0) == 0;
        }
    }

    private sealed class PendingReplyRelay(
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        ZLinkServiceWireCodec.ReplyRelayRecord relay)
    {
        internal ZLinkServiceWireCodec.RequestSourceFence ExpectedSource { get; } =
            expectedSource;
        internal ZLinkServiceWireCodec.ReplyRelayRecord Relay { get; } = relay;
        internal TaskCompletionSource<ZLinkServiceWireCodec.ReplyRelayAckRecord>
            Completion { get; } = new(
                TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private readonly record struct RemoteUserSpotOperationKey(
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        MeshOperationId OperationId);

    private sealed class RemoteUserSpotInvocation
    {
        private readonly Lazy<Task<UserSpotOperationTerminal>> _task;

        internal RemoteUserSpotInvocation(
            ZLinkServiceWireCodec.UserSpotOperationRecord record,
            Func<Task<UserSpotOperationTerminal>> execute)
        {
            Record = record;
            _task = new Lazy<Task<UserSpotOperationTerminal>>(
                execute,
                LazyThreadSafetyMode.ExecutionAndPublication);
        }

        internal ZLinkServiceWireCodec.UserSpotOperationRecord Record { get; }
        internal Task<UserSpotOperationTerminal> Task => _task.Value;
    }

    private readonly record struct RemoteActorCreateOperationKey(
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        MeshOperationId OperationId);

    private sealed class RemoteActorCreateInvocation
    {
        private readonly Lazy<Task<ActorCreateOperationTerminal>> _task;

        internal RemoteActorCreateInvocation(
            ZLinkServiceWireCodec.ActorCreateOperationRecord record,
            Func<Task<ActorCreateOperationTerminal>> execute)
        {
            Record = record;
            _task = new Lazy<Task<ActorCreateOperationTerminal>>(
                execute,
                LazyThreadSafetyMode.ExecutionAndPublication);
        }

        internal ZLinkServiceWireCodec.ActorCreateOperationRecord Record { get; }
        internal Task<ActorCreateOperationTerminal> Task => _task.Value;
    }

}

internal sealed class ZLinkManagedSpot(
    ZLinkManagedMeshNode node,
    string spotId,
    ulong lifecycleGeneration,
    ulong authorityOwnerGeneration) : ISpot
{
    private readonly Dictionary<ZLinkChannelName, HashSet<string>> _subscriptions = [];
    private int _disposed;
    private int _actorCount;

    private string _spotId = ZLinkSpotId.Require(spotId, nameof(spotId));
    public RoutingId RoutingId => ZLinkSpotId.ToNativeRoutingId(_spotId);
    internal string SpotId => _spotId;
    public ulong LifecycleGeneration { get; } = lifecycleGeneration;
    internal ulong AuthorityOwnerGeneration { get; } = authorityOwnerGeneration;
    internal int ActorCount => Volatile.Read(ref _actorCount);

    public void SetRoutingId(RoutingId routingId)
    {
        var next = ZLinkSpotId.FromNativeRoutingId(routingId);
        ZLinkSpotId.Require(next, nameof(routingId));
        var previous = _spotId;
        node.RekeySpot(this, previous, next);
        _spotId = next;
    }

    public SpotStatus Status() => new(LifecycleGeneration);

    public void SetSubscription(string channelName, string topic)
    {
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        lock (_subscriptions)
        {
            if (!_subscriptions.TryGetValue(channel, out var topics))
            {
                topics = new HashSet<string>(StringComparer.Ordinal);
                _subscriptions.Add(channel, topics);
            }
            topics.Add(topic);
        }
    }

    internal bool Matches(string channelName, string topic)
        => Matches(
            ZLinkChannelName.FromBoundary(channelName, nameof(channelName)),
            topic);

    internal bool Matches(ZLinkChannelName channelName, string topic)
    {
        lock (_subscriptions)
            return _subscriptions.TryGetValue(channelName, out var topics)
                   && topics.Contains(topic);
    }

    internal void AddActor() => Interlocked.Increment(ref _actorCount);

    internal void RemoveActor()
    {
        if (Interlocked.Decrement(ref _actorCount) < 0)
            Interlocked.Exchange(ref _actorCount, 0);
    }

    public SubmitResult SendToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.SendToChannel(channelName, parts, flags, metadata);

    public SubmitResult RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.RequestToChannel(
            channelName,
            parts,
            out operationId,
            timeout,
            flags,
            metadata);

    public SubmitResult RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        ZLinkBackendRequestCallback callback,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.RequestToChannel(
            channelName,
            parts,
            callback,
            timeout,
            flags,
            metadata);

    public void Publish(
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default)
    {
        node.Publish(SpotId, channelName, topic, parts, flags, metadata);
    }

    public SubmitResult SendToSpot(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.SendToSpot(
            SpotId,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            parts,
            flags,
            metadata);

    public SubmitResult RequestToSpot(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message> parts,
        out MeshOperationId operationId,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.RequestToSpot(
            SpotId,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            parts,
            out operationId,
            timeout,
            flags,
            metadata);

    public SubmitResult RequestToSpot(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        IReadOnlyList<Message> parts,
        MeshOperationId correlationId,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default) =>
        node.RequestToSpot(
            SpotId,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            parts,
            correlationId,
            timeout,
            flags,
            metadata);

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
            node.ReleaseSpot(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }
}

internal sealed class ZLinkManagedStreamSessionService(
    ZLinkManagedMeshNode node,
    IStreamSocket stream) : IStreamSessionService
{
    private readonly ConcurrentDictionary<
        RoutingId,
        ConcurrentDictionary<ZLinkActorId, StreamSessionBinding>> _bindings = new();
    private int _started;
    private int _disposed;

    public void Start()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        Interlocked.Exchange(ref _started, 1);
    }

    public SubmitResult BindActor(
        RoutingId sessionRid,
        ActorRef actor,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        EnsureStarted();
        return node.BindSessionActor(
            this,
            sessionRid,
            actor,
            out operationId,
            timeout);
    }

    public SubmitResult BindActor(
        RoutingId sessionRid,
        ActorRef actor,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        EnsureStarted();
        return node.BindSessionActor(
            this,
            sessionRid,
            actor,
            correlationId,
            timeout);
    }

    public SubmitResult UnbindActor(
        RoutingId sessionRid,
        ActorRef actor,
        ulong expectedBindingGeneration,
        out MeshOperationId operationId,
        TimeSpan timeout = default)
    {
        EnsureStarted();
        return node.UnbindSessionActor(
            this,
            sessionRid,
            actor,
            expectedBindingGeneration,
            out operationId,
            timeout);
    }

    public SubmitResult UnbindActor(
        RoutingId sessionRid,
        ActorRef actor,
        ulong expectedBindingGeneration,
        MeshOperationId correlationId,
        TimeSpan timeout = default)
    {
        EnsureStarted();
        return node.UnbindSessionActor(
            this,
            sessionRid,
            actor,
            expectedBindingGeneration,
            correlationId,
            timeout);
    }

    public StreamSessionBinding[] Bindings(RoutingId sessionRid) =>
        _bindings.TryGetValue(sessionRid, out var bindings)
            ? bindings.Values
                .OrderBy(static binding => binding.Actor.ActorId, StringComparer.Ordinal)
                .ToArray()
            : Array.Empty<StreamSessionBinding>();

    public SubmitResult SendToActor(
        RoutingId sessionRid,
        ActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        EnsureStarted();
        var actorId = ZLinkActorId.FromBoundary(actor.ActorId, nameof(actor));
        if (!_bindings.TryGetValue(sessionRid, out var bindings)
            || !bindings.TryGetValue(actorId, out var binding)
            || binding.Actor != actor)
            return SubmitResult.NotFound;
        return node.RelaySessionToActor(this, sessionRid, actor, parts, flags);
    }

    internal void RecordBinding(
        RoutingId sessionRid,
        StreamSessionBinding binding)
    {
        var session = _bindings.GetOrAdd(
            sessionRid,
            static _ => new ConcurrentDictionary<ZLinkActorId, StreamSessionBinding>());
        session[ZLinkActorId.FromBoundary(binding.Actor.ActorId, nameof(binding))] = binding;
    }

    internal void RemoveBinding(
        RoutingId sessionRid,
        string actorId,
        ulong expectedBindingGeneration)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        if (!_bindings.TryGetValue(sessionRid, out var bindings)
            || !bindings.TryGetValue(actorKey, out var binding)
            || (expectedBindingGeneration != 0
                && binding.BindingGeneration != expectedBindingGeneration))
            return;
        bindings.TryRemove(
            new KeyValuePair<ZLinkActorId, StreamSessionBinding>(
                actorKey,
                binding));
        if (bindings.IsEmpty)
            _bindings.TryRemove(sessionRid, out _);
    }

    internal SubmitResult SendToSession(
        RoutingId sessionRid,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        EnsureStarted();
        var retained = parts.Select(Message.From).ToArray();
        try
        {
            return stream.TrySend(sessionRid)
                .Messages(retained)
                .Flags(flags)
                .Submit()
                ? SubmitResult.Ok
                : SubmitResult.Backpressured;
        }
        catch (ZlinkException)
        {
            return SubmitResult.NotConnected;
        }
        finally
        {
            foreach (var part in retained)
                part.Dispose();
        }
    }

    internal async ValueTask SendToSessionAsync(
        RoutingId sessionRid,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        EnsureStarted();
        var retained = parts.Select(Message.From).ToArray();
        var ownershipTransferred = false;
        try
        {
            var terminal = stream.Send(sessionRid)
                .Messages(retained)
                .Async(cancellationToken);
            ownershipTransferred = true;
            await terminal.ConfigureAwait(false);
        }
        finally
        {
            if (!ownershipTransferred)
                foreach (var part in retained)
                    part.Dispose();
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _bindings.Clear();
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    private void EnsureStarted()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        if (Volatile.Read(ref _started) == 0)
            throw new InvalidOperationException(
                "The STREAM session service has not started.");
    }
}
