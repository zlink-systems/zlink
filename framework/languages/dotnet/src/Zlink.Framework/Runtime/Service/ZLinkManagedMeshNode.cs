using System.Collections.Concurrent;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
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
    private const int MaxCompletionControlParts = 64;
    private const long MaxCompletionControlBytes = 256 * 1024;
    private const long MaxCompletionPayloadBytes = 4_294_966_774L;
    private const long DefaultPendingCompletionByteBudget = 16L * 1024 * 1024;
    private const string DefaultInboundSecurityIdentity = "none";
    private static readonly TimeSpan DefaultInboundOperationShutdownTimeout =
        TimeSpan.FromSeconds(2);
    private static readonly TimeSpan DefaultRemoteUserSpotTerminalRetention =
        TimeSpan.FromMinutes(5);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(100);
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
    private readonly object _gate = new();
    private readonly object _socketGate = new();
    private readonly object _readyGate = new();
    private readonly object _sendReadyGate = new();
    private readonly object _operationGate = new();
    private readonly object _remoteUserSpotGate = new();
    private readonly object _remoteActorCreateGate = new();
    private readonly object _inboundOperationGate = new();
    private readonly object _disposeGate = new();
    private readonly Dictionary<string, uint> _channels = new(StringComparer.Ordinal);
    private readonly Dictionary<ulong, Peer> _peersByIntent = new();
    private readonly Dictionary<RoutingId, Peer> _peersByRid = new();
    private readonly Dictionary<RoutingId, ZLinkMeshPeerExpectation> _peerExpectations = new();
    private readonly ZLinkMeshPeerAdmission _peerAdmission = new();
    private readonly ZLinkMeshPeerControlRetryQueue _peerControlRetry = new();
    private readonly ConcurrentDictionary<MailboxKey, OwnedMailbox> _ownedMailboxes = new();
    private readonly object _pendingCompletionGate = new();
    private readonly Queue<QueuedRecord> _pendingInfrastructureCompletions = new();
    private long _pendingInfrastructureCompletionBytes;
    private long _pendingInfrastructureCompletionCount;
    private readonly ConcurrentDictionary<ulong, PendingOperation> _operations = new();
    private readonly Dictionary<RelocationReplyTerminalKey, RelocationReplyTerminal>
        _relocationReplyTerminals = [];
    private readonly Dictionary<RelocationReplyTerminalKey, PendingOperation>
        _relocationReplyOperations = [];
    private readonly Queue<RelocationReplyTerminalKey>
        _relocationReplyTerminalOrder = [];
    private readonly ConcurrentDictionary<string, ZLinkManagedSpot> _spots = new();
    private readonly object _entrySpotGate = new();
    private readonly ConcurrentDictionary<string, ManagedActor> _actors =
        new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<RemoteUserSpotOperationKey, RemoteUserSpotInvocation>
        _remoteUserSpotOperations = new();
    private readonly ConcurrentDictionary<RemoteActorCreateOperationKey, RemoteActorCreateInvocation>
        _remoteActorCreateOperations = new();
    private readonly ConcurrentDictionary<PendingReplyRelayKey, PendingReplyRelay>
        _pendingReplyRelays = new();
    private readonly ConcurrentDictionary<PendingRelocationReservationKey,
        PendingRelocationReservation> _pendingRelocationReservations = new();
    private readonly ConcurrentDictionary<PendingRelocationAttemptKey,
        PendingRelocationAttempt> _pendingRelocationAttempts = new();
    private readonly ConcurrentDictionary<ObservedSpotAuthorityKey, ObservedAuthority>
        _observedSpotAuthorities = new();
    private readonly ConcurrentDictionary<ObservedActorAuthorityKey, ObservedAuthority>
        _observedActorAuthorities = new();
    private readonly List<RawMeshMonitor> _monitors = new();
    private readonly HashSet<Task> _inboundOperations = [];
    private readonly ulong _lifecycleGeneration = NewNonZeroToken();

    private IRouterSocket? _socket;
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
    private IInstanceSpotActivationTarget? _instanceSpotActivationTarget;
    private IRelocationReplyRelayTarget? _relocationReplyRelayTarget;
    private ICanonicalRelocationReservationTarget?
        _canonicalRelocationReservationTarget;
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
    private int _peerControlRetryReady;
    private long _sendReadyVersion;
    private TaskCompletionSource _sendReadyPulse = NewSendReadyPulse();
    private int _disposed;
    private bool _inboundOperationAdmissionClosed;
    private ulong _activeSocketGeneration;
    private uint _localEffectiveMaxMessageBytes = uint.MaxValue;
    private ZLinkInboundDispatchBudget? _inboundDispatchBudget;

    internal ZLinkManagedMeshNode(
        IContext context,
        string meshName,
        int maxPendingOperations = DefaultMaxPendingOperations,
        TimeSpan? remoteUserSpotTerminalRetention = null,
        TimeSpan? inboundOperationShutdownTimeout = null)
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
    }

    public RoutingId RoutingId => _routingId;
    internal string MeshName => _meshName;
    public long MaxMessageSize { get; set; } = -1;
    public ulong RouterHighWaterMark { get; set; } = 4_096_000;
    public ulong MailboxMessageBudget { get; set; } = 10_000;
    public ulong MailboxByteBudget { get; set; } = 64 * 1024 * 1024;
    public TimeSpan? SendTimeout { get; set; }

    public void SetInboundDispatchBudget(ZLinkInboundDispatchBudget budget)
    {
        ArgumentNullException.ThrowIfNull(budget);
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_state != MeshNodeState.Created)
                throw new InvalidOperationException(
                    "The inbound dispatch budget must be configured before Start.");
            _inboundDispatchBudget = budget;
        }
    }

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
            IPoller? poller = null;
            try
            {
                _localEffectiveMaxMessageBytes =
                    ZLinkClientServerControlProtocol.NormalizeMaximumMessageBytes(
                        MaxMessageSize);
                socket.Options.Mandatory = true;
                socket.Options.Handover = true;
                socket.Options.Linger = TimeSpan.Zero;
                socket.Options.MaxMessageSize = MaxMessageSize;
                socket.Options.SendHighWaterMark = RouterHighWaterMark;
                socket.Options.ReceiveHighWaterMark = RouterHighWaterMark;
                if (SendTimeout is { } timeout)
                    socket.Options.SendTimeout = timeout;
                socket.SetRoutingId(_routingId);
                socket.OnSendReady(EnqueueSendReady);
                socket.OnCompletionControl(ProcessCompletionControl);
                var configuredBindEndpoint = _bindEndpoint;
                socket.Bind(configuredBindEndpoint);
                _bindEndpoint = socket.Options.LastEndpoint;
                if (string.Equals(
                        _advertisedEndpoint,
                        configuredBindEndpoint,
                        StringComparison.Ordinal))
                    _advertisedEndpoint = _bindEndpoint;

                poller = Systems.Zlink.Zlink.CreatePoller();
                // One poller owns both inbound frames and request completion.
                // Registering only PollIn lets the binding create a second
                // completion worker, which can process the same socket command
                // queue concurrently during disconnect and close.
                poller.Add(
                    socket,
                    PollEventFlags.PollIn | PollEventFlags.PollCompletion,
                    1);
                _socket = socket;
                _activeSocketGeneration = _lifecycleGeneration;
                _poller = poller;
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
        string expectedSecurityIdentity = "none")
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
        Peer[] peers;
        lock (_gate)
        {
            ThrowIfDisposed();
            if (!_channels.TryAdd(channelName, 100))
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
        Peer[] peers;
        lock (_gate)
        {
            if (!_channels.ContainsKey(channelName))
                throw new InvalidOperationException($"Channel '{channelName}' is not registered.");
            _channels[channelName] = weight;
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
        return TrySend(
            peer.PhysicalRoutingId,
            [encoded],
            SendFlags.DontWait);
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

    public void SetCanonicalRelocationReservationTarget(
        ICanonicalRelocationReservationTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        lock (_gate)
        {
            if (_canonicalRelocationReservationTarget is not null
                && !ReferenceEquals(_canonicalRelocationReservationTarget, target))
                throw new InvalidOperationException(
                    "A canonical relocation reservation target is already registered.");
            _canonicalRelocationReservationTarget = target;
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
                wire.Add(EncodeFrameworkMultipartForSend(
                    peer.PhysicalRoutingId,
                    payload,
                    wire[0].Length));
            if (!TrySend(peer.PhysicalRoutingId, wire, SendFlags.None))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "The relocation reply relay could not be submitted.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
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

    public async ValueTask<ZLinkServiceWireCodec.RelocationReservedRecord>
        ReserveCanonicalRelocationAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted
            || peer.LifecycleGeneration != prepare.Candidate.NodeGeneration
            || prepare.Candidate.NodeRid != targetNodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.", ZLinkRetryAdvice.RetryAfterBackoff);
        var key = new PendingRelocationReservationKey(
            targetNodeRid, prepare.RelocationId,
            prepare.TargetAttemptGeneration, prepare.Coordinator);
        var fingerprint = SHA256.HashData(
            ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare));
        PendingRelocationReservation pending;
        while (true)
        {
            if (_pendingRelocationReservations.TryGetValue(key, out pending!))
            {
                if (!pending.Fingerprint.AsSpan().SequenceEqual(fingerprint))
                    throw new InvalidDataException(
                        "A canonical relocation retry changed command 40 fields.");
                break;
            }
            pending = new PendingRelocationReservation(prepare, fingerprint,
                () => RunCanonicalRelocationReservationAsync(
                    peer, prepare, timeout, cancellationToken));
            if (_pendingRelocationReservations.TryAdd(key, pending))
            {
                _ = pending.Operation.Value.ContinueWith(
                    _ => _pendingRelocationReservations.TryRemove(
                        new KeyValuePair<PendingRelocationReservationKey,
                            PendingRelocationReservation>(key, pending)),
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
                break;
            }
        }
        return await pending.Operation.Value.WaitAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask StageCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        IReadOnlyList<ZLinkServiceWireCodec.RelocationDataRecord> data,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(data);
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted
            || peer.LifecycleGeneration != prepare.Candidate.NodeGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.", ZLinkRetryAdvice.RetryAfterBackoff);
        var key = new PendingRelocationAttemptKey(targetNodeRid,
            prepare.RelocationId, prepare.TargetAttemptGeneration,
            prepare.Coordinator);
        var pending = new PendingRelocationAttempt(prepare, peer);
        if (!_pendingRelocationAttempts.TryAdd(key, pending))
            throw new InvalidDataException(
                "A canonical relocation stage is already active.");
        var effectiveTimeout = timeout == Timeout.InfiniteTimeSpan
            ? timeout
            : timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30) : timeout;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadline.CancelAfter(effectiveTimeout);
        var commitAuthorized = false;
        try
        {
            _ = await ReserveCanonicalRelocationAsync(
                    targetNodeRid, prepare, effectiveTimeout, deadline.Token)
                .ConfigureAwait(false);
            foreach (var record in data)
            {
                pending.Register(record);
                var encoded = ZLinkServiceWireCodec.EncodeRelocationData(record);
                while (true)
                {
                    await SendCanonicalRelocationRecordAsync(
                            peer, encoded, deadline.Token)
                        .ConfigureAwait(false);
                    using var retry = CancellationTokenSource
                        .CreateLinkedTokenSource(deadline.Token);
                    retry.CancelAfter(RelocationAckRetryInterval);
                    try
                    {
                        await pending.WaitForAckAsync(record.ParticipantId,
                                record.Sequence, retry.Token)
                            .ConfigureAwait(false);
                        break;
                    }
                    catch (OperationCanceledException)
                        when (!deadline.IsCancellationRequested)
                    {
                        // Command 31 is immutable and idempotent. A duplicate
                        // makes the target replay the cumulative command 32 ACK.
                    }
                }
            }
            var noJournalRecords = data.Count == 0
                                   && prepare.Participants.All(
                                       static participant =>
                                           participant.AllowanceMessages == 0);
            if (prepare.Object.Kind == 1 || noJournalRecords)
            {
                var response = pending.CreateFinalSealResponse();
                if (pending.TryStartSealResponseRetries())
                    RunInboundOperation(() => RetryRelocationSealResponseAsync(
                        peer, pending, response));
                await pending.SealResponseSent.Task.WaitAsync(deadline.Token)
                    .ConfigureAwait(false);
                await pending.TargetSealAcknowledged.Task
                    .WaitAsync(deadline.Token)
                    .ConfigureAwait(false);
                commitAuthorized = true;
                return;
            }
            await pending.SealResponseSent.Task.WaitAsync(deadline.Token)
                .ConfigureAwait(false);
            await pending.TargetSealAcknowledged.Task
                .WaitAsync(deadline.Token)
                .ConfigureAwait(false);
            commitAuthorized = true;
        }
        finally
        {
            if (!commitAuthorized && _pendingRelocationAttempts.TryRemove(
                    new KeyValuePair<PendingRelocationAttemptKey,
                        PendingRelocationAttempt>(key, pending)))
                pending.StopSealResponseRetries();
        }
    }

    public async ValueTask CompleteCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationCompleteRecord complete,
        CancellationToken cancellationToken)
    {
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(targetNodeRid, out peer);
        if (peer is null || !peer.Admitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The canonical relocation target is not connected.", ZLinkRetryAdvice.RetryAfterBackoff);
        var key = new PendingRelocationAttemptKey(targetNodeRid,
            complete.RelocationId, complete.TargetAttemptGeneration,
            complete.Coordinator);
        if (!_pendingRelocationAttempts.TryGetValue(key, out var pending))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "The canonical relocation completion has no recoverable source attempt.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        pending.RegisterComplete(complete);
        var encoded = ZLinkServiceWireCodec.EncodeRelocationComplete(complete);
        while (!pending.CompleteAcknowledged.Task.IsCompleted)
        {
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    encoded,
                    cancellationToken)
                .ConfigureAwait(false);
            var retry = Task.Delay(
                RelocationAckRetryInterval,
                cancellationToken);
            await Task.WhenAny(pending.CompleteAcknowledged.Task, retry)
                .ConfigureAwait(false);
        }
        await pending.CompleteAcknowledged.Task
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        if (_pendingRelocationAttempts.TryRemove(
                new KeyValuePair<
                    PendingRelocationAttemptKey,
                    PendingRelocationAttempt>(key, pending)))
            pending.StopSealResponseRetries();
    }

    public void CancelCanonicalRelocation(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator)
    {
        var key = new PendingRelocationAttemptKey(targetNodeRid, relocationId,
            targetAttemptGeneration, coordinator);
        if (_pendingRelocationAttempts.TryRemove(key, out var pending))
            pending.StopSealResponseRetries();
    }

    private async ValueTask SendCanonicalRelocationRecordAsync(
        Peer expectedPeer,
        byte[] encoded,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Peer? current;
            lock (_gate)
                _peersByRid.TryGetValue(expectedPeer.RoutingId, out current);
            if (!ReferenceEquals(current, expectedPeer) || !expectedPeer.Admitted)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The canonical relocation connection changed.", ZLinkRetryAdvice.RetryAfterBackoff);
            var sendReadyVersion = Volatile.Read(ref _sendReadyVersion);
            switch (TrySendOutcome(
                expectedPeer.PhysicalRoutingId,
                [encoded],
                SendFlags.DontWait))
            {
                case MeshSendOutcome.Accepted:
                    return;
                case MeshSendOutcome.Backpressured:
                    await WaitForSendReadyAsync(
                            sendReadyVersion,
                            cancellationToken)
                        .ConfigureAwait(false);
                    break;
                case MeshSendOutcome.Stale:
                case MeshSendOutcome.PermanentFailure:
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "The canonical relocation record could not be submitted.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                default:
                    throw new ArgumentOutOfRangeException();
            }
        }
    }

    private async Task<ZLinkServiceWireCodec.RelocationReservedRecord>
        RunCanonicalRelocationReservationAsync(
            Peer peer,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        var effectiveTimeout = timeout == Timeout.InfiniteTimeSpan
            ? timeout
            : timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30) : timeout;
        using var deadline = CancellationTokenSource
            .CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(effectiveTimeout);
        try
        {
            await SendCanonicalRelocationControlAsync(peer, prepare, deadline.Token);
            var key = new PendingRelocationReservationKey(
                prepare.Candidate.NodeRid, prepare.RelocationId,
                prepare.TargetAttemptGeneration, prepare.Coordinator);
            var pending = _pendingRelocationReservations[key];
            var offer = await pending.Offer.Task.WaitAsync(deadline.Token)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_offer_received relocation={offer.RelocationId.High:x16}{offer.RelocationId.Low:x16} "
                + $"attempt={offer.TargetAttemptGeneration} kind={offer.Object.Kind}");
            var acceptance = offer with
            {
                Role = 1,
                OfferedMessages = 0,
                OfferedBytes = 0,
                Participants = prepare.Participants
            };
            await SendCanonicalRelocationControlAsync(peer, acceptance, deadline.Token);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_acceptance_sent relocation={acceptance.RelocationId.High:x16}{acceptance.RelocationId.Low:x16} "
                + $"attempt={acceptance.TargetAttemptGeneration} kind={acceptance.Object.Kind}");
            return await pending.Reserved.Task.WaitAsync(deadline.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException) when (deadline.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "The canonical relocation reservation deadline elapsed.", ZLinkRetryAdvice.RetryAfterBackoff);
        }
    }

    private async ValueTask SendCanonicalRelocationControlAsync(
        Peer expectedPeer,
        object record,
        CancellationToken cancellationToken)
    {
        var encoded = record switch
        {
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare =>
                ZLinkServiceWireCodec.EncodeRelocationPrepare(prepare),
            ZLinkServiceWireCodec.RelocationReadyRecord ready =>
                ZLinkServiceWireCodec.EncodeRelocationReady(ready),
            _ => throw new ArgumentOutOfRangeException(nameof(record))
        };
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Peer? current;
            lock (_gate)
                _peersByRid.TryGetValue(expectedPeer.RoutingId, out current);
            if (!ReferenceEquals(current, expectedPeer) || !expectedPeer.Admitted)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The canonical relocation target connection changed.", ZLinkRetryAdvice.RetryAfterBackoff);
            var sendReadyVersion = Volatile.Read(ref _sendReadyVersion);
            switch (TrySendOutcome(
                expectedPeer.PhysicalRoutingId,
                [encoded],
                SendFlags.DontWait))
            {
                case MeshSendOutcome.Accepted:
                    return;
                case MeshSendOutcome.Backpressured:
                    await WaitForSendReadyAsync(
                            sendReadyVersion,
                            cancellationToken)
                        .ConfigureAwait(false);
                    break;
                case MeshSendOutcome.Stale:
                case MeshSendOutcome.PermanentFailure:
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "The canonical relocation control could not be submitted.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                default:
                    throw new ArgumentOutOfRangeException();
            }
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
        wireParts.Add(EncodeFrameworkMultipartForSend(
            peer.PhysicalRoutingId,
            parts,
            checked(head.Length + metadata.Length)));
        if (!TrySend(peer.PhysicalRoutingId, wireParts, flags))
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
        return TrySend(peer.PhysicalRoutingId, [head], SendFlags.None)
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
        return TrySend(peer.PhysicalRoutingId, [head], SendFlags.None)
            ? SubmitResult.Ok
            : SubmitResult.Backpressured;
    }

    internal int RetainedActorCreateOperationCount =>
        _remoteActorCreateOperations.Count;

    internal int PendingCanonicalRelocationReservationCount =>
        _pendingRelocationReservations.Count;

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
                .Sum(static entry => entry.Value.Count)
                + _peerControlRetry.Count
                + checked((int)Math.Max(
                    0,
                    Volatile.Read(ref _pendingInfrastructureCompletionCount)));
            var pendingBytes = checked(
                (ulong)Math.Max(0, Volatile.Read(ref _queuedBytes))
                + (ulong)Math.Max(
                    0,
                    Volatile.Read(ref _pendingInfrastructureCompletionBytes)));
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
                if (!entry.Value.TryClaim(_inboundDispatchBudget))
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
        return _spots.GetOrAdd(
            spotId,
            value => new ZLinkManagedSpot(
                this,
                value,
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
            return _entrySpot ??= _spots.GetOrAdd(
                _routingId.ToString(),
                value => new ZLinkManagedSpot(
                    this,
                    value,
                    _lifecycleGeneration,
                    _lifecycleGeneration));
        }
    }

    public ISpot GetOrCreateSpot(string spotId, out bool created)
    {
        ZLinkSpotId.Require(spotId, nameof(spotId));
        if (_spots.TryGetValue(spotId, out var existing))
        {
            created = false;
            return existing;
        }

        var candidate = new ZLinkManagedSpot(
            this,
            spotId,
            Interlocked.Increment(ref _nextSpotGeneration),
            NextAuthorityOwnerGeneration());
        var spot = _spots.GetOrAdd(spotId, candidate);
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
        ZLinkSpotId.Require(spotId, nameof(spotId));
        if (objectGeneration == 0 || objectGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(objectGeneration));
        if (authorityOwnerGeneration == 0 || authorityOwnerGeneration > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(authorityOwnerGeneration));
        if (_spots.TryGetValue(spotId, out var existing))
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
        var spot = _spots.GetOrAdd(spotId, candidate);
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
        if (!_actors.TryAdd(actorId, actor))
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
        if (_actors.TryGetValue(actorId, out var actor)
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
        var operation = BeginOperation(MeshOperationKind.ActorDestroy, timeout);
        if (!_actors.TryGetValue(actor.ActorId, out var current)
            || current.Ref.ObjectGeneration != actor.ObjectGeneration
            || current.Ref.NodeRid != actor.NodeRid
            || !current.TryDrain())
        {
            CompleteManagedOperation(
                operation,
                RequestResult.Conflict,
                current is null ? 2 : 1,
                Array.Empty<Message>());
            return operation.OperationId;
        }

        _actors.TryRemove(new KeyValuePair<string, ManagedActor>(actor.ActorId, current));
        if (_spots.TryGetValue(current.SpotId, out var spot))
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
        return operation.OperationId;
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
        RequestCallback callback,
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
            out _,
            callback);

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
        RequestCallback callback,
        TimeSpan timeout,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        ArgumentNullException.ThrowIfNull(callback);
        if (!TrySelectChannelTarget(channelName, out var targetRid))
        {
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
            out _,
            callback);
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
            wireParts.Add(EncodeFrameworkMultipartForSend(
                peer.PhysicalRoutingId,
                parts,
                checked(head.Length + metadata.Length)));
            // Publish has already crossed its admission boundary. A remote target
            // that is no longer reachable must not hold this worker until the
            // socket send timeout; target acceptance is neither a public result nor
            // publish-specific monitoring data after the transaction starts.
            _ = TrySend(
                peer.PhysicalRoutingId,
                wireParts,
                flags | SendFlags.DontWait);
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
                await receiveLoop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }

        await CloseInboundOperationAdmissionAsync(shutdownToken).ConfigureAwait(false);

        IRouterSocket? socket;
        IPoller? poller;
        lock (_socketGate)
        {
            _activeSocketGeneration = 0;
            socket = _socket;
            poller = _poller;
            _socket = null;
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
        DisposePendingInfrastructureCompletions();
        foreach (var mailbox in _ownedMailboxes.Values)
            mailbox.Dispose();
        _ownedMailboxes.Clear();
        _peerControlRetry.Clear();
        foreach (var spot in _spots.Values)
            await spot.DisposeAsync().ConfigureAwait(false);
        _spots.Clear();

        poller?.Dispose();
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

    internal SubmitResult MessageFollowSendToSpot(
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
        SendFlags flags,
        ReadOnlyMemory<byte> metadata) =>
        SubmitSpot(
            targetRid,
            sourceSpotId,
            spotId,
            spotGeneration,
            parts,
            request: false,
            operation: null,
            flags,
            metadata,
            new SpotMessageFollowRoute(
                operationId,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                messageFollowHopCount,
                0));

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

    internal void ReleaseSpot(ZLinkManagedSpot spot)
    {
        if (spot.ActorCount != 0)
            return;
        _spots.TryRemove(
            new KeyValuePair<string, ZLinkManagedSpot>(
                spot.SpotId,
                spot));
    }

    internal void RekeySpot(
        ZLinkManagedSpot spot,
        string previousSpotId,
        string currentSpotId)
    {
        if (string.Equals(previousSpotId, currentSpotId, StringComparison.Ordinal))
            return;
        if (!_spots.TryRemove(
                new KeyValuePair<string, ZLinkManagedSpot>(previousSpotId, spot))
            || !_spots.TryAdd(currentSpotId, spot))
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
        TimeSpan timeout)
    {
        var operation = BeginOperation(MeshOperationKind.ActorJoin, timeout);
        if (targetNodeRid != _routingId
            || !TryGetActor(actorRef, out var actor)
            || !_spots.TryGetValue(targetSpotId, out var targetSpot)
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

        if (_spots.TryGetValue(previous.SpotId, out var oldSpot)
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
            Peer? peer;
            lock (_gate)
                _peersByRid.TryGetValue(targetNodeRid, out peer);
            if (peer is null || !peer.Admitted)
                return SubmitResult.NotConnected;
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
            var head = ZLinkServiceWireCodec.EncodeSpot(
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
            return SubmitStatefulWire(
                peer,
                head,
                parts,
                flags,
                metadata,
                request ? operation : null);
        }
        if (!_spots.TryGetValue(targetSpotId, out var spot))
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

    private readonly record struct SpotMessageFollowRoute(
        MeshOperationId OperationId,
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration,
        byte MessageFollowHopCount,
        ulong DeadlineUnixMs);

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
        wireParts.Add(EncodeFrameworkMultipartForSend(
            peer.PhysicalRoutingId,
            parts,
            checked(head.Length + metadata.Length)));
        if (!TrySend(peer.PhysicalRoutingId, wireParts, flags))
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
        var wire = new Message[metadata.IsEmpty ? 2 : 3];
        var created = 0;
        try
        {
            wire[created++] = Message.From(head);
            if (!metadata.IsEmpty)
                wire[created++] = Message.From(metadata);
            wire[created++] = Message.From(
                EncodeFrameworkMultipartForSend(
                    peer.PhysicalRoutingId,
                    parts,
                    checked(head.Length + metadata.Length)));

            var remainingMilliseconds = pending.DeadlineUnixMs
                - Math.Min(
                    pending.DeadlineUnixMs,
                    checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
            var remaining = TimeSpan.FromMilliseconds(
                Math.Max(1, Math.Min(remainingMilliseconds, (ulong)int.MaxValue)));
            bool submitted;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    return SubmitResult.Terminated;
                submitted = socket.Request(peer.PhysicalRoutingId)
                    .Messages(wire)
                    .Timeout(remaining)
                    .Flags(flags)
                    .Submit((result, replyParts) =>
                        CompleteNativeApplicationRequest(
                            pending,
                            result,
                            replyParts));
            }
            if (!submitted)
            {
                Publish(
                    MeshMonitorEventKind.Backpressured,
                    peerRid: peer.RoutingId);
                return SubmitResult.Backpressured;
            }

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
        finally
        {
            for (var index = 0; index < created; index++)
                wire[index].Dispose();
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
        var completion = new TaskCompletionSource<InstanceSpotActivationTerminal>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            wire[created++] = Message.From(head);
            if (metadata is { IsEmpty: false } value)
                wire[created++] = Message.From(value);
            wire[created++] = Message.From(
                EncodeFrameworkMultipartForSend(
                    peer.PhysicalRoutingId,
                    parts,
                    checked(head.Length + (metadata?.Length ?? 0))));

            var remainingMilliseconds = operation.DeadlineUnixMs
                - Math.Min(
                    operation.DeadlineUnixMs,
                    checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
            if (remainingMilliseconds == 0)
                return new InstanceSpotActivationTerminal(
                    RequestResult.TimedOut,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                    Array.Empty<ReadOnlyMemory<byte>>());
            var remaining = TimeSpan.FromMilliseconds(
                Math.Max(1, Math.Min(remainingMilliseconds, (ulong)int.MaxValue)));
            bool submitted;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    throw new ZlinkSubmitException(
                        ZlinkSubmitException.ErrorCode.Terminated);
                submitted = socket.Request(peer.PhysicalRoutingId)
                    .Messages(wire)
                    .Timeout(remaining)
                    .Flags(SendFlags.DontWait)
                    .Submit((result, replyParts) =>
                    {
                        try
                        {
                            completion.TrySetResult(
                                DecodeForwardedInstanceSpotTerminal(
                                    operation,
                                    result,
                                    replyParts));
                        }
                        finally
                        {
                            DisposeParts(replyParts);
                        }
                    });
            }
            if (!submitted)
            {
                Publish(
                    MeshMonitorEventKind.Backpressured,
                    peerRid: peer.RoutingId);
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Backpressured);
            }

            Publish(
                MeshMonitorEventKind.MessageSubmitted,
                peerRid: peer.RoutingId);
            using var cancellation = cancellationToken.Register(
                static state => ((TaskCompletionSource<InstanceSpotActivationTerminal>)state!)
                    .TrySetCanceled(),
                completion);
            return await completion.Task.ConfigureAwait(false);
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
            return new InstanceSpotActivationTerminal(
                transportResult,
                transportResult == RequestResult.TimedOut
                    ? ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut
                    : ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                Array.Empty<ReadOnlyMemory<byte>>());
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
        var effectiveTimeout = timeout <= TimeSpan.Zero
            ? TimeSpan.FromSeconds(30)
            : timeout;
        if (!TryCreateOperation(kind, out var correlation, out operation))
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
        RequestCallback? directCompletion = null)
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
            correlation = ++_nextOperation;
            if (correlation == 0)
                throw new InvalidOperationException(
                    "The operation id space was exhausted.");
            operation = new PendingOperation(
                new MeshOperationId(_lifecycleGeneration, correlation),
                kind,
                _localRequestSourceFence,
                directCompletion);
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
        if (operation.DirectCompletion is { } directCompletion)
        {
            try
            {
                directCompletion(result, parts);
            }
            catch
            {
                DisposeParts(parts);
            }
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
        if (pending.DirectCompletion is { } directCompletion)
        {
            try
            {
                directCompletion(
                    (RequestResult)relay.TerminalResult,
                    payload);
            }
            catch
            {
                ZLinkMessageParts.DisposeAll(payload);
            }
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
        _actors.TryGetValue(actorRef.ActorId, out actor!)
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
        parts.Select(Message.From).ToArray();

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
                    DrainRawSocket();
                ProcessInfrastructure(Stopwatch.GetTimestamp());
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

    private void DrainRawSocket()
    {
        var startedAt = Stopwatch.GetTimestamp();
        long bytes = 0;
        for (var index = 0; index < ReceiveBatchSize; index++)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(index, bytes, startedAt))
                return;
            ZLinkInboundReceivePermit? receivePermit = null;
            try
            {
                if (_inboundDispatchBudget is { } budget)
                {
                    // Do not block the only node poller on application HWM.
                    // Return to the poller so request completions and
                    // infrastructure control can be processed while the
                    // application lease is released by its owner.
                    if (!budget.TryAcquireReceive(out receivePermit))
                        return;
                }

                using var received = Received.Create();
                bool available;
                lock (_socketGate)
                    available = _socket!.Recv(received, RecvFlags.DontWait);
                if (!available)
                    return;
                bytes = checked(
                    bytes + ZLinkReceiveBatchBudget.MeasureParts(received.Parts));
                ProcessReceived(received, ref receivePermit);
            }
            finally
            {
                _inboundDispatchBudget?.CompleteReceiveAttempt(receivePermit);
            }
        }
    }

    private void ProcessCompletionControl(
        RoutingId sourceRid,
        IReadOnlyList<Message> parts)
    {
        try
        {
            if (!FitsCompleteMessageBound(
                    parts,
                    Volatile.Read(ref _localEffectiveMaxMessageBytes))
                || !IsAllowedCompletionControl(parts, out var command)
                || !HasCurrentCompletionControlSource(sourceRid, command)
                || !ProcessInfrastructureControl(
                    sourceRid,
                    parts,
                    parts[0].ToArray()))
                Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
        }
        finally
        {
            DisposeParts(parts);
        }
    }

    private bool HasCurrentCompletionControlSource(
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

    internal static bool IsAllowedCompletionControl(
        IReadOnlyList<Message> parts,
        out ServiceWireConstants.Command command)
    {
        command = default;
        if (parts.Count == 0 || parts.Count > MaxCompletionControlParts)
            return false;

        var head = parts[0].ToArray();
        if (head.Length < 5
            || head[0] != ServiceWireConstants.Magic0
            || head[1] != ServiceWireConstants.Magic1
            || head[2] != ServiceWireConstants.WireMajor)
            return false;

        command = (ServiceWireConstants.Command)head[3];
        if (!IsAllowedCompletionControlCommand(command))
            return false;

        return IsCompletionControlFrameShape(command, parts.Count)
            && IsWithinCompletionControlBounds(parts, command)
            && IsValidOptionalApplicationPayload(parts, command);
    }

    private static bool IsAllowedCompletionControlCommand(
        ServiceWireConstants.Command command) =>
        command is ServiceWireConstants.Command.Hello
            or ServiceWireConstants.Command.Admit
            or ServiceWireConstants.Command.Reject
            or ServiceWireConstants.Command.Update
            or ServiceWireConstants.Command.LivenessProbe
            or ServiceWireConstants.Command.LivenessAck
            or ServiceWireConstants.Command.Reply
            or ServiceWireConstants.Command.RelocationReady
            or ServiceWireConstants.Command.RelocationAck
            or ServiceWireConstants.Command.ReplyRelay
            or ServiceWireConstants.Command.RelocationData
            or ServiceWireConstants.Command.RelocationSeal
            or ServiceWireConstants.Command.RelocationComplete
            or ServiceWireConstants.Command.RelocationPrepare
            or ServiceWireConstants.Command.RelocationReserved
            or ServiceWireConstants.Command.ReplyRelayAck
            or ServiceWireConstants.Command.MessageFollow;

    private static bool ShouldUseCompletionControl(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        if (parts.Count == 0 || parts.Count > MaxCompletionControlParts)
            return false;
        if (!TryGetCompletionControlCommand(parts, out var command))
            return false;
        return IsCompletionControlFrameShape(command, parts.Count)
            && IsWithinCompletionControlBounds(parts, command)
            && IsValidOptionalApplicationPayload(parts, command);
    }

    private static bool TryGetCompletionControlCommand(
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
        return IsAllowedCompletionControlCommand(command);
    }

    private static bool IsWithinCompletionControlBounds(
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ServiceWireConstants.Command command)
    {
        var maximumBytes = IsPayloadBearingCompletionCommand(command)
            ? MaxCompletionPayloadBytes
            : MaxCompletionControlBytes;
        long totalBytes = 0;
        foreach (var part in parts)
        {
            totalBytes = checked(totalBytes + part.Length);
            if (totalBytes > maximumBytes)
                return false;
        }
        return true;
    }

    private static bool IsWithinCompletionControlBounds(
        IReadOnlyList<Message> parts,
        ServiceWireConstants.Command command)
    {
        var maximumBytes = IsPayloadBearingCompletionCommand(command)
            ? MaxCompletionPayloadBytes
            : MaxCompletionControlBytes;
        long totalBytes = 0;
        foreach (var part in parts)
        {
            totalBytes = checked(totalBytes + part.Size);
            if (totalBytes > maximumBytes)
                return false;
        }
        return true;
    }

    private static bool IsPayloadBearingCompletionCommand(
        ServiceWireConstants.Command command) =>
        command is ServiceWireConstants.Command.Reply
            or ServiceWireConstants.Command.ReplyRelay
            // RelocationData is one infrastructure record, but its frozen
            // relocation payload is governed by the negotiated complete
            // message bound rather than the small control-record bound.
            or ServiceWireConstants.Command.RelocationData;

    private static bool IsCompletionControlFrameShape(
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
        if (!_pendingRelocationReservations.IsEmpty)
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_ready_decode_skipped source={sourceRid} error={relocationReadyError}");
        if (ZLinkServiceWireCodec.TryDecodeRelocationReserved(
                head, out var relocationReserved, out _))
        {
            ProcessRelocationReserved(sourceRid, relocationReserved);
            return true;
        }
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
        if (ZLinkServiceWireCodec.TryDecodeRelocationAck(
                head, out var relocationAck, out _))
        {
            ProcessRelocationAck(sourceRid, relocationAck);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRelocationSeal(
                head, out var relocationSeal, out _))
        {
            ProcessRelocationSeal(sourceRid, relocationSeal);
            return true;
        }
        if (ZLinkServiceWireCodec.TryDecodeRelocationComplete(
                head, out var relocationComplete, out _))
        {
            ProcessRelocationComplete(sourceRid, relocationComplete);
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

    private void ProcessReceived(
        Received received,
        ref ZLinkInboundReceivePermit? receivePermit)
    {
        if (received.RoutingId is not { } sourceRid || received.Parts.Count == 0)
        {
            Publish(MeshMonitorEventKind.ProtocolError);
            return;
        }

        if (!FitsCompleteMessageBound(
                received.Parts,
                Volatile.Read(ref _localEffectiveMaxMessageBytes)))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return;
        }

        var head = received.Parts[0].ToArray();
        if (ProcessInfrastructureControl(sourceRid, received.Parts, head))
            return;
        if (ZLinkServiceWireCodec.TryDecodeStateful(
                head,
                _meshName,
                out var stateful,
                out _))
        {
            ProcessStateful(
                sourceRid,
                stateful,
                received,
                ref receivePermit);
            return;
        }
        if (ZLinkServiceWireCodec.TryDecodeInstanceSpotActivation(
                head,
                out var instanceActivation,
                out _))
        {
            ProcessInstanceSpotActivation(sourceRid, instanceActivation, received);
            return;
        }
        if (ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
                head,
                out var userSpotOperation,
                out _))
        {
            ProcessUserSpotOperation(sourceRid, userSpotOperation);
            return;
        }
        if (ZLinkServiceWireCodec.TryDecodeActorCreateOperation(
                head,
                out var actorCreateOperation,
                out _))
        {
            ProcessActorCreateOperation(sourceRid, actorCreateOperation);
            return;
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
            return;
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
                return;
            }
            var multicastMetadata = logicalMulticast.HasMetadata
                ? received.Parts[1].ToArray()
                : null;
            try
            {
                foreach (var spot in _spots.Values
                             .Where(spot => spot.Matches(
                                 logicalMulticast.ChannelName,
                                 logicalMulticast.Topic))
                             .OrderBy(static spot => spot.RoutingId.ToHex(), StringComparer.Ordinal))
                {
                    var multicastParts = CloneParts(decodedMulticastParts);
                    EnqueueOwned(
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
                        ref receivePermit);
                }
            }
            finally
            {
                DisposeParts(decodedMulticastParts);
            }
            return;
        }
        if (!ZLinkServiceWireCodec.TryDecodeApplication(
            head,
            out var application,
            out _))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return;
        }

        var payloadOffset = application.HasMetadata ? 2 : 1;
        if (received.Parts.Count != payloadOffset + 1
            || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                received.Parts[payloadOffset].AsReadOnlyMemory(),
                out var parts))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return;
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
            return;
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
        EnqueueOwned(
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
            ref receivePermit);
    }

    private void ProcessRelocationPrepare(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        ICanonicalRelocationReservationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationReservationTarget;
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
        ICanonicalRelocationReservationTarget target,
        Peer peer,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        try
        {
            var offer = await target.OfferAsync(prepare, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    ZLinkServiceWireCodec.EncodeRelocationReady(offer),
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"canonical_offer_sent relocation={offer.RelocationId.High:x16}{offer.RelocationId.Low:x16} "
                + $"attempt={offer.TargetAttemptGeneration} kind={offer.Object.Kind}");
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-offer",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationReady(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationReadyRecord ready)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_ready_received relocation={ready.RelocationId.High:x16}{ready.RelocationId.Low:x16} "
            + $"attempt={ready.TargetAttemptGeneration} role={ready.Role} kind={ready.Object.Kind}");
        if (ready.Role == 2)
        {
            var key = new PendingRelocationReservationKey(sourceNodeRid,
                ready.RelocationId, ready.TargetAttemptGeneration,
                ready.Coordinator);
            if (_pendingRelocationReservations.TryGetValue(key, out var pending)
                && pending.MatchesOffer(ready))
                pending.Offer.TrySetResult(ready);
            else
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"canonical_offer_rejected relocation={ready.RelocationId.High:x16}{ready.RelocationId.Low:x16} "
                    + $"attempt={ready.TargetAttemptGeneration} pending={pending is not null}");
                Publish(MeshMonitorEventKind.ProtocolError,
                    peerRid: sourceNodeRid);
            }
            return;
        }
        if (ready.Role != 1)
            return;
        ICanonicalRelocationReservationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationReservationTarget;
            _peersByRid.TryGetValue(sourceNodeRid, out peer);
        }
        if (target is null || peer is null || !peer.Admitted
            || peer.LifecycleGeneration != ready.SourceNodeGeneration)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => ProcessRelocationReadyAsync(target, peer, sourceNodeRid, ready));
    }

    private void ProcessRelocationReserved(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationReservedRecord reserved)
    {
        var key = new PendingRelocationReservationKey(sourceNodeRid,
            reserved.RelocationId, reserved.TargetAttemptGeneration,
            reserved.Coordinator);
        //  여기서 어긋나면 `pending.Reserved`가 끝내 완료되지 않아 source가
        //  reservation deadline을 소진한다. 그런데 실패해도 monitor event만
        //  남아 "key를 못 찾았는지" "찾았는데 내용이 안 맞는지" 구분할 수 없다.
        //  둘을 나눠 남긴다.
        var found = _pendingRelocationReservations.TryGetValue(key, out var pending);
        var matches = pending is not null && pending.MatchesReserved(reserved);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"canonical_reserved_applied found={found} matches={matches} "
            + $"source={sourceNodeRid} "
            + $"relocation={reserved.RelocationId.High:x16}{reserved.RelocationId.Low:x16} "
            + $"attempt={reserved.TargetAttemptGeneration}");
        if (pending is not null && matches)
            pending.Reserved.TrySetResult(reserved);
        else
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
    }

    private async Task ProcessRelocationReadyAsync(
        ICanonicalRelocationReservationTarget target,
        Peer peer,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationReadyRecord ready)
    {
        try
        {
            var reserved = await target.AcceptAsync(ready, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            if (!await SendServiceTerminalAsync(
                    sourceNodeRid,
                    [ZLinkServiceWireCodec.EncodeRelocationReserved(reserved)])
                .ConfigureAwait(false))
            {
                Publish(MeshMonitorEventKind.Backpressured,
                    peerRid: sourceNodeRid);
                return;
            }
            if (target.TryCreateSealRequest(ready.RelocationId,
                    ready.TargetAttemptGeneration, out var seal))
            {
                await SendCanonicalRelocationRecordAsync(peer,
                        ZLinkServiceWireCodec.EncodeRelocationSeal(seal),
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
            }
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-acceptance",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationData(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data)
    {
        ICanonicalRelocationReservationTarget? target;
        Peer? peer;
        lock (_gate)
        {
            target = _canonicalRelocationReservationTarget;
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
        ICanonicalRelocationReservationTarget target,
        Peer peer,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data)
    {
        try
        {
            var ack = await target.StageDataAsync(data, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            await SendCanonicalRelocationRecordAsync(peer,
                    ZLinkServiceWireCodec.EncodeRelocationAck(ack),
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            if (target.TryCreateSealRequest(data.RelocationId,
                    data.TargetAttemptGeneration, out var seal))
                await SendCanonicalRelocationRecordAsync(peer,
                        ZLinkServiceWireCodec.EncodeRelocationSeal(seal),
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
        }
        catch
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationAck(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationAckRecord ack)
    {
        var key = new PendingRelocationAttemptKey(sourceNodeRid,
            ack.RelocationId, ack.TargetAttemptGeneration, ack.Coordinator);
        Peer? peer;
        lock (_gate) _peersByRid.TryGetValue(sourceNodeRid, out peer);
        if (ack.SenderRole == 2
            && _pendingRelocationAttempts.TryGetValue(key, out var pending)
            && pending.MatchesPeer(peer)
            && pending.AcceptAck(ack)) return;
        Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
    }

    private void ProcessRelocationSeal(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationSealRecord seal)
    {
        if (seal.Response && seal.SenderRole == 2)
        {
            var key = new PendingRelocationAttemptKey(sourceNodeRid,
                seal.RelocationId, seal.TargetAttemptGeneration,
                seal.Coordinator);
            Peer? peer;
            lock (_gate) _peersByRid.TryGetValue(sourceNodeRid, out peer);
            if (_pendingRelocationAttempts.TryGetValue(key, out var pending)
                && pending.MatchesPeer(peer)
                && pending.AcceptTargetSealAcknowledgement(seal))
                return;
            Publish(MeshMonitorEventKind.ProtocolError,
                peerRid: sourceNodeRid);
            return;
        }
        if (!seal.Response)
        {
            var key = new PendingRelocationAttemptKey(sourceNodeRid,
                seal.RelocationId, seal.TargetAttemptGeneration,
                seal.Coordinator);
            Peer? peer;
            lock (_gate) _peersByRid.TryGetValue(sourceNodeRid, out peer);
            if (peer is null || !peer.Admitted || seal.SenderRole != 2
                || seal.Participants.Count != 0
                || !_pendingRelocationAttempts.TryGetValue(key,
                    out var pending)
                || !pending.MatchesPeer(peer))
            {
                Publish(MeshMonitorEventKind.ProtocolError,
                    peerRid: sourceNodeRid);
                return;
            }
            RunInboundOperation(
                () => RespondRelocationSealAsync(peer, pending, seal));
            return;
        }
        ICanonicalRelocationReservationTarget? target;
        lock (_gate) target = _canonicalRelocationReservationTarget;
        if (target is null || seal.SenderRole != 1)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => AcceptRelocationSealAsync(target, sourceNodeRid, seal));
    }

    private async Task RespondRelocationSealAsync(Peer peer,
        PendingRelocationAttempt pending,
        ZLinkServiceWireCodec.RelocationSealRecord request)
    {
        var response = pending.CreateSealResponse(request);
        if (!pending.TryStartSealResponseRetries()) return;
        await RetryRelocationSealResponseAsync(peer, pending, response)
            .ConfigureAwait(false);
    }

    private async Task RetryRelocationSealResponseAsync(Peer peer,
        PendingRelocationAttempt pending,
        ZLinkServiceWireCodec.RelocationSealRecord response)
    {
        try
        {
            var encoded = ZLinkServiceWireCodec.EncodeRelocationSeal(response);
            while (!pending.SealResponseRetriesStopped)
            {
                await SendCanonicalRelocationRecordAsync(peer, encoded,
                        _stop?.Token ?? CancellationToken.None)
                    .ConfigureAwait(false);
                pending.SealResponseSent.TrySetResult();
                await Task.WhenAny(
                        pending.SealResponseRetryStop,
                        Task.Delay(RelocationAckRetryInterval,
                            _stop?.Token ?? CancellationToken.None))
                    .ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
            when (_stop?.IsCancellationRequested == true)
        {
            pending.StopSealResponseRetries();
        }
        catch (Exception exception)
        {
            pending.SealResponseSent.TrySetException(exception);
            Publish(MeshMonitorEventKind.ProtocolError,
                peerRid: peer.RoutingId);
        }
    }

    private async Task AcceptRelocationSealAsync(
        ICanonicalRelocationReservationTarget target,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationSealRecord seal)
    {
        try
        {
            await target.AcceptSealResponseAsync(seal, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            Peer? peer;
            lock (_gate) _peersByRid.TryGetValue(sourceNodeRid, out peer);
            if (peer is null || !peer.Admitted)
                return;
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    ZLinkServiceWireCodec.EncodeRelocationSeal(
                        seal with { SenderRole = 2 }),
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-seal",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
    }

    private void ProcessRelocationComplete(RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationCompleteRecord complete)
    {
        if (complete.SenderRole == 2)
        {
            var key = new PendingRelocationAttemptKey(
                sourceNodeRid,
                complete.RelocationId,
                complete.TargetAttemptGeneration,
                complete.Coordinator);
            if (!_pendingRelocationAttempts.TryGetValue(key, out var pending)
                || !pending.AcceptCompleteAcknowledgement(complete))
                Publish(
                    MeshMonitorEventKind.ProtocolError,
                    peerRid: sourceNodeRid);
            return;
        }
        ICanonicalRelocationReservationTarget? target;
        lock (_gate) target = _canonicalRelocationReservationTarget;
        if (target is null || complete.SenderRole != 1
            || complete.Coordinator.NodeRid != sourceNodeRid)
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
            return;
        }
        RunInboundOperation(
            () => CompleteRelocationAsync(target, sourceNodeRid, complete));
    }

    private async Task CompleteRelocationAsync(
        ICanonicalRelocationReservationTarget target,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.RelocationCompleteRecord complete)
    {
        try
        {
            await target.CompleteAsync(complete, sourceNodeRid,
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
            Peer? peer;
            lock (_gate) _peersByRid.TryGetValue(sourceNodeRid, out peer);
            if (peer is null || !peer.Admitted)
                return;
            await SendCanonicalRelocationRecordAsync(
                    peer,
                    ZLinkServiceWireCodec.EncodeRelocationComplete(
                        complete with { SenderRole = 2 }),
                    _stop?.Token ?? CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "canonical-relocation-complete",
                exception);
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceNodeRid);
        }
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
                Reply(
                    target is null ? RequestResult.InvalidState : RequestResult.ProtocolError,
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
                RequestResult.TimedOut,
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
                    RequestResult.TimedOut,
                    ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
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

    private void ProcessStateful(
        RoutingId sourceRid,
        ZLinkServiceWireCodec.StatefulRecord stateful,
        Received received,
        ref ZLinkInboundReceivePermit? receivePermit)
    {
        lock (_gate)
            if (!_peersByRid.TryGetValue(sourceRid, out var peer)
                || !peer.Admitted)
            {
                //  A request dropped here never reaches any staleness check and
                //  simply times out at the caller.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"stateful_dropped reason=peer_not_admitted actor={stateful.TargetActor}");
                return;
            }
        var request = stateful.Command is ServiceWireConstants.Command.SpotRequest
            or ServiceWireConstants.Command.ActorRequest;
        if (request
            && (received.MessageType != ReceivedMessageType.Request
                || received.RequestSeq is null))
        {
            Publish(MeshMonitorEventKind.ProtocolError, peerRid: sourceRid);
            return;
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
            return;
        }
        if (request
            && stateful.DeadlineUnixMs
               <= checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
        {
            Reply(
                RequestResult.TimedOut,
                (uint)ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut,
                Array.Empty<Message>());
            return;
        }
        var payloadOffset = stateful.HasMetadata ? 2 : 1;
        if (received.Parts.Count != payloadOffset + 1)
        {
            if (request)
                Reply(
                    RequestResult.ProtocolError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                    Array.Empty<Message>());
            return;
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
            var hasTargetSpot = _spots.TryGetValue(stateful.TargetSpotId, out var spot);
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
                    Reply(
                        RequestResult.Conflict,
                        (uint)ServiceWireConstants.FrameworkErrorCode.SpotRouteNotFound,
                        Array.Empty<Message>());
                return;
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
                    return;
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
                return;
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
            return;
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
        EnqueueOwned(
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
            ref receivePermit);
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
                RequestResult.Conflict,
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
        if (deadlineUnixMs <= checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.TimedOut,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
            return;
        }
        if (target is null)
        {
            SendUserSpotFailure(
                sourceRid,
                correlation,
                record.Command,
                RequestResult.InvalidState,
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
                        RequestResult.Busy,
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
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0)
            return new UserSpotOperationTerminal(
                RequestResult.TimedOut,
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
                RequestResult.TimedOut,
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
                || terminal.FailureCode == ServiceWireConstants.FrameworkErrorCode.None)
                throw new InvalidOperationException(
                    "A failed User Spot operation requires one failure code and no success completion.");
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
            wire.Add(EncodeFrameworkMultipartForSend(
                sourceRid,
                replyParts,
                head.Length));
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
        while (!deadline.IsCancellationRequested)
        {
            RoutingId target;
            lock (_gate)
                target = _peersByRid.TryGetValue(sourceRid, out var peer)
                    && peer.Admitted
                    ? peer.PhysicalRoutingId
                    : sourceRid;

            var sendReadyVersion = Volatile.Read(ref _sendReadyVersion);
            switch (TrySendOutcome(target, wire, SendFlags.DontWait))
            {
                case MeshSendOutcome.Accepted:
                    return true;
                case MeshSendOutcome.Stale:
                case MeshSendOutcome.PermanentFailure:
                    return false;
                case MeshSendOutcome.Backpressured:
                    break;
                default:
                    throw new ArgumentOutOfRangeException();
            }

            try
            {
                await WaitForSendReadyAsync(
                        sendReadyVersion,
                        deadline.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
        return false;
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
            var remaining = Math.Max(
                0,
                retentionDeadline - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            await Task.Delay(
                    TimeSpan.FromMilliseconds(remaining),
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
                    RequestResult.Conflict,
                    ServiceWireConstants.FrameworkErrorCode.SpotCreateFailed),
            ZLinkFrameworkErrorKind.CapacityExceeded =>
                new UserSpotOperationTerminal(
                    RequestResult.Busy,
                    ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull),
            ZLinkFrameworkErrorKind.ProtocolError =>
                new UserSpotOperationTerminal(
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
            _ => new UserSpotOperationTerminal(
                exception.RetryAdvice != ZLinkRetryAdvice.DoNotRetry ? RequestResult.Busy : RequestResult.InternalError,
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
                RequestResult.Conflict,
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
            <= checked((ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.TimedOut,
                ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut);
            return;
        }
        if (target is null)
        {
            SendActorCreateFailure(
                sourceRid,
                operation.Correlation,
                RequestResult.InvalidState,
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
                        RequestResult.Busy,
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
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0)
            return new ActorCreateOperationTerminal(
                RequestResult.TimedOut,
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
                RequestResult.TimedOut,
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
                || terminal.FailureCode == ServiceWireConstants.FrameworkErrorCode.None)
                throw new InvalidOperationException(
                    "A failed Actor create operation requires one failure code and no success completion.");
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
            wire.Add(EncodeFrameworkMultipartForSend(
                sourceRid,
                replyParts,
                head.Length));
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
            var remaining = Math.Max(
                0,
                retentionDeadline - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            await Task.Delay(
                    TimeSpan.FromMilliseconds(remaining),
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
            ZLinkFrameworkErrorKind.CapacityExceeded =>
                new ActorCreateOperationTerminal(
                    RequestResult.Busy,
                    ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull),
            ZLinkFrameworkErrorKind.ProtocolError =>
                new ActorCreateOperationTerminal(
                    RequestResult.ProtocolError,
                    ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
            _ => new ActorCreateOperationTerminal(
                exception.RetryAdvice != ZLinkRetryAdvice.DoNotRetry ? RequestResult.Busy : RequestResult.InternalError,
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
            peer = _peerAdmission.FindForAdmission(
                       _peersByRid,
                       _peersByIntent.Values,
                       sourceRid,
                       command,
                       admission.AdvertisedEndpoint)
                   ?? new Peer(
                       checked(++_nextIntent),
                       admission.AdvertisedEndpoint,
                       sourceRid,
                       _peerExpectations.TryGetValue(
                           sourceRid,
                           out var expectedInbound)
                           ? expectedInbound.SecurityIdentity
                           : DefaultInboundSecurityIdentity,
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
                    DefaultInboundSecurityIdentity,
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
                _peerControlRetry.RemoveTarget(peer.PhysicalRoutingId);
                _peerControlRetry.RemoveTarget(sourceRid);
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
                        RetireDuplicatePeer(peer);
                        keepPeer = notRequiredDuplicate;
                        publishNotRequired = false;
                    }
                    else
                    {
                        RetireDuplicatePeer(notRequiredDuplicate);
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
                    RetireDuplicatePeer(peer);
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
                    RetireDuplicatePeer(duplicate);
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
            _peerControlRetry.RemoveTarget(peer.PhysicalRoutingId);
            _peerControlRetry.RemoveTarget(sourceRid);
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
        FlushPendingInfrastructureCompletions();
        Peer[] peers;
        lock (_gate)
            peers = _peersByIntent.Values.ToArray();

        foreach (var peer in peers)
        {
            if (peer.State == MeshPeerState.NotRequired)
                continue;
            if (!peer.Admitted)
            {
                if (now >= peer.NextAdmissionTimestamp)
                {
                    peer.NextAdmissionTimestamp = Add(now, AdmissionRetryInterval);
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
                    _peerControlRetry.RemoveTarget(peer.PhysicalRoutingId);
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

        if (Interlocked.Exchange(ref _peerControlRetryReady, 0) != 0)
        {
            _peerControlRetry.Flush(FlushPeerControlRetry);
            // Core normally emits another send-ready edge after a retry is
            // accepted. If the edge observed above still reports
            // back-pressure, retain the owner work signal so the bounded
            // receive-loop poll retries it without depending on a second
            // edge that may already have been coalesced.
            if (_peerControlRetry.Count != 0)
                Interlocked.Exchange(ref _peerControlRetryReady, 1);
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
        out MeshOperationId operationId,
        RequestCallback? directCompletion = null)
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
                directCompletion))
        {
            operationId = default;
            Publish(MeshMonitorEventKind.Backpressured, peerRid: targetRid);
            return SubmitResult.Backpressured;
        }
        operationId = pending.OperationId;

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
            return result;
        }

        // Local requests have no native request window, so the managed timeout
        // remains their terminal owner. Remote requests complete through the
        // binding callback driven by Core's Completion connection.
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
        var wire = new Message[metadata.IsEmpty ? 2 : 3];
        var created = 0;
        try
        {
            var head = ZLinkServiceWireCodec.EncodeApplication(
                command,
                pending.OperationId.Low,
                channelName,
                !metadata.IsEmpty);
            wire[created] = Message.From(head);
            created++;
            if (!metadata.IsEmpty)
            {
                wire[created] = Message.From(metadata);
                created++;
            }
            wire[created] = Message.From(
                EncodeFrameworkMultipartForSend(
                    peer.PhysicalRoutingId,
                    parts,
                    checked(head.Length + metadata.Length)));
            created++;

            bool submitted;
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    return SubmitResult.Terminated;
                submitted = socket.Request(peer.PhysicalRoutingId)
                    .Messages(wire)
                    .Timeout(timeout)
                    .Flags(flags)
                    .Submit((result, replyParts) =>
                        CompleteNativeApplicationRequest(
                            pending,
                            result,
                            replyParts));
            }
            if (!submitted)
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
        catch (ZLinkFrameworkException exception) when (
            exception.Kind == ZLinkFrameworkErrorKind.CapacityExceeded)
        {
            // The complete application message is known to exceed the
            // admitted transport bound before Core can receive its header.
            // Complete the request through the normal terminal path so the
            // caller observes the public protocol error and no handler runs.
            CompleteManagedOperation(
                pending,
                RequestResult.ProtocolError,
                (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                Array.Empty<Message>());
            return SubmitResult.Ok;
        }
        catch (ZlinkException)
        {
            return SubmitResult.Terminated;
        }
        finally
        {
            for (var index = 0; index < created; index++)
                wire[index].Dispose();
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
        TimeSpan timeout)
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

        if (!TryCreateOperation(kind, out var correlation, out var pending))
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

        if (!TrySend(peer.PhysicalRoutingId, [head], SendFlags.None))
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
            var retained = parts.Select(Message.From).ToArray();
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
                    retained.Length,
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
        wireParts.Add(EncodeFrameworkMultipartForSend(
            peer.PhysicalRoutingId,
            parts,
            checked(head.Length + metadata.Length)));
        var sent = TrySend(peer.PhysicalRoutingId, wireParts, flags);
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

    private SubmitResult SendReply(
        RoutingId targetRid,
        ulong correlation,
        IReadOnlyList<Message> parts,
        SendFlags flags) =>
        SendTerminalReply(
            targetRid,
            correlation,
            RequestResult.Ok,
            0,
            parts,
            flags);

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
                wire[created++] = Message.From(
                    EncodeFrameworkMultipartForSend(
                        targetRid,
                        parts,
                        wire[0].Size));
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
                wire[created++] = Message.From(
                    EncodeFrameworkMultipartForSend(
                        targetRid,
                        parts,
                        wire[0].Size));

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
            wireParts.Add(
                EncodeFrameworkMultipartForSend(
                    targetRid,
                    parts,
                    wireParts[0].Length));
        return TrySend(targetRid, wireParts, flags)
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
            if (pending.DirectCompletion is { } directCompletion)
                directCompletion(RequestResult.TimedOut, Array.Empty<Message>());
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
        var queued = new QueuedRecord(
            new MeshReceiveRecord(
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
                kindData),
            parts);
        var currentCompletionReplaced = false;
        MeshReceiveRecord? currentOverflowFailure = null;
        List<MeshReceiveRecord>? overflowFailures = null;
        lock (_pendingCompletionGate)
        {
            if (_pendingInfrastructureCompletions.Count != 0
                || !TryEnqueueInfrastructureCompletionUnderLock(queued))
            {
                if (!TryRetainPendingInfrastructureCompletionUnderLock(
                        queued,
                        out currentCompletionReplaced,
                        out overflowFailures))
                {
                    if (currentCompletionReplaced)
                    {
                        // A Backpressured marker is still a retained record and
                        // therefore has the fixed mailbox cost. When the
                        // configured byte budget is smaller than that cost, the
                        // marker cannot be queued; complete it through the
                        // internal overflow sink instead.
                        currentOverflowFailure =
                            MeshReceiveRecord.CompletionFailure(
                                operationId,
                                operationKind,
                                RequestResult.Backpressured);
                    }
                }
            }
        }
        if (currentCompletionReplaced)
            RecordCompletionRetentionOverflow(operationId);
        var overflowHandler = Volatile.Read(ref _completionOverflowHandler);
        if (currentOverflowFailure is { } currentFailure
            && overflowHandler is not null)
        {
            try
            {
                overflowHandler(currentFailure, Array.Empty<Message>());
            }
            catch (Exception exception)
            {
                ZLinkFrameworkDebugLog.TaskFailure(
                    "completion-overflow-handler",
                    exception);
            }
        }
        if (overflowFailures is not null)
        {
            foreach (var overflowFailure in overflowFailures)
            {
                RecordCompletionRetentionOverflow(overflowFailure.OperationId);
                if (overflowHandler is not null)
                {
                    try
                    {
                        overflowHandler(overflowFailure, Array.Empty<Message>());
                    }
                    catch (Exception exception)
                    {
                        ZLinkFrameworkDebugLog.TaskFailure(
                            "completion-overflow-handler",
                            exception);
                    }
                }
            }
        }
        SignalReadyIfNeeded();
        if (publishEvent)
            Publish(
                MeshMonitorEventKind.OperationCompleted,
                operationId: operationId,
                resultCode: currentCompletionReplaced
                    ? (int)RequestResult.Backpressured
                    : result,
                failureErrno: currentCompletionReplaced ? 0 : failure);
    }

    private bool TryEnqueueInfrastructureCompletionUnderLock(QueuedRecord queued)
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

    private void FlushPendingInfrastructureCompletions()
    {
        var enqueued = false;
        lock (_pendingCompletionGate)
        {
            while (_pendingInfrastructureCompletions.TryPeek(out var queued))
            {
                if (!TryEnqueueInfrastructureCompletionUnderLock(queued))
                    break;
                _pendingInfrastructureCompletions.Dequeue();
                RemovePendingInfrastructureCompletionAccounting(queued);
                enqueued = true;
            }
        }
        if (enqueued)
            SignalReadyIfNeeded();
    }

    private void DisposePendingInfrastructureCompletions()
    {
        lock (_pendingCompletionGate)
        {
            while (_pendingInfrastructureCompletions.TryDequeue(out var queued))
            {
                RemovePendingInfrastructureCompletionAccounting(queued);
                queued.Dispose();
            }
        }
    }

    private bool TryRetainPendingInfrastructureCompletionUnderLock(
        QueuedRecord queued,
        out bool currentCompletionReplaced,
        out List<MeshReceiveRecord>? overflowFailures)
    {
        currentCompletionReplaced = false;
        overflowFailures = null;
        var byteBudget = PendingInfrastructureCompletionByteBudget;
        if (queued.PendingBytes > byteBudget)
        {
            queued = ReplaceWithBackpressuredCompletion(queued);
            currentCompletionReplaced = true;
            if (queued.PendingBytes > byteBudget)
            {
                queued.Dispose();
                return false;
            }
        }

        while (_pendingInfrastructureCompletions.Count
                   >= PendingInfrastructureCompletionRecordBudget
               || !CanAddPendingInfrastructureCompletionBytes(
                   queued.PendingBytes,
                   byteBudget))
        {
            if (!_pendingInfrastructureCompletions.TryDequeue(out var evicted))
                break;
            RemovePendingInfrastructureCompletionAccounting(evicted);
            evicted.Dispose();
            overflowFailures ??= [];
            overflowFailures.Add(
                MeshReceiveRecord.CompletionFailure(
                    evicted.Record.OperationId,
                    evicted.Record.OperationKind,
                    RequestResult.Backpressured));
        }

        if (_pendingInfrastructureCompletions.Count
            >= PendingInfrastructureCompletionRecordBudget)
        {
            queued.Dispose();
            return false;
        }

        _pendingInfrastructureCompletions.Enqueue(queued);
        AddPendingInfrastructureCompletionAccounting(queued);
        return true;
    }

    private QueuedRecord ReplaceWithBackpressuredCompletion(
        QueuedRecord queued)
    {
        var failure = QueuedRecordForCompletionFailure(queued.Record);
        queued.Dispose();
        return failure;
    }

    private static QueuedRecord QueuedRecordForCompletionFailure(
        MeshReceiveRecord record) =>
        new(
            MeshReceiveRecord.CompletionFailure(
                record.OperationId,
                record.OperationKind,
                RequestResult.Backpressured),
            Array.Empty<Message>());

    private void AddPendingInfrastructureCompletionAccounting(
        QueuedRecord queued)
    {
        Interlocked.Increment(ref _pendingInfrastructureCompletionCount);
        Interlocked.Add(
            ref _pendingInfrastructureCompletionBytes,
            checked((long)queued.PendingBytes));
    }

    private void RemovePendingInfrastructureCompletionAccounting(
        QueuedRecord queued)
    {
        Interlocked.Decrement(ref _pendingInfrastructureCompletionCount);
        Interlocked.Add(
            ref _pendingInfrastructureCompletionBytes,
            -checked((long)queued.PendingBytes));
    }

    private bool CanAddPendingInfrastructureCompletionBytes(
        ulong pendingBytes,
        ulong byteBudget)
    {
        if (pendingBytes > byteBudget)
            return false;
        var current = checked((ulong)Math.Max(
            0,
            Volatile.Read(ref _pendingInfrastructureCompletionBytes)));
        return pendingBytes <= byteBudget - Math.Min(current, byteBudget);
    }

    private int PendingInfrastructureCompletionRecordBudget =>
        Math.Max(1, _maxPendingOperations);

    private ulong PendingInfrastructureCompletionByteBudget =>
        MailboxByteBudget == 0
            ? checked((ulong)DefaultPendingCompletionByteBudget)
            : Math.Min(MailboxByteBudget, (ulong)long.MaxValue);

    private void RecordCompletionRetentionOverflow(MeshOperationId operationId)
    {
        ZLinkRuntimeMetrics.RecordMessageDropped(
            _meshName,
            "completion",
            "reply",
            "capacity_exceeded");
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"completion_retention_capacity_exceeded operation="
            + $"{operationId.High:x16}{operationId.Low:x16}");
        Publish(MeshMonitorEventKind.Backpressured, operationId: operationId);
    }

    private void EnqueueSendReady()
    {
        SignalSendReady();
        Interlocked.Exchange(ref _peerControlRetryReady, 1);
        EnqueueOwned(
            MailboxKey.ForNode(MeshReadyDomains.Infrastructure),
            new MeshReceiveRecord(
                MeshRecordKind.SendReady,
                MeshReadyDomains.Infrastructure,
                default,
                string.Empty,
                0,
                default,
                default,
                default,
                null,
                null,
                null,
                0,
                0,
                0,
                0,
                new MeshSendReadyData(
                    MeshDestinationKind.Node,
                    default,
                    string.Empty,
                    default,
                    null)),
            Array.Empty<Message>());
    }

    private void SignalSendReady()
    {
        TaskCompletionSource pulse;
        lock (_sendReadyGate)
        {
            _sendReadyVersion = checked(_sendReadyVersion + 1);
            pulse = _sendReadyPulse;
            _sendReadyPulse = NewSendReadyPulse();
        }
        pulse.TrySetResult();
    }

    private Task WaitForSendReadyAsync(
        long observedVersion,
        CancellationToken cancellationToken)
    {
        lock (_sendReadyGate)
        {
            if (_sendReadyVersion != observedVersion)
                return Task.CompletedTask;
            return _sendReadyPulse.Task.WaitAsync(cancellationToken);
        }
    }

    private static TaskCompletionSource NewSendReadyPulse() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private void EnqueueOwned(
        MailboxKey key,
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        bool admitApplication = false)
    {
        ZLinkInboundReceivePermit? receivePermit = null;
        EnqueueOwned(
            key,
            record,
            parts,
            admitApplication,
            ref receivePermit);
    }

    private void EnqueueOwned(
        MailboxKey key,
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        bool admitApplication,
        ref ZLinkInboundReceivePermit? receivePermit)
    {
        var queued = new QueuedRecord(record, parts);
        try
        {
            if (admitApplication
                && record.RequiresApplicationDispatchLease
                && _inboundDispatchBudget is { } budget)
            {
                ZLinkInboundDispatchLease lease;
                if (receivePermit is not null)
                {
                    lease = budget.TrackReceived(
                        receivePermit,
                        queued.PayloadBytes);
                    budget.CompleteReceiveAttempt(receivePermit);
                    receivePermit = null;
                    queued.AttachLease(lease);
                }
                // A local send or a fanout copy has no raw receive permit.
                // Keep it in the bounded mailbox and acquire the application
                // HWM lease when the dispatch pump claims that mailbox. A
                // synchronous wait here would hold the receive loop while it
                // is supposed to continue processing infrastructure control.
            }

            var mailbox = _ownedMailboxes.GetOrAdd(
                key,
                _ => new OwnedMailbox(
                    RecordOwnedRecordEnqueued,
                    RecordOwnedRecordDequeued));
            if (!mailbox.TryEnqueue(
                    queued,
                    MailboxMessageBudget,
                    MailboxByteBudget))
            {
                RecordInboundBackpressureDrop(record.Kind);
                queued.Dispose();
                Publish(MeshMonitorEventKind.Backpressured);
                return;
            }
            SignalReadyIfNeeded();
        }
        catch
        {
            queued.Dispose();
            throw;
        }
    }

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
            batch.Add(queued.Record, queued.TakeParts());
            count++;
        }
        return count > 0;
    }

    private void ReleaseOwnedMailbox(OwnedMailbox mailbox)
    {
        mailbox.Release();
        FlushPendingInfrastructureCompletions();
        if (mailbox.HasRecords)
            SignalReadyIfNeeded();
    }

    private void SignalReadyIfNeeded()
    {
        if ((!_ownedMailboxes.Values.Any(static mailbox => mailbox.HasRecords)
             && Volatile.Read(ref _pendingInfrastructureCompletionCount) == 0)
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
        uint effectiveMaxMessageBytes;
        lock (_gate)
        {
            descriptorRevision = _descriptorRevision;
            channels = new Dictionary<string, uint>(_channels, StringComparer.Ordinal);
            effectiveMaxMessageBytes = _localEffectiveMaxMessageBytes;
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
            DefaultInboundSecurityIdentity,
            effectiveMaxMessageBytes);
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
        var intentVersion = _peerControlRetry.NextIntentVersion();
        var outcome = TrySendOutcome(target, [head], SendFlags.DontWait);
        switch (outcome)
        {
            case MeshSendOutcome.Accepted:
                _peerControlRetry.RemoveUpTo(
                    target,
                    connectionGeneration,
                    command,
                    intentVersion);
                return true;
            case MeshSendOutcome.Backpressured:
                if (_peerControlRetry.TryRemember(
                        target,
                        connectionGeneration,
                        command,
                        head,
                        intentVersion))
                {
                    return false;
                }
                ZLinkRuntimeMetrics.RecordMessageDropped(
                    _meshName,
                    "mesh",
                    "control",
                    "retry_capacity");
                // A bounded infrastructure retry queue cannot discard an
                // admission or liveness record while leaving the connection
                // looking usable. Close this exact connection so the peer
                // observes PeerClosed and the normal admission path can
                // establish a fresh progress record.
                ClosePeerAfterControlRetryCapacity(target, connectionGeneration);
                return false;
            case MeshSendOutcome.Stale:
            case MeshSendOutcome.PermanentFailure:
                return false;
            default:
                throw new ArgumentOutOfRangeException();
        }
    }

    private void ClosePeerAfterControlRetryCapacity(
        RoutingId physicalRoutingId,
        ulong connectionGeneration)
    {
        lock (_gate)
        {
            var peer = _peersByIntent.Values.FirstOrDefault(candidate =>
                candidate.PhysicalRoutingId == physicalRoutingId
                && candidate.ConnectionGeneration == connectionGeneration
                && candidate.State != MeshPeerState.Closed);
            if (peer is not null)
                RemovePeer(peer, disconnect: true);
        }
    }

    private bool TrySend(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        SendFlags flags)
        => TrySendOutcome(target, parts, flags) == MeshSendOutcome.Accepted;

    private ZLinkMeshPeerControlRetryResult FlushPeerControlRetry(
        RoutingId target,
        ulong connectionGeneration,
        byte[] payload)
    {
        lock (_gate)
        {
            var current = _peersByIntent.Values.FirstOrDefault(peer =>
                peer.PhysicalRoutingId == target
                && peer.ConnectionGeneration == connectionGeneration
                && peer.State != MeshPeerState.Closed);
            if (current is null)
                return ZLinkMeshPeerControlRetryResult.Stale;
        }

        return TrySendOutcome(target, [payload], SendFlags.DontWait) switch
        {
            MeshSendOutcome.Accepted =>
                ZLinkMeshPeerControlRetryResult.Accepted,
            MeshSendOutcome.Backpressured =>
                ZLinkMeshPeerControlRetryResult.Backpressured,
            MeshSendOutcome.Stale =>
                ZLinkMeshPeerControlRetryResult.Stale,
            _ => ZLinkMeshPeerControlRetryResult.PermanentFailure
        };
    }

    private enum MeshSendOutcome
    {
        Accepted,
        Backpressured,
        Stale,
        PermanentFailure
    }

    private byte[] EncodeFrameworkMultipartForSend(
        RoutingId target,
        IReadOnlyList<Message> parts,
        int otherPartBytes)
    {
        var maximumEncodedBytes = checked(
            (long)GetEffectiveSendMessageBound(target) - otherPartBytes);
        return ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
            parts,
            maximumEncodedBytes);
    }

    private byte[] EncodeFrameworkMultipartForSend(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        int otherPartBytes)
    {
        var maximumEncodedBytes = checked(
            (long)GetEffectiveSendMessageBound(target) - otherPartBytes);
        return ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
            parts,
            maximumEncodedBytes);
    }

    private MeshSendOutcome TrySendOutcome(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        SendFlags flags)
    {
        if (!FitsCompleteMessageBound(parts, GetEffectiveSendMessageBound(target)))
            return MeshSendOutcome.PermanentFailure;
        if (TryGetCompletionControlCommand(parts, out var completionCommand)
            && (parts.Count > MaxCompletionControlParts
                || !IsCompletionControlFrameShape(completionCommand, parts.Count)
                || !IsWithinCompletionControlBounds(parts, completionCommand)
                || !IsValidOptionalApplicationPayload(parts, completionCommand)))
            return MeshSendOutcome.PermanentFailure;

        var messages = new Message[parts.Count];
        var created = 0;
        try
        {
            for (; created < messages.Length; created++)
                messages[created] = Message.From(parts[created]);
            lock (_socketGate)
            {
                var socket = _socket;
                if (socket is null
                    || _activeSocketGeneration != _lifecycleGeneration)
                    return MeshSendOutcome.Stale;
                if (ShouldUseCompletionControl(parts))
                    return socket.TrySendCompletionControl(target, messages)
                        ? MeshSendOutcome.Accepted
                        : MeshSendOutcome.Backpressured;
                return socket.Send(target)
                    .Messages(messages)
                    .Flags(flags)
                    .Submit()
                    ? MeshSendOutcome.Accepted
                    : MeshSendOutcome.Backpressured;
            }
        }
        catch (ZlinkSubmitException exception)
        {
            return exception.Result == ZlinkSubmitException.ErrorCode.Backpressured
                ? MeshSendOutcome.Backpressured
                : MeshSendOutcome.PermanentFailure;
        }
        catch (ObjectDisposedException)
        {
            return MeshSendOutcome.Stale;
        }
        catch (ZlinkException)
        {
            return MeshSendOutcome.PermanentFailure;
        }
        finally
        {
            for (var index = 0; index < created; index++)
                messages[index].Dispose();
        }
    }

    private uint GetEffectiveSendMessageBound(RoutingId target)
    {
        lock (_gate)
        {
            var local = _localEffectiveMaxMessageBytes;
            var peer = _peersByIntent.Values.FirstOrDefault(candidate =>
                candidate.Admitted
                && (candidate.PhysicalRoutingId == target
                    || candidate.RoutingId == target));
            return peer?.Admission is { } admission
                ? Math.Min(local, admission.EffectiveMaxMessageBytes)
                : local;
        }
    }

    private static bool FitsCompleteMessageBound(
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        uint bound)
    {
        if (bound == 0)
            return false;
        long total = 0;
        foreach (var part in parts)
        {
            total = checked(total + part.Length);
            if (total > bound)
                return false;
        }
        return true;
    }

    private static bool FitsCompleteMessageBound(
        IReadOnlyList<Message> parts,
        uint bound)
    {
        if (bound == 0)
            return false;
        long total = 0;
        foreach (var part in parts)
        {
            total = checked(total + part.Size);
            if (total > bound)
                return false;
        }
        return true;
    }

    private void RetireDuplicatePeer(Peer peer)
    {
        var wasAdmitted = peer.Admitted;
        var physicalRoutingId = peer.PhysicalRoutingId;
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
            && _socket is not null)
            DisconnectTransport(
                peer,
                wasAdmitted,
                physicalRoutingId);
        _peerControlRetry.RemoveTarget(peer.PhysicalRoutingId);
    }

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
        _peerControlRetry.RemoveTarget(peer.PhysicalRoutingId);
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
                if (wasAdmitted && !physicalRoutingId.IsEmpty)
                {
                    // Endpoint termination is broader than one mesh lifetime.
                    // During a rolling RID handover it can terminate the old
                    // pipe while the replacement connect is being attached.
                    // An admitted peer has an exact physical RID, so retire
                    // only that pipe and leave other candidates on the endpoint
                    // untouched.
                    _socket!.DisconnectRid(physicalRoutingId);
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"mesh_peer_transport_disconnect local={_routingId} "
                        + $"peer={peer.RoutingId} mode=rid physical={physicalRoutingId}");
                    return;
                }

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
        RequestCallback? directCompletion = null)
    {
        private readonly CancellationTokenSource _timeout = new();
        private int _terminal;
        internal MeshOperationId OperationId { get; } = operationId;
        internal MeshOperationKind Kind { get; } = kind;
        internal ZLinkServiceWireCodec.RequestSourceFence RequestSource { get; } =
            requestSource;
        internal RequestCallback? DirectCompletion { get; } = directCompletion;
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
                _timeout.Cancel();
            _timeout.Dispose();
        }
    }

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

    private readonly record struct PendingRelocationReservationKey(
        RoutingId TargetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator);

    private readonly record struct PendingRelocationAttemptKey(
        RoutingId TargetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId RelocationId,
        ulong TargetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator);

    private sealed class PendingRelocationAttempt(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        Peer expectedPeer)
    {
        private readonly object _gate = new();
        private readonly Dictionary<ulong, ulong> _sentHighWater = new();
        private readonly Dictionary<ulong, TaskCompletionSource<ulong>> _acks =
            new();
        private readonly TaskCompletionSource _sealResponseRetryStop = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private byte[]? _expectedCompleteAcknowledgement;
        private int _sealResponseRetryStarted;

        internal TaskCompletionSource SealResponseSent { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource TargetSealAcknowledged { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource CompleteAcknowledged { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        internal Task SealResponseRetryStop => _sealResponseRetryStop.Task;
        internal bool SealResponseRetriesStopped =>
            _sealResponseRetryStop.Task.IsCompleted;

        internal bool TryStartSealResponseRetries() =>
            Interlocked.CompareExchange(ref _sealResponseRetryStarted, 1, 0) == 0;

        internal void StopSealResponseRetries() =>
            _sealResponseRetryStop.TrySetResult();

        internal bool MatchesPeer(Peer? peer) =>
            ReferenceEquals(peer, expectedPeer)
            && peer is { Admitted: true }
            && peer.LifecycleGeneration == prepare.Candidate.NodeGeneration;

        internal void Register(
            ZLinkServiceWireCodec.RelocationDataRecord record)
        {
            if (record.RelocationId != prepare.RelocationId
                || record.TargetAttemptGeneration
                   != prepare.TargetAttemptGeneration
                || record.Coordinator != prepare.Coordinator
                || record.SenderRole != 1
                || !prepare.Participants.Any(participant =>
                    participant.ParticipantId == record.ParticipantId
                    && record.Sequence <= participant.AllowanceMessages))
                throw new InvalidDataException(
                    "Command 31 does not match the sealed relocation attempt.");
            lock (_gate)
            {
                _sentHighWater[record.ParticipantId] = Math.Max(
                    _sentHighWater.GetValueOrDefault(record.ParticipantId),
                    record.Sequence);
                _acks.TryAdd(record.ParticipantId,
                    new TaskCompletionSource<ulong>(
                        TaskCreationOptions.RunContinuationsAsynchronously));
            }
        }

        internal bool AcceptAck(
            ZLinkServiceWireCodec.RelocationAckRecord ack)
        {
            lock (_gate)
            {
                if (!_sentHighWater.TryGetValue(ack.ParticipantId,
                        out var sent)
                    || ack.HighWater > sent
                    || !_acks.TryGetValue(ack.ParticipantId, out var completion))
                    return false;
                completion.TrySetResult(ack.HighWater);
                return true;
            }
        }

        internal bool AcceptCompleteAcknowledgement(
            ZLinkServiceWireCodec.RelocationCompleteRecord complete)
        {
            lock (_gate)
            {
                if (_expectedCompleteAcknowledgement is not { } expected
                    || complete.SenderRole != 2
                    || !CryptographicOperations.FixedTimeEquals(
                        expected,
                        SHA256.HashData(
                            ZLinkServiceWireCodec.EncodeRelocationComplete(
                                complete))))
                    return false;
                CompleteAcknowledged.TrySetResult();
                return true;
            }
        }

        internal bool AcceptTargetSealAcknowledgement(
            ZLinkServiceWireCodec.RelocationSealRecord seal)
        {
            lock (_gate)
            {
                if (!seal.Response
                    || seal.SenderRole != 2
                    || seal.RelocationId != prepare.RelocationId
                    || seal.TargetAttemptGeneration
                       != prepare.TargetAttemptGeneration
                    || seal.Coordinator != prepare.Coordinator)
                    return false;
                var expected = prepare.Participants
                    .OrderBy(static participant => participant.ParticipantId)
                    .Select(participant =>
                        new ZLinkServiceWireCodec
                            .RelocationParticipantTerminalRecord(
                                participant.ParticipantId,
                                prepare.Object.Kind == 1
                                    ? _sentHighWater.GetValueOrDefault(
                                        participant.ParticipantId)
                                    : participant.AllowanceMessages))
                    .ToArray();
                var actual = seal.Participants
                    .OrderBy(static participant => participant.ParticipantId)
                    .ToArray();
                if (!expected.SequenceEqual(actual))
                    return false;
                TargetSealAcknowledged.TrySetResult();
                return true;
            }
        }

        internal void RegisterComplete(
            ZLinkServiceWireCodec.RelocationCompleteRecord complete)
        {
            if (complete.SenderRole != 1
                || complete.RelocationId != prepare.RelocationId
                || complete.TargetAttemptGeneration
                   != prepare.TargetAttemptGeneration
                || complete.Coordinator != prepare.Coordinator)
                throw new InvalidDataException(
                    "Command 35 does not match the source relocation attempt.");
            var fingerprint = SHA256.HashData(
                ZLinkServiceWireCodec.EncodeRelocationComplete(
                    complete with { SenderRole = 2 }));
            lock (_gate)
            {
                if (_expectedCompleteAcknowledgement is { } prior
                    && !CryptographicOperations.FixedTimeEquals(
                        prior,
                        fingerprint))
                    throw new InvalidDataException(
                        "A command 35 retry changed fields.");
                _expectedCompleteAcknowledgement ??= fingerprint;
            }
        }

        internal async ValueTask WaitForAckAsync(
            ulong participantId,
            ulong sequence,
            CancellationToken cancellationToken)
        {
            Task<ulong> task;
            lock (_gate) task = _acks[participantId].Task;
            while (await task.WaitAsync(cancellationToken).ConfigureAwait(false)
                   < sequence)
            {
                lock (_gate)
                {
                    _acks[participantId] = new TaskCompletionSource<ulong>(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    task = _acks[participantId].Task;
                }
            }
        }

        internal ZLinkServiceWireCodec.RelocationSealRecord CreateSealResponse(
            ZLinkServiceWireCodec.RelocationSealRecord request)
        {
            lock (_gate)
            {
                if (request.Response
                    || request.SenderRole != 2
                    || request.RelocationId != prepare.RelocationId
                    || request.TargetAttemptGeneration
                       != prepare.TargetAttemptGeneration
                    || request.Coordinator != prepare.Coordinator
                    || request.Participants.Count != 0)
                    throw new InvalidDataException(
                        "Command 34 request does not match the sealed relocation attempt.");
                var ordered = prepare.Participants
                    .OrderBy(static participant => participant.ParticipantId)
                    .ToArray();
                var terminals = ordered
                    .Select(participant =>
                        new ZLinkServiceWireCodec.RelocationParticipantTerminalRecord(
                            participant.ParticipantId,
                            _sentHighWater.GetValueOrDefault(
                                participant.ParticipantId)))
                    .ToArray();
                for (var index = 0; index < terminals.Length; index++)
                    if (terminals[index].HighWater
                        > ordered[index].AllowanceMessages
                        || prepare.Object.Kind != 1
                           && terminals[index].HighWater
                           != ordered[index].AllowanceMessages)
                        throw new InvalidDataException(
                            "The target sealed before all command 31 records were acknowledged.");
                foreach (var participant in ordered)
                    if (_acks.TryGetValue(participant.ParticipantId,
                            out var completion))
                        completion.TrySetResult(
                            prepare.Object.Kind == 1
                                ? _sentHighWater.GetValueOrDefault(
                                    participant.ParticipantId)
                                : participant.AllowanceMessages);
                return request with
                {
                    SenderRole = 1,
                    Response = true,
                    Participants = terminals
                };
            }
        }

        internal ZLinkServiceWireCodec.RelocationSealRecord
            CreateFinalSealResponse()
        {
            lock (_gate)
            {
                var ordered = prepare.Participants
                    .OrderBy(static participant => participant.ParticipantId)
                    .ToArray();
                var terminals = ordered
                    .Select(participant =>
                        new ZLinkServiceWireCodec.RelocationParticipantTerminalRecord(
                            participant.ParticipantId,
                            _sentHighWater.GetValueOrDefault(
                                participant.ParticipantId)))
                    .ToArray();
                for (var index = 0; index < terminals.Length; index++)
                    if (terminals[index].HighWater
                        > ordered[index].AllowanceMessages
                        || prepare.Object.Kind != 1
                           && terminals[index].HighWater
                           != ordered[index].AllowanceMessages)
                        throw new InvalidDataException(
                            "The source sealed before all command 31 records were acknowledged.");
                return new ZLinkServiceWireCodec.RelocationSealRecord(
                    prepare.RelocationId,
                    prepare.TargetAttemptGeneration,
                    prepare.Coordinator,
                    1,
                    true,
                    terminals);
            }
        }
    }

    private sealed class PendingRelocationReservation(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        byte[] fingerprint,
        Func<Task<ZLinkServiceWireCodec.RelocationReservedRecord>> operation)
    {
        internal byte[] Fingerprint { get; } = fingerprint;
        internal Lazy<Task<ZLinkServiceWireCodec.RelocationReservedRecord>>
            Operation { get; } = new(operation,
                LazyThreadSafetyMode.ExecutionAndPublication);
        internal TaskCompletionSource<ZLinkServiceWireCodec.RelocationReadyRecord>
            Offer { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource<ZLinkServiceWireCodec.RelocationReservedRecord>
            Reserved { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal bool MatchesOffer(
            ZLinkServiceWireCodec.RelocationReadyRecord value)
        {
            if (value.Role != 2 || value.Participants.Count != 0
                || value.OfferedMessages == 0 || value.OfferedBytes == 0
                || value.RelocationId != prepare.RelocationId
                || value.TargetAttemptGeneration != prepare.TargetAttemptGeneration
                || value.RoundKind != prepare.RoundKind
                || value.Coordinator != prepare.Coordinator
                || value.Candidate != prepare.Candidate
                || value.Object != prepare.Object
                || value.SourceNodeGeneration != prepare.SourceNodeGeneration
                || value.TargetNodeGeneration != prepare.Candidate.NodeGeneration
                || value.ReservationGeneration == 0
                || value.Root != prepare.Root
                || value.ApplicationVersion != prepare.ApplicationVersion
                || value.ParticipantProgress.Count != prepare.Participants.Count)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"canonical_offer_mismatch expected_object={prepare.Object} actual_object={value.Object} "
                    + $"expected_candidate={prepare.Candidate} actual_candidate={value.Candidate} "
                    + $"expected_source_generation={prepare.SourceNodeGeneration} actual_source_generation={value.SourceNodeGeneration} "
                    + $"expected_target_generation={prepare.Candidate.NodeGeneration} actual_target_generation={value.TargetNodeGeneration} "
                    + $"expected_root={prepare.Root} actual_root={value.Root} "
                    + $"expected_application_version={prepare.ApplicationVersion} actual_application_version={value.ApplicationVersion} "
                    + $"expected_participants={prepare.Participants.Count} actual_progress={value.ParticipantProgress.Count}");
                return false;
            }
            if (prepare.RequiredMessages > value.OfferedMessages
                || prepare.RequiredBytes > value.OfferedBytes)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"canonical_offer_capacity_mismatch required_messages={prepare.RequiredMessages} "
                    + $"offered_messages={value.OfferedMessages} required_bytes={prepare.RequiredBytes} "
                    + $"offered_bytes={value.OfferedBytes}");
                return false;
            }
            ulong messages = 0;
            ulong bytes = 0;
            for (var index = 0; index < prepare.Participants.Count; index++)
            {
                var participant = prepare.Participants[index];
                var progress = value.ParticipantProgress[index];
                messages = checked(messages + participant.AllowanceMessages);
                bytes = checked(bytes + participant.AllowanceBytes);
                if (participant.ParticipantId != progress.ParticipantId)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"canonical_offer_participant_mismatch expected={participant.ParticipantId} "
                        + $"actual={progress.ParticipantId}");
                    return false;
                }
            }
            return messages <= value.OfferedMessages
                && bytes <= value.OfferedBytes;
        }

        internal bool MatchesReserved(
            ZLinkServiceWireCodec.RelocationReservedRecord value)
        {
            //  여덟 비교를 한 식으로 묶으면 어느 것이 거부했는지 알 수 없고,
            //  실패는 source의 reservation deadline으로만 나타난다. 어느 필드가
            //  어긋났는지 남긴다.
            if (!Offer.Task.IsCompletedSuccessfully)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    "canonical_reserved_mismatch field=offer_not_completed");
                return false;
            }
            var offer = Offer.Task.Result;
            var mismatch =
                value.RelocationId != prepare.RelocationId ? "relocationId"
                : value.TargetAttemptGeneration != prepare.TargetAttemptGeneration
                    ? "targetAttemptGeneration"
                : value.RoundKind != prepare.RoundKind ? "roundKind"
                : value.Coordinator != prepare.Coordinator ? "coordinator"
                : value.Candidate != prepare.Candidate ? "candidate"
                : value.ReservationGeneration != offer.ReservationGeneration
                    ? $"reservationGeneration value={value.ReservationGeneration} offer={offer.ReservationGeneration}"
                : value.Participants.Count != prepare.Participants.Count
                    ? $"participantCount value={value.Participants.Count} prepare={prepare.Participants.Count}"
                : null;
            if (mismatch is not null)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    "canonical_reserved_mismatch field=" + mismatch);
                return false;
            }
            for (var index = 0; index < prepare.Participants.Count; index++)
                if (value.Participants[index] != prepare.Participants[index])
                {
                    //  개수는 같은데 항목이 어긋나는 경우다. 어느 자리에서 무엇이
                    //  다른지 남기지 않으면 source의 reservation deadline으로만
                    //  나타난다.
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"canonical_reserved_mismatch field=participant index={index} "
                        + $"value={value.Participants[index]} prepare={prepare.Participants[index]} "
                        + $"valueAll=[{string.Join(",", value.Participants)}] "
                        + $"prepareAll=[{string.Join(",", prepare.Participants)}]");
                    return false;
                }
            return true;
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
    private readonly Dictionary<string, HashSet<string>> _subscriptions =
        new(StringComparer.Ordinal);
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
        lock (_subscriptions)
        {
            if (!_subscriptions.TryGetValue(channelName, out var topics))
            {
                topics = new HashSet<string>(StringComparer.Ordinal);
                _subscriptions.Add(channelName, topics);
            }
            topics.Add(topic);
        }
    }

    internal bool Matches(string channelName, string topic)
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
        RequestCallback callback,
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
        ConcurrentDictionary<string, StreamSessionBinding>> _bindings = new();
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
        if (!_bindings.TryGetValue(sessionRid, out var bindings)
            || !bindings.TryGetValue(actor.ActorId, out var binding)
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
            static _ => new ConcurrentDictionary<string, StreamSessionBinding>(
                StringComparer.Ordinal));
        session[binding.Actor.ActorId] = binding;
    }

    internal void RemoveBinding(
        RoutingId sessionRid,
        string actorId,
        ulong expectedBindingGeneration)
    {
        if (!_bindings.TryGetValue(sessionRid, out var bindings)
            || !bindings.TryGetValue(actorId, out var binding)
            || (expectedBindingGeneration != 0
                && binding.BindingGeneration != expectedBindingGeneration))
            return;
        bindings.TryRemove(
            new KeyValuePair<string, StreamSessionBinding>(
                actorId,
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
            return stream.Send(sessionRid)
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
