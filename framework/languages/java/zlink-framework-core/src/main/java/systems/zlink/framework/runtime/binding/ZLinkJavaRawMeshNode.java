package systems.zlink.framework.runtime.binding;
import java.security.NoSuchAlgorithmException;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.Locale;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.transport.ZLinkEndpointNotation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.concurrent.ThreadLocalRandom;
import java.util.logging.Logger;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.OperationKind;
import systems.zlink.framework.runtime.internal.binding.spot.OwnerKind;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyDomain;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkUserSpotOperationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkActorJoinRecoveryCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceAdmissionGuard;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMailbox;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireFrame;
import systems.zlink.framework.runtime.internal.service.ZLinkCanonicalActorJoinReplyCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.runtime.internal.completion.ZLinkTerminalWinner;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

/**
 * Raw-binding RouteMesh owner. Service topology and application dispatch are
 * implemented in Framework code; the binding only owns ROUTER transport.
 */
final class ZLinkJavaRawMeshNode implements ZLinkInternalMeshNode,
    ZLinkInternalMeshNode.CanonicalRelocationPrepareRequestReplySupport {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
    private static final int PREFIX_BYTES = 5;
    // Core sets this flag only on the CONNECTION_READY edge. A paired
    // CONNECTION_READY event without it is the physical close snapshot.
    private static final int CONNECTION_READY_EDGE_FLAG = 1;
    private static final int MAX_INFRASTRUCTURE_CONTROL_PARTS = 64;
    private static final long MAX_INFRASTRUCTURE_CONTROL_BYTES = 256L * 1024;
    private static final long MAX_INFRASTRUCTURE_PAYLOAD_BYTES =
        4_294_966_774L;
    private static final String RELOCATION_CONTROL_PACKET =
        "zlink.internal.spot.relocation.control.v1";
    private static final int USER_SPOT_TERMINAL_CAPACITY = 4096;
    private static final long USER_SPOT_TERMINAL_RETENTION_MS =
        Duration.ofMinutes(5).toMillis();

    private final String meshName;
    private final ZLinkJavaRawServicePort port;
    private final LongSupplier currentTimeMillis;
    private final Map<String, Integer> channelWeights = new ConcurrentHashMap<>();
    private final Map<Long, PeerIntent> peerIntents = new ConcurrentHashMap<>();
    private final Map<RoutingId, PeerAdmissionExpectation>
        peerAdmissionExpectations = new ConcurrentHashMap<>();
    private final Map<RoutingId, Map<String, Integer>> admittedPeerChannels =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, Map<String, Integer>> knownPeerChannels =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, ZLinkServiceNodeDescriptor.ObjectRole>
        admittedPeerObjectRoles = new ConcurrentHashMap<>();
    private final Set<RoutingId> notRequiredPeers =
        ConcurrentHashMap.newKeySet();
    private final Set<RoutingId> disconnectedPeers =
        ConcurrentHashMap.newKeySet();
    private final Set<RoutingId> rejectedPeers =
        ConcurrentHashMap.newKeySet();
    private final Set<Long> closedPeerIntents =
        ConcurrentHashMap.newKeySet();
    private final Set<Long> closeRequestedPeerIntents =
        ConcurrentHashMap.newKeySet();
    private final Set<Long> livePeerIntents =
        ConcurrentHashMap.newKeySet();
    private final Map<Long, RoutingId> peerIntentRoutingIds =
        new ConcurrentHashMap<>();
    private final Map<Long, Set<TransportIdentity>>
        peerIntentTransports = new ConcurrentHashMap<>();
    private final Map<RoutingId, AutomaticNotRequiredPeer>
        automaticNotRequiredPeers = new ConcurrentHashMap<>();
    private final AtomicLong nextIntent = new AtomicLong(1);
    private final AtomicLong channelSelectionCursor = new AtomicLong();
    private final AtomicLong nextCorrelation = new AtomicLong(1);
    private final AtomicLong nextDispatchEnvelope = new AtomicLong(1);
    private final long preStartBindingGenerationSeed =
        positiveRandomLong();
    private final Map<Long, ZLinkMeshDispatchRecord> dispatchEnvelopes =
        new ConcurrentHashMap<>();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final AtomicBoolean applicationDrainActive = new AtomicBoolean();
    private final ZLinkServiceM6AWireCodec wire =
        new ZLinkServiceM6AWireCodec();
    private final ZLinkServiceM6BWireCodec statefulWire =
        new ZLinkServiceM6BWireCodec();
    private final ZLinkServiceLivenessRegistry liveness;
    private final ScheduledExecutorService deadlines =
        ZLinkProcessExecutionLanes.deadlines();
    private final Executor applicationDispatch =
        ZLinkProcessExecutionLanes.applicationLane();
    private final ZLinkServiceOperationRegistry operations =
        new ZLinkServiceOperationRegistry(deadlines);
    private final ConcurrentLinkedQueue<MonitorEvent>
        monitorEvents = new ConcurrentLinkedQueue<>();
    private final ConcurrentLinkedQueue<PeerCloseRequest>
        pendingPeerCloseRequests = new ConcurrentLinkedQueue<>();
    private final Map<RoutingId, String> connectionIds =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, String> admissionControlReadyConnections =
        new ConcurrentHashMap<>();
    private final Map<ConnectionCandidate,
        ConcurrentLinkedQueue<String>>
        pendingConnectionIds = new ConcurrentHashMap<>();
    private final Map<String, ConcurrentLinkedQueue<String>>
        monitorConnectionIds = new ConcurrentHashMap<>();
    private final Map<String, TransportPair> transportPairs =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, TransportPair> applicationTransportPairs =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, Long> nextAnnouncementNanos =
        new ConcurrentHashMap<>();
    private volatile RoutingId routingId;
    private volatile String bindEndpoint;
    private volatile String advertiseHost;
    private volatile RouterSocket router;
    private volatile SocketMonitor rawMonitor;
    private volatile MeshNodeState state = MeshNodeState.CREATED;
    private volatile Consumer<ZLinkMeshDispatchRecord> receiver =
        ZLinkMeshDispatchRecord::close;
    private volatile ZLinkMeshApplicationReceiver applicationReceiver;
    private volatile systems.zlink.framework.runtime.internal.dispatch
        .ZLinkApplicationJobQueue applicationJobQueue;
    private volatile ZLinkJavaRawSpotNode spotNode;
    private volatile ExecutorService pump;
    private volatile long routerHighWaterMark = 16_777_216L;
    private volatile long routerReceiveHighWaterMark = 16_777_216L;
    private volatile int placementWeight = 100;
    private volatile ZLinkServiceNodeDescriptor.ObjectRole objectRole =
        ZLinkServiceNodeDescriptor.ObjectRole.NONE;
    private volatile ZLinkServiceMailbox mailbox;
    private volatile ZLinkServiceTopologyRegistry topology;
    private volatile ZLinkServiceNodeDescriptor localDescriptor;
    private boolean deferServiceReadyPublication;
    private volatile ZLinkInternalMeshNode.UserSpotOperationHandler
        userSpotOperationHandler;
    private volatile ZLinkInternalMeshNode.ActorCreateOperationHandler
        actorCreateOperationHandler;
    private volatile ZLinkInternalMeshNode.CanonicalActorJoinHandler
        canonicalActorJoinHandler;
    private volatile ZLinkInternalMeshNode.PeerAuthorityResolver
        peerAuthorityResolver;
    private volatile ZLinkInternalMeshNode.PeerAuthorityFence
        localAuthorityFence;
    private volatile ZLinkInternalMeshNode.RelocationControlHandler
        relocationControlHandler;
    private volatile ZLinkInternalMeshNode.CanonicalRelocationControlHandler
        canonicalRelocationControlHandler;
    private volatile ZLinkInternalMeshNode.ActorLeftHandler actorLeftHandler;
    private volatile ZLinkInternalMeshNode.MessageFollowHandler
        messageFollowHandler;
    private volatile Function<String, Optional<ZLinkStreamCodec>>
        applicationStreamCodecResolver =
            ZLinkJavaRawMeshNode::defaultApplicationStreamCodec;
    private volatile ZLinkInternalMeshNode.RelocationReplyRelayHandler
        relocationReplyRelayHandler;
    private final Map<ReplyRelayPendingKey, ReplyRelayPending>
        pendingReplyRelays = new ConcurrentHashMap<>();
    private volatile ZLinkInternalMeshNode.SessionRelocationRouteHandler
        sessionRelocationRouteHandler;
    private volatile ZLinkInternalMeshNode.SessionRelocationSealHandler
        sessionRelocationSealHandler;
    private volatile ZLinkInternalMeshNode.BoundSessionSendHandler
        boundSessionSendHandler;
    private volatile ZLinkInternalMeshNode.BoundSessionReplacedHandler
        boundSessionReplacedHandler;
    private final Map<UserSpotOperationKey, UserSpotTerminalSlot>
        userSpotTerminals = new HashMap<>();

    ZLinkJavaRawMeshNode(Context context, String meshName) {
        this(
            context,
            meshName,
            System::currentTimeMillis,
            ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
            ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT);
    }

    ZLinkJavaRawMeshNode(
        Context context,
        String meshName,
        LongSupplier currentTimeMillis) {
        this(
            context,
            meshName,
            currentTimeMillis,
            ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
            ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT);
    }

    ZLinkJavaRawMeshNode(
        Context context,
        String meshName,
        LongSupplier currentTimeMillis,
        Duration livenessProbeInterval,
        Duration livenessPeerTimeout) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        this.meshName = meshName;
        this.port = new ZLinkJavaRawServicePort(context);
        this.currentTimeMillis = Objects.requireNonNull(
            currentTimeMillis, "currentTimeMillis");
        this.liveness = new ZLinkServiceLivenessRegistry(
            livenessProbeInterval, livenessPeerTimeout);
    }

    @Override
    public String name() {
        return meshName;
    }

    @Override
    public void setPeerAuthorityResolver(
        ZLinkInternalMeshNode.PeerAuthorityResolver resolver) {
        peerAuthorityResolver = Objects.requireNonNull(
            resolver, "resolver");
    }

    @Override
    public CompletionStage<Void> refreshLocalAuthorityFence() {
        ZLinkInternalMeshNode.PeerAuthorityResolver resolver =
            peerAuthorityResolver;
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        if (resolver == null || descriptor == null || routingId == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "local MeshNode authority resolver is unavailable"));
        }
        return resolver.resolve(
                meshName,
                routingId,
                descriptor.lifecycleGeneration())
            .thenAccept(resolved -> localAuthorityFence = resolved
                .orElseThrow(() -> new IllegalStateException(
                    "local MeshNode authority descriptor is not live")));
    }

    ZLinkInternalMeshNode.PeerAuthorityFence localAuthorityFence() {
        return localAuthorityFence;
    }

    @Override
    public long localAuthorityLeaseGeneration() {
        ZLinkInternalMeshNode.PeerAuthorityFence local =
            localAuthorityFence;
        // A raw MeshNode without a configured durable authority resolver is
        // the in-process backend used by the focused runtime tests. It still
        // needs a nonzero wire fence; the durable Framework integration
        // replaces this value when the local authority fence is refreshed.
        return local == null ? 1L : local.ownerLeaseGeneration();
    }

    byte[] encodeLocalSpotAccepted(
        String sourceSpotId,
        String targetSpotId,
        long targetSpotGeneration,
        byte[] metadata,
        List<Message> parts,
        Long replyRouteId) {
        ZLinkInternalMeshNode.PeerAuthorityFence local =
            localAuthorityFence;
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long ownerGeneration = spots
            .spotAuthorityOwnerGeneration(
                routingId, targetSpotId, targetSpotGeneration);
        long ownerLeaseGeneration = spots
            .spotAuthorityOwnerLeaseGeneration(
                routingId, targetSpotId, targetSpotGeneration);
        if (local == null
            || descriptor == null
            || ownerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            return new byte[0];
        }
        UUID operation = UUID.randomUUID();
        var message = new ZLinkServiceM6BWireCodec.SpotMessage(
            replyRouteId != null,
            metadata == null || metadata.length == 0
                ? 0
                : ServiceWireConstants.FLAG_METADATA,
            replyRouteId,
            operation.getMostSignificantBits(),
            operation.getLeastSignificantBits(),
            0,
            sourceSpotId,
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                targetSpotId,
                targetSpotGeneration,
                routingId,
                descriptor.lifecycleGeneration(),
                ownerGeneration,
                ownerLeaseGeneration));
        return ZLinkServiceFrozenRecordCodec.encodeSpot(
            local,
            local,
            message,
            metadata,
            wire.encodeApplicationPayload(applicationPayload(parts)));
    }

    byte[] encodeLocalActorAccepted(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long requestId,
        List<Message> parts) {
        ZLinkInternalMeshNode.PeerAuthorityFence local =
            localAuthorityFence;
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long ownerGeneration = spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (local == null
            || descriptor == null
            || sourceNodeRid == null
            || sourceSessionRid == null && !routingId.equals(sourceNodeRid)
            || sourceSessionRid != null
                && (sourceBindingGeneration <= 0 || sourceSessionSequence <= 0)
            || ownerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            return new byte[0];
        }
        var boundSession = sourceSessionRid == null
            ? null
            : new ZLinkServiceM6BWireCodec.BoundSessionTail(
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence);
        UUID operation = UUID.randomUUID();
        var message = new ZLinkServiceM6BWireCodec.ActorMessage(
            requestId != 0,
            0,
            requestId == 0 ? null : requestId,
            operation.getMostSignificantBits(),
            operation.getLeastSignificantBits(),
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor,
                descriptor.lifecycleGeneration(),
                ownerGeneration,
                ownerLeaseGeneration),
            boundSession);
        return ZLinkServiceFrozenRecordCodec.encodeActor(
            local,
            local,
            message,
            new byte[0],
            wire.encodeApplicationPayload(applicationPayload(parts)));
    }

    byte[] encodeLocalActorAccepted(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long requestId,
        String packetName,
        Map<String, String> metadata,
        byte[] payload) {
        ZLinkInternalMeshNode.PeerAuthorityFence local = localAuthorityFence;
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long ownerGeneration = spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (local == null
            || descriptor == null
            || sourceNodeRid == null
            || sourceSessionRid == null
            || sourceBindingGeneration <= 0
            || sourceSessionSequence <= 0
            || ownerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            return new byte[0];
        }
        UUID operation = UUID.randomUUID();
        var message = new ZLinkServiceM6BWireCodec.ActorMessage(
            requestId != 0,
            metadata == null || metadata.isEmpty()
                ? 0
                : ServiceWireConstants.FLAG_METADATA,
            requestId == 0 ? null : requestId,
            operation.getMostSignificantBits(),
            operation.getLeastSignificantBits(),
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor,
                descriptor.lifecycleGeneration(),
                ownerGeneration,
                ownerLeaseGeneration),
            new ZLinkServiceM6BWireCodec.BoundSessionTail(
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence));
        return ZLinkServiceFrozenRecordCodec.encodeActor(
            local,
            local,
            message,
            encodeFrozenMetadata(metadata),
            wire.encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    packetName,
                    "application/zlink-framework-json-v1",
                    payload)));
    }

    private static byte[] encodeFrozenMetadata(
        Map<String, String> metadata) {
        if (metadata == null || metadata.isEmpty()) {
            return new byte[0];
        }
        if (metadata.size() > 255) {
            throw new IllegalArgumentException(
                "accepted metadata entry count exceeds u8");
        }
        try {
            ByteArrayOutputStream bytes =
                new ByteArrayOutputStream();
            DataOutputStream output =
                new DataOutputStream(bytes);
            output.writeByte(1);
            output.writeByte(metadata.size());
            ArrayList<Map.Entry<String, String>> entries =
                new ArrayList<>(metadata.entrySet());
            entries.sort(Map.Entry.comparingByKey());
            for (var entry : entries) {
                byte[] key = entry.getKey().getBytes(
                    StandardCharsets.UTF_8);
                byte[] value = entry.getValue().getBytes(
                    StandardCharsets.UTF_8);
                if (key.length == 0 || key.length > 255
                    || value.length > 0xffff) {
                    throw new IllegalArgumentException(
                        "accepted metadata exceeds canonical bounds");
                }
                output.writeByte(key.length);
                output.write(key);
                output.writeShort(value.length);
                output.write(value);
            }
            output.flush();
            return bytes.toByteArray();
        } catch (IOException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    @Override
    public void setBind(String endpoint) {
        requireCreated();
        if (endpoint == null || endpoint.isBlank()) {
            throw new IllegalArgumentException("bind endpoint is required");
        }
        bindEndpoint = endpoint;
    }

    @Override
    public void setAdvertiseHost(String host) {
        requireCreated();
        advertiseHost = host == null || host.isBlank() ? null : host;
    }

    @Override
    public void addChannel(String channelName) {
        requireCreated();
        channelWeights.putIfAbsent(requireChannel(channelName), 100);
    }

    @Override
    public synchronized void setChannelWeight(String channelName, int weight) {
        if (weight < 0 || weight > 10_000) {
            throw new IllegalArgumentException(
                "channel weight must be in 0..10000");
        }
        String validatedChannel = requireChannel(channelName);
        if (!channelWeights.containsKey(validatedChannel)) {
            throw new IllegalArgumentException(
                "channel is not registered: " + validatedChannel);
        }
        Integer previous = channelWeights.put(validatedChannel, weight);
        ZLinkServiceNodeDescriptor current = localDescriptor;
        if (current == null || previous == weight) {
            return;
        }
        List<ZLinkServiceNodeDescriptor.Channel> channels =
            channelWeights.entrySet().stream()
                .sorted(Map.Entry.comparingByKey())
                .map(entry -> new ZLinkServiceNodeDescriptor.Channel(
                    entry.getKey(), entry.getValue()))
                .toList();
        ZLinkServiceNodeDescriptor updated = descriptor(
            current.lifecycleGeneration(),
            Math.addExact(current.descriptorRevision(), 1),
            channels,
            current.state());
        localDescriptor = updated;
        topology.publishLocal(updated);
        byte[] update = wire.encodeAdmission(
            ServiceWireConstants.COMMAND_UPDATE,
            updated);
        for (ZLinkServiceTopologyRegistry.Peer peer : topology.peers()) {
            port.send(
                requireStarted(),
                peer.descriptor().nodeRoutingId(),
                List.of(update));
        }
    }

    @Override
    public Map<String, Integer> channelWeights() {
        return Map.copyOf(channelWeights);
    }

    @Override
    public int placementWeight() {
        return placementWeight;
    }

    @Override
    public synchronized void setPlacementWeight(int weight) {
        if (weight < 0 || weight > 10_000) {
            throw new IllegalArgumentException(
                "placement weight must be in 0..10000");
        }
        int previous = placementWeight;
        placementWeight = weight;
        ZLinkServiceNodeDescriptor current = localDescriptor;
        if (current == null || previous == weight) {
            return;
        }
        ZLinkServiceNodeDescriptor updated = descriptor(
            current.lifecycleGeneration(),
            Math.addExact(current.descriptorRevision(), 1),
            current.channels(),
            current.state());
        localDescriptor = updated;
        topology.publishLocal(updated);
        byte[] update = wire.encodeAdmission(
            ServiceWireConstants.COMMAND_UPDATE,
            updated);
        for (ZLinkServiceTopologyRegistry.Peer peer : topology.peers()) {
            port.send(
                requireStarted(),
                peer.descriptor().nodeRoutingId(),
                List.of(update));
        }
    }

    @Override
    public void setObjectRole(
        ZLinkMeshNodeObjectRole role) {
        requireCreated();
        objectRole = switch (Objects.requireNonNull(role, "role")) {
            case NONE -> ZLinkServiceNodeDescriptor.ObjectRole.NONE;
            case CLIENT -> ZLinkServiceNodeDescriptor.ObjectRole.CLIENT;
            case SERVER -> ZLinkServiceNodeDescriptor.ObjectRole.SERVER;
        };
    }

    @Override
    public void setRoutingId(RoutingId value) {
        requireCreated();
        routingId = Objects.requireNonNull(value, "routingId");
    }

    @Override
    public void setRouterHighWaterMark(long value) {
        if (value < 0) {
            throw new IllegalArgumentException(
                "router high-water mark must not be negative");
        }
        routerHighWaterMark = value;
    }

    @Override
    public void setRouterReceiveHighWaterMark(long value) {
        if (value < 0) {
            throw new IllegalArgumentException(
                "router receive high-water mark must not be negative");
        }
        routerReceiveHighWaterMark = value;
    }

    @Override
    public void start() {
        requireCreated();
        if (bindEndpoint == null) {
            throw new IllegalStateException("bind endpoint is required");
        }
        if (routingId == null) {
            routingId = RoutingId.from(UUID.randomUUID());
        }
        RouterSocket opened = port.openRouter(routingId);
        try {
            opened.options().sendHwm(routerHighWaterMark);
            opened.options().recvHwm(routerReceiveHighWaterMark);
            // RouteMesh admission is peer-initiated when automatic discovery
            // chooses the other node as the deterministic dialer. Enable the
            // ZMTP probe on this ROUTER before bind so that an inbound,
            // store-expected connection is promoted to a usable route and
            // its HELLO reaches this node's admission pump. Node configures
            // the same option on every raw mesh router.
            opened.options().probe(true);
            opened.bind(bindEndpoint);
            String boundEndpoint = opened.options().lastEndpoint();
            if (boundEndpoint != null && !boundEndpoint.isBlank()) {
                // Location discovery must advertise the actual listener when
                // the caller requested an ephemeral port. Otherwise a peer
                // selected as the connection initiator dials :0 and the
                // RouteMesh never reaches channel admission.
                bindEndpoint = boundEndpoint;
            }
            // Register the single Framework-owned receive/completion poller
            // after the socket is configured and before the mesh can submit
            // any request. This is the same lifecycle boundary as the .NET
            // raw service's Start method.
            port.ensureReceivePollerRegistered(opened);
            router = opened;
            long lifecycle = positiveRandomLong();
            List<ZLinkServiceNodeDescriptor.Channel> descriptorChannels =
                channelWeights.entrySet().stream()
                    .sorted(Map.Entry.comparingByKey())
                    .map(entry -> new ZLinkServiceNodeDescriptor.Channel(
                        entry.getKey(), entry.getValue()))
                    .toList();
            localDescriptor = descriptor(
                lifecycle,
                1,
                descriptorChannels,
                ZLinkServiceNodeDescriptor.State.PREPARING);
            topology = new ZLinkServiceTopologyRegistry(localDescriptor);
            mailbox = new ZLinkServiceMailbox(
                4096, 64L * 1024 * 1024, 1024, 8L * 1024 * 1024);
            state = MeshNodeState.STARTED;
            state = MeshNodeState.READY;
            rawMonitor = port.openMonitor(
                opened,
                MonitorEventType.CONNECTION_READY,
                MonitorEventType.DISCONNECTED,
                MonitorEventType.CLOSED);
            rawMonitor.onEvent(monitorEvents::add);
            startPump();
            if (!deferServiceReadyPublication) {
                markServiceReady();
            }
        } catch (RuntimeException failure) {
            state = MeshNodeState.ERROR;
            throw failure;
        }
    }

    @Override
    public synchronized void deferServiceReadyPublication() {
        requireCreated();
        deferServiceReadyPublication = true;
    }

    @Override
    public synchronized void markServiceReady() {
        if (state != MeshNodeState.READY) {
            throw new IllegalStateException(
                "raw MeshNode must be READY before service readiness is published");
        }
        ZLinkServiceNodeDescriptor current = localDescriptor;
        if (current == null
            || current.state() == ZLinkServiceNodeDescriptor.State.SERVING) {
            return;
        }
        if (current.state() != ZLinkServiceNodeDescriptor.State.PREPARING) {
            throw new IllegalStateException(
                "raw MeshNode has an invalid descriptor state: "
                    + current.state());
        }
        ZLinkServiceNodeDescriptor updated = descriptor(
            current.lifecycleGeneration(),
            Math.addExact(current.descriptorRevision(), 1),
            current.channels(),
            ZLinkServiceNodeDescriptor.State.SERVING);
        localDescriptor = updated;
        topology.publishLocal(updated);
        byte[] update = wire.encodeAdmission(
            ServiceWireConstants.COMMAND_UPDATE,
            updated);
        for (ZLinkServiceTopologyRegistry.Peer peer : topology.peers()) {
            trySendAdmissionControl(
                peer.descriptor().nodeRoutingId(),
                List.of(update),
                "service-ready");
        }
    }

    @Override
    public long connectPeer(String endpoint) {
        return connectPeer(endpoint, null);
    }

    @Override
    public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
        //  A declared peer routing id fences the endpoint and the transport
        //  security identity, not the routing id twice: the descriptor's
        //  securityIdentity is the plaintext placeholder every language
        //  encodes. Expecting the routing id here rejected every non-Java
        //  peer. An endpoint-only intent keeps "no constraint" (null).
        return connectPeer(
            endpoint,
            expectedRoutingId,
            0,
            expectedRoutingId == null
                ? null
                : ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
    }

    @Override
    public long connectPeer(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity) {
        RouterSocket current = requireStarted();
        if (endpoint == null || endpoint.isBlank()) {
            throw new IllegalArgumentException("peer endpoint is required");
        }
        //  Write-time normalization (endpoint notation policy §2.3): the
        //  acceptance point for a manually connected peer endpoint.
        endpoint = ZLinkEndpointNotation.normalize(endpoint);
        //  0 means "no lifecycle constraint"; every other 64-bit pattern is
        //  a valid opaque CSPRNG generation token, including the ~50% that
        //  read back as a negative long (spec 13 §7.1 -- the token is never
        //  judged by numeric magnitude).
        long intent = nextIntent.getAndIncrement();
        peerIntents.put(
            intent,
            new PeerIntent(
                endpoint,
                expectedRoutingId,
                expectedLifecycleGeneration,
                expectedSecurityIdentity,
                System.currentTimeMillis()));
        closedPeerIntents.remove(intent);
        closeRequestedPeerIntents.remove(intent);
        livePeerIntents.remove(intent);
        peerIntentRoutingIds.remove(intent);
        peerIntentTransports.remove(intent);
        if (expectedRoutingId != null) {
            rejectedPeers.remove(expectedRoutingId);
            nextAnnouncementNanos.put(expectedRoutingId, 0L);
        }
        try {
            // Publish the intent and its admission fence before native
            // connect can deliver an inproc HELLO/ADMIT. Registering after
            // connect lets the caller-side initialization erase a rejection
            // that already arrived on the pump thread.
            current.connect(endpoint);
        } catch (RuntimeException failure) {
            peerIntents.remove(intent);
            closedPeerIntents.remove(intent);
            closeRequestedPeerIntents.remove(intent);
            livePeerIntents.remove(intent);
            peerIntentRoutingIds.remove(intent);
            peerIntentTransports.remove(intent);
            throw failure;
        }
        return intent;
    }

    @Override
    public long replacePeerConnection(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity) {
        requireStarted();
        if (endpoint == null || endpoint.isBlank()) {
            throw new IllegalArgumentException("peer endpoint is required");
        }
        //  Write-time normalization (endpoint notation policy §2.3): keep
        //  the staleIntentIds lookup below and the connectPeer() call it
        //  delegates to comparing on the same normalized form.
        String normalizedEndpoint = ZLinkEndpointNotation.normalize(endpoint);
        //  See the opaque-token note on connectPeer above: 0 is "no
        //  constraint", any other 64-bit pattern is a valid generation.
        List<Long> staleIntentIds = peerIntents.entrySet().stream()
            .filter(entry -> normalizedEndpoint.equals(entry.getValue().endpoint()))
            .map(Map.Entry::getKey)
            .toList();
        for (long staleIntentId : staleIntentIds) {
            if (!peerIntentIsClosed(staleIntentId)) {
                requestPeerIntentClose(staleIntentId);
                throw new IllegalStateException(
                    "previous peer connection has not completed liveness close");
            }
        }
        // requestPeerIntentClose already terminated the shared endpoint. Do
        // not issue the same endpoint termination once per stale intent; an
        // endpoint may have both a startup intent and a descriptor intent.
        staleIntentIds.forEach(intentId ->
            removePeerConnection(intentId, false));
        return connectPeer(
            normalizedEndpoint,
            expectedRoutingId,
            expectedLifecycleGeneration,
            expectedSecurityIdentity);
    }

    private boolean peerIntentIsClosed(long connectionIntentId) {
        PeerIntent intent = peerIntents.get(connectionIntentId);
        return intent == null
            || closedPeerIntents.contains(connectionIntentId);
    }

    boolean hasLivePeerIntent(String endpoint) {
        return peerIntents.entrySet().stream()
            .filter(entry -> endpoint.equals(entry.getValue().endpoint()))
            .anyMatch(entry -> livePeerIntents.contains(entry.getKey()));
    }

    @Override
    public boolean isPeerConnectionClosing(long connectionIntentId) {
        return closeRequestedPeerIntents.contains(connectionIntentId);
    }

    private void requestPeerIntentClose(long connectionIntentId) {
        PeerIntent intent = peerIntents.get(connectionIntentId);
        if (intent == null || router == null
            || !closeRequestedPeerIntents.add(connectionIntentId)) {
            return;
        }
        Set<TransportPair> pairs = peerIntentTransports.getOrDefault(
            connectionIntentId,
            Set.of()).stream()
            .filter(transport -> transport.pairId() != 0
                && transport.pairGeneration() != 0)
            .map(transport -> new TransportPair(
                transport.pairId(), transport.pairGeneration()))
            .collect(Collectors.toSet());
        pendingPeerCloseRequests.add(new PeerCloseRequest(
            connectionIntentId, intent.endpoint(), Set.copyOf(pairs)));
    }

    private void drainPeerCloseRequests() {
        PeerCloseRequest request;
        while ((request = pendingPeerCloseRequests.poll()) != null) {
            PeerIntent intent = peerIntents.get(request.connectionIntentId());
            RouterSocket current = router;
            if (intent == null || current == null
                || !closeRequestedPeerIntents.contains(request.connectionIntentId())) {
                continue;
            }
            try {
                if (request.transportPairs().isEmpty()) {
                    current.disconnect(request.endpoint());
                } else {
                    for (TransportPair pair : request.transportPairs()) {
                        current.disconnectTransportPair(
                            pair.id(), pair.generation());
                    }
                }
                if (request.endpoint().startsWith("inproc://")) {
                    // Core removes an inproc endpoint and terminates both pipe
                    // halves synchronously. There is no later monitor event for
                    // this path, so the successful disconnect is the physical
                    // liveness close for this transport.
                    closedPeerIntents.add(request.connectionIntentId());
                    closeRequestedPeerIntents.remove(request.connectionIntentId());
                    livePeerIntents.remove(request.connectionIntentId());
                    peerIntentTransports.remove(request.connectionIntentId());
                }
              } catch (RuntimeException failure) {
                  if (request.endpoint().startsWith("inproc://")) {
                      // An inproc peer can disappear as part of the remote
                      // endpoint teardown before this pump reaches disconnect.
                      // That is already the terminal physical state; retaining
                      // the intent as closing would permanently fence a later
                      // descriptor-backed replacement.
                      closedPeerIntents.add(request.connectionIntentId());
                      livePeerIntents.remove(request.connectionIntentId());
                      peerIntentTransports.remove(request.connectionIntentId());
                  }
                  closeRequestedPeerIntents.remove(request.connectionIntentId());
                  // A non-inproc transport must wait for a later descriptor
                  // observation to retry its physical close.
              }
        }
    }

    @Override
    public void removePeerConnection(long connectionIntentId) {
        removePeerConnection(connectionIntentId, true);
    }

    private void removePeerConnection(
        long connectionIntentId,
        boolean disconnectEndpoint) {
        PeerIntent removed = peerIntents.remove(connectionIntentId);
        closedPeerIntents.remove(connectionIntentId);
        closeRequestedPeerIntents.remove(connectionIntentId);
        livePeerIntents.remove(connectionIntentId);
        peerIntentRoutingIds.remove(connectionIntentId);
        peerIntentTransports.remove(connectionIntentId);
        if (removed != null && router != null) {
            if (removed.expectedRoutingId() != null) {
                admittedPeerChannels.remove(removed.expectedRoutingId());
                admittedPeerObjectRoles.remove(removed.expectedRoutingId());
                notRequiredPeers.remove(removed.expectedRoutingId());
                disconnectedPeers.remove(removed.expectedRoutingId());
                rejectedPeers.remove(removed.expectedRoutingId());
                nextAnnouncementNanos.remove(removed.expectedRoutingId());
                String connectionId =
                    connectionIds.remove(removed.expectedRoutingId());
                admissionControlReadyConnections.remove(
                    removed.expectedRoutingId());
                if (connectionId != null) {
                    ZLinkServiceTopologyRegistry currentTopology = topology;
                    if (currentTopology != null) {
                        currentTopology.disconnect(
                            removed.expectedRoutingId(), connectionId);
                    }
                    liveness.disconnect(
                        removed.expectedRoutingId(), connectionId);
                }
                clearConnectionCandidates(removed.expectedRoutingId());
                forgetKnownPeerChannelsIfUntracked(
                    removed.expectedRoutingId());
            }
            if (disconnectEndpoint) {
                router.disconnect(removed.endpoint());
            }
        }
    }

    @Override
    public void observePeerAdmissionExpectation(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration,
        String securityIdentity) {
        peerAdmissionExpectations.put(
            Objects.requireNonNull(peerRid, "peerRid"),
            new PeerAdmissionExpectation(
                endpoint,
                lifecycleGeneration,
                securityIdentity));
    }

    @Override
    public void forgetPeerAdmissionExpectation(RoutingId peerRid) {
        peerAdmissionExpectations.remove(peerRid);
        forgetKnownPeerChannelsIfUntracked(peerRid);
    }

    @Override
    public MeshNodeStatus status() {
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        return new MeshNodeStatus(
            state,
            routingId,
            meshName,
            bindEndpoint,
            descriptor == null ? 0 : descriptor.lifecycleGeneration(),
            descriptor == null ? 0 : descriptor.descriptorRevision(),
            channelWeights.size(),
            peerIntents.size(),
            Math.toIntExact(admittedPeerChannels.keySet().stream()
                .filter(this::isReadyPeer)
                .count()),
            0,
            mailbox == null
                ? 0
                : mailbox.pendingMessages(
                    ZLinkServiceMailbox.Domain.APPLICATION),
            0,
            0,
            0,
            System.currentTimeMillis());
    }

    @Override
    public List<MeshPeerEntry> peers() {
        List<MeshPeerEntry> manualPeers = peerIntents.entrySet().stream()
            .filter(entry -> entry.getValue().expectedRoutingId() != null)
            .sorted(Comparator.comparingLong(Map.Entry::getKey))
            .map(entry -> new MeshPeerEntry(
                entry.getValue().expectedRoutingId(),
                entry.getValue().endpoint(),
                entry.getKey(),
                MeshPeerSource.MANUAL,
                isReadyPeer(entry.getValue().expectedRoutingId())
                    ? MeshPeerState.ADMITTED
                    : admittedPeerChannels.containsKey(
                        entry.getValue().expectedRoutingId())
                        ? MeshPeerState.CONNECTING
                    : notRequiredPeers.contains(
                        entry.getValue().expectedRoutingId())
                        ? MeshPeerState.NOT_REQUIRED
                        : rejectedPeers.contains(
                            entry.getValue().expectedRoutingId())
                        ? MeshPeerState.ERROR
                        : disconnectedPeers.contains(
                            entry.getValue().expectedRoutingId())
                            ? MeshPeerState.CLOSED
                            : MeshPeerState.CONNECTING,
                entry.getValue().expectedLifecycleGeneration(),
                1,
                0,
                0,
                entry.getValue().createdAtMs()))
            .toList();
        List<MeshPeerEntry> automaticPeers = automaticNotRequiredPeers.entrySet().stream()
            .filter(entry -> manualPeers.stream().noneMatch(
                peer -> peer.routingId().equals(entry.getKey())))
            .sorted(Map.Entry.comparingByKey(
                Comparator.comparing(RoutingId::toHex)))
            .map(entry -> new MeshPeerEntry(
                entry.getKey(),
                entry.getValue().endpoint(),
                0,
                MeshPeerSource.DISCOVERY,
                MeshPeerState.NOT_REQUIRED,
                entry.getValue().lifecycleGeneration(),
                1,
                0,
                0,
                entry.getValue().observedAtMs()))
            .toList();
        List<MeshPeerEntry> admittedInboundPeers = topology == null
            ? List.of()
            : topology.peers().stream()
                .filter(entry -> manualPeers.stream().noneMatch(
                    peer -> peer.routingId().equals(
                        entry.descriptor().nodeRoutingId())))
                .filter(entry -> automaticPeers.stream().noneMatch(
                    peer -> peer.routingId().equals(
                        entry.descriptor().nodeRoutingId())))
                .map(entry -> new MeshPeerEntry(
                    entry.descriptor().nodeRoutingId(),
                    entry.descriptor().advertisedEndpoint(),
                    0,
                MeshPeerSource.DISCOVERY,
                    isReadyPeer(entry.descriptor().nodeRoutingId())
                        ? MeshPeerState.ADMITTED
                        : MeshPeerState.CONNECTING,
                    entry.descriptor().lifecycleGeneration(),
                    entry.descriptor().descriptorRevision(),
                    0,
                    0,
                    currentTimeMillis.getAsLong()))
                .toList();
        return Stream.concat(
            Stream.concat(
                manualPeers.stream(),
                automaticPeers.stream()),
            admittedInboundPeers.stream()).toList();
    }

    @Override
    public boolean isCanonicalRelocationTargetAdmitted(
        RoutingId peerRid,
        long lifecycleGeneration) {
        ZLinkServiceTopologyRegistry current = topology;
        if (current == null) {
            return false;
        }
        return current.peer(peerRid)
            .filter(peer -> peer.descriptor().lifecycleGeneration()
                == lifecycleGeneration)
            .filter(peer -> peer.connectionId().equals(
                admissionControlReadyConnections.get(peerRid)))
            .filter(this::hasSelectedApplicationTransportPair)
            .isPresent();
    }

    @Override
    public void markPeerConnectionNotRequired(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration) {
        knownPeerChannels.remove(peerRid);
        disconnectedPeers.remove(peerRid);
        automaticNotRequiredPeers.put(
            peerRid,
            new AutomaticNotRequiredPeer(
                endpoint,
                lifecycleGeneration,
                currentTimeMillis.getAsLong()));
        admittedPeerObjectRoles.put(
            peerRid,
            ZLinkServiceNodeDescriptor.ObjectRole.CLIENT);
    }

    @Override
    public void clearPeerConnectionNotRequired(RoutingId peerRid) {
        automaticNotRequiredPeers.remove(peerRid);
        if (!notRequiredPeers.contains(peerRid)
            && !admittedPeerChannels.containsKey(peerRid)) {
            admittedPeerObjectRoles.remove(peerRid);
        }
    }

    @Override
    public PeerChannels peerChannels(
        RoutingId peerRid,
        long lifecycleGeneration) {
        Map<String, Integer> channels =
            admittedPeerChannels.getOrDefault(peerRid, Map.of());
        List<Map.Entry<String, Integer>> ordered = channels.entrySet().stream()
            .sorted(Map.Entry.comparingByKey())
            .toList();
        return new PeerChannels(
            ordered.stream().map(Map.Entry::getKey).toList(),
            ordered.stream().map(Map.Entry::getValue).toList());
    }

    @Override
    public MeshNodeMonitor openMonitor() {
        return new ZLinkJavaRawMeshMonitor(this::status);
    }

    @Override
    public List<Long> connectionIntentIds() {
        return peerIntents.keySet().stream().sorted().toList();
    }

    @Override
    public void startDispatch(Consumer<ZLinkMeshDispatchRecord> value) {
        receiver = Objects.requireNonNull(value, "receiver");
    }

    @Override
    public void setApplicationReceiver(ZLinkMeshApplicationReceiver value) {
        applicationReceiver =
            Objects.requireNonNull(value, "applicationReceiver");
        ZLinkJavaRawSpotNode current = spotNode;
        if (current != null) {
            current.setApplicationReceiver(value);
        }
    }

    @Override
    public void setApplicationJobQueue(
        systems.zlink.framework.runtime.internal.dispatch
            .ZLinkApplicationJobQueue value) {
        applicationJobQueue = Objects.requireNonNull(
            value, "applicationJobQueue");
    }

    void executeApplication(Runnable task) {
        applicationDispatch.execute(
            Objects.requireNonNull(task, "task"));
    }

    @Override
    public synchronized ZLinkInternalSpotNode spotNode() {
        if (spotNode == null) {
            spotNode = new ZLinkJavaRawSpotNode(this);
            if (applicationReceiver != null) {
                spotNode.setApplicationReceiver(applicationReceiver);
            }
        }
        return spotNode;
    }

    @Override
    public Optional<RoutingId> selectPlacementTarget() {
        ZLinkServiceTopologyRegistry current = topology;
        return current == null
            ? Optional.empty()
            : current.selectPlacement(this::isReadyPeer)
                .map(peer -> peer.descriptor().nodeRoutingId());
    }

    @Override
    public RoutingId routingId() {
        return routingId;
    }

    private boolean isReadyPeer(RoutingId peerRoutingId) {
        ZLinkServiceTopologyRegistry current = topology;
        if (current == null) {
            return false;
        }
        return current.peer(peerRoutingId)
            .filter(peer -> peer.connectionId().equals(
                admissionControlReadyConnections.get(peerRoutingId)))
            .filter(this::hasSelectedApplicationTransportPair)
            .map(peer -> liveness.isReady(
                peerRoutingId, peer.connectionId()))
            .orElse(false);
    }

    private boolean isReadyPeer(
        ZLinkServiceTopologyRegistry.Peer peer) {
        RoutingId peerRoutingId = peer.descriptor().nodeRoutingId();
        return peer.connectionId().equals(
                admissionControlReadyConnections.get(peerRoutingId))
            && hasSelectedApplicationTransportPair(peer)
            && liveness.isReady(peerRoutingId, peer.connectionId());
    }

    @Override
    public boolean canRequestCanonicalActorJoin(
        ZLinkInternalMeshNode.CanonicalActorJoinRequest request) {
        Objects.requireNonNull(request, "request");
        if (!hasCompleteCanonicalActorJoinFence(request)) {
            return false;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> peer = topology == null
            ? Optional.empty()
            : topology.peer(request.targetNodeRid());
        if (peer.isEmpty()
            || !isReadyPeer(peer.orElseThrow())
            || peer.orElseThrow().descriptor().lifecycleGeneration()
                != request.targetNodeGeneration()
            || !peer.orElseThrow().descriptor().protocolCapabilities()
                .contains(ServiceWireConstants.REQUIRED_CAPABILITY)) {
            return false;
        }
        // The caller's Location resolver supplied this exact route fence.
        // Require it to still be the observed route remembered by the raw
        // Spot boundary; capability alone must not promote an unobserved
        // target into canonical command 28.
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        return spots.spotAuthorityOwnerGeneration(
                request.targetNodeRid(), request.targetSpotId(),
                request.targetSpotGeneration())
                == request.targetAuthorityOwnerGeneration()
            && spots.spotAuthorityOwnerLeaseGeneration(
                request.targetNodeRid(), request.targetSpotId(),
                request.targetSpotGeneration())
                == request.targetOwnerLeaseGeneration();
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.CanonicalActorJoinReply>
        requestCanonicalActorJoin(
            ZLinkInternalMeshNode.CanonicalActorJoinRequest request,
            Duration timeout) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(timeout, "timeout");
        if (!canRequestCanonicalActorJoin(request)) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "canonical actorJoin target is not authority-observed and admitted"));
        }
        final long correlation = allocateCorrelation();
        final List<byte[]> frames;
        try {
            frames = ServiceWirePilotCodec.encodeActorJoin28(
                new ServiceWirePilotCodec.ActorJoin28(
                    correlation,
                    new ServiceWirePilotCodec.Fence(
                        request.actor().actorId(),
                        request.actor().generation(),
                        request.actor().nodeRid().toBytes(),
                        request.actorNodeGeneration(),
                        request.actorAuthorityOwnerGeneration(),
                        request.actorOwnerLeaseGeneration()),
                    request.entry(),
                    new ServiceWirePilotCodec.Fence(
                        request.targetSpotId(),
                        request.targetSpotGeneration(),
                        request.targetNodeRid().toBytes(),
                        request.targetNodeGeneration(),
                        request.targetAuthorityOwnerGeneration(),
                        request.targetOwnerLeaseGeneration()),
                    new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                        request.packetName(), request.contentType(),
                        request.applicationPayload())));
        } catch (IOException invalid) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "canonical actorJoin request is invalid", invalid));
        }
        return requestApplication(request.targetNodeRid(), frames, timeout)
            .thenApply(reply -> decodeCanonicalActorJoinReply(
                correlation,
                request,
                reply));
    }

    private static boolean hasCompleteCanonicalActorJoinFence(
        ZLinkInternalMeshNode.CanonicalActorJoinRequest request) {
        // Object/authority/lease generations are bounded counters. Lifecycle
        // generations are opaque full-range tokens, where only zero is absent.
        return request.actor().generation() > 0
            && request.actorNodeGeneration() != 0
            && request.actorAuthorityOwnerGeneration() > 0
            && request.actorOwnerLeaseGeneration() > 0
            && request.targetSpotGeneration() > 0
            && request.targetNodeGeneration() != 0
            && request.targetAuthorityOwnerGeneration() > 0
            && request.targetOwnerLeaseGeneration() > 0;
    }

    private ZLinkInternalMeshNode.CanonicalActorJoinReply
        decodeCanonicalActorJoinReply(
            long correlation,
            ZLinkInternalMeshNode.CanonicalActorJoinRequest request,
            List<byte[]> frames) {
        if (frames.isEmpty() || frames.size() > 2) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "canonical actorJoin reply frame count is invalid");
        }
        // A rejected Store admission is still a command-20 typed terminal.
        // Preserve its established framework mapping instead of mistaking the
        // shorter no-tail failure reply for malformed canonical success.
        if (frames.getFirst().length == 21) {
            ZLinkServiceM6AWireCodec.Reply terminal = wire.decodeReplyHeader(
                frames.getFirst());
            if (terminal.correlation() != correlation
                || terminal.terminalResult() == 0
                || frames.size() != 1) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "canonical actorJoin terminal reply is invalid");
            }
            ZLinkBackendRequestResult result = backendResult(
                terminal.terminalResult());
            throw new ZLinkFrameworkException(
                result.toFrameworkErrorKind(terminal.failureCode()),
                "canonical actorJoin rejected by target terminal="
                    + terminal.terminalResult()
                    + " failureCode=" + terminal.failureCode());
        }
        final ZLinkCanonicalActorJoinReplyCodec.ActorJoinReply reply;
        try {
            reply = new ZLinkCanonicalActorJoinReplyCodec().decode(frames.getFirst());
        } catch (RuntimeException invalid) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "canonical actorJoin reply is invalid", invalid);
        }
        if (reply.correlation() != correlation) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "canonical actorJoin reply correlation does not match");
        }
        ZLinkServiceM6AWireCodec.ApplicationPayload application =
            frames.size() == 2
                ? wire.decodeApplicationPayload(frames.get(1))
                : null;
        List<Message> applicationReply = application == null
            ? List.of()
            : decodeApplicationMessages(application);
        return new ZLinkInternalMeshNode.CanonicalActorJoinReply(
            reply.accepted(),
            reply.receiveChunkLimitBytes() == null
                ? 0L
                : reply.receiveChunkLimitBytes(),
            applicationReply,
            ZLinkActorJoinRecoveryCodec.canonicalHandoffId(
                request.actor().nodeRid().toBytes(),
                request.actor().actorId(),
                request.actor().generation(),
                request.actorNodeGeneration(),
                correlation),
            application == null
                ? "application/json"
                : application.contentType());
    }

    private boolean hasSelectedApplicationTransportPair(
        ZLinkServiceTopologyRegistry.Peer peer) {
        TransportPair selected = transportPairs.get(peer.connectionId());
        return applicationTransportPairMatches(
            selected,
            transportPairFor(peer.descriptor().nodeRoutingId()));
    }

    static boolean applicationTransportPairMatches(
        TransportPair selected,
        TransportPair application) {
        // Transports without pair identity retain the legacy single-pair
        // route. Once identity is available, readiness is exact-pair scoped
        // and must not be laundered through another RID route.
        return selected == null
            ? application == null
            : selected.equals(application);
    }

    @Override
    public long lifecycleGeneration() {
        ZLinkServiceNodeDescriptor current = localDescriptor;
        if (current == null) {
            throw new IllegalStateException("raw MeshNode is not started");
        }
        return current.lifecycleGeneration();
    }

    @Override
    public String localAuthorityOwnerId() {
        ZLinkInternalMeshNode.PeerAuthorityFence local = localAuthorityFence;
        return local == null ? routingId().toString() : local.ownerId();
    }

    long bindingGenerationSeed() {
        ZLinkServiceNodeDescriptor current = localDescriptor;
        return current == null
            ? preStartBindingGenerationSeed
            : current.lifecycleGeneration();
    }

    long nodeLifecycleGeneration(RoutingId nodeRid) {
        if (routingId().equals(nodeRid)) {
            return lifecycleGeneration();
        }
        return topology == null
            ? 0L
            : topology.peer(nodeRid)
                .map(peer -> peer.descriptor().lifecycleGeneration())
                .orElse(0L);
    }

    CompletionStage<Void> sendNode(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        boolean request,
        Long correlation) {
        if (topology == null || topology.peer(target).isEmpty()) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        List<byte[]> frames = new ArrayList<>();
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        frames.add(request
            ? wire.encodeNodeRequestHeader(
                Objects.requireNonNull(correlation, "correlation"),
                flags)
            : wire.encodeNodeSendHeader(flags));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(requireStarted(), target, frames);
    }

    CompletionStage<Void> sendChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts) {
        String selectedChannel = requireChannel(channelName);
        ZLinkServiceTopologyRegistry currentTopology = topology;
        Optional<RoutingId> target = currentTopology == null
            ? Optional.empty()
            : currentTopology.selectChannel(selectedChannel, this::isReadyPeer)
                .map(peer -> peer.descriptor().nodeRoutingId());
        if (target.isEmpty()) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        frames.add(wire.encodeChannelSendHeader(selectedChannel, flags));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(requireStarted(), target.orElseThrow(), frames);
    }

    CompletionStage<Void> publishLogicalMulticast(
        ZLinkJavaRawSpot source,
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts) {
        String selectedChannel = requireChannel(channelName);
        ZLinkJavaRawSpotNode currentSpots =
            (ZLinkJavaRawSpotNode) spotNode();
        streamTrace(STREAM_TRACE ? "logical-multicast-publish channel=" + selectedChannel
            + " topic=" + topic
            + " sourceSpot=" + (source == null ? "none" : source.spotId())
            + " peerCount=" + (topology == null ? 0 : topology.peers().size()) : null);
        currentSpots.enqueueLogicalMulticast(
            selectedChannel,
            topic,
            source == null ? routingId.toString() : source.spotId(),
            routingId,
            metadata,
            ZLinkChannelContentTypeFrame.decode(parts),
            parts);
        List<ZLinkServiceTopologyRegistry.Peer> targets =
            topology == null
                ? List.of()
                : topology.peers().stream()
                    .filter(peer ->
                        peer.descriptor().serves(selectedChannel))
                    .toList();
        streamTrace(STREAM_TRACE ? "logical-multicast-targets channel=" + selectedChannel
            + " targets=" + targets.stream()
                .map(peer -> peer.descriptor().nodeRoutingId().toString())
                .sorted()
                .toList() : null);
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeLogicalMulticastHeader(
            flags,
            selectedChannel,
            topic,
            source == null ? routingId.toString() : source.spotId()));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        CompletableFuture<?>[] submissions = targets.stream()
            .map(target -> port.send(
                    requireStarted(),
                    target.descriptor().nodeRoutingId(),
                    frames)
                .whenComplete((ignored, failure) ->
                    streamTrace(STREAM_TRACE ? "logical-multicast-send target="
                        + target.descriptor().nodeRoutingId()
                        + " result=" + (failure == null ? "accepted" : "failed")
                        + " channel=" + selectedChannel
                        + " topic=" + topic : null))
                .toCompletableFuture())
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(submissions);
    }

    CompletionStage<ZLinkBackendReceived> requestNode(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        return request(
            target,
            metadata,
            parts,
            timeout,
            null);
    }

    CompletionStage<ZLinkBackendReceived> requestChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        String selectedChannel = requireChannel(channelName);
        Optional<RoutingId> target = topology.selectChannel(
                selectedChannel, this::isReadyPeer)
            .map(peer -> peer.descriptor().nodeRoutingId());
        streamTrace(STREAM_TRACE ? "request-channel-select channel=" + selectedChannel
            + " target=" + target.map(RoutingId::toString).orElse("none")
            + " peerCount=" + (topology == null ? 0 : topology.peers().size()) : null);
        if (target.isPresent()
            && transportPairFor(target.orElseThrow()) == null) {
            // Liveness admission may complete before the Application lane is
            // available. Do not submit through the unscoped RID route, which
            // can select a stale or Completion transport during reconnect.
            streamTrace(STREAM_TRACE ? "request-channel-deferred channel=" + selectedChannel
                + " target=" + target.orElseThrow()
                + " reason=application-pair-not-ready" : null);
            return CompletableFuture.failedFuture(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
        }
        try {
            if (target.isEmpty()) {
                return CompletableFuture.failedFuture(
                    new ZlinkRequestException(RequestResult.NOT_FOUND));
            }
            return request(
                    target.orElseThrow(),
                    metadata,
                    parts,
                    timeout,
                    selectedChannel)
                .whenComplete((reply, failure) -> streamTrace(STREAM_TRACE ?
                    "request-channel-submit channel=" + selectedChannel
                        + " target=" + target.orElseThrow()
                        + " result="
                        + (failure == null ? "accepted" : "failed") : null));
        } catch (RuntimeException failure) {
            streamTrace(STREAM_TRACE ? "request-channel-submit-failed channel="
                + selectedChannel
                + " target=" + target.map(RoutingId::toString).orElse("none")
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()) : null);
            throw failure;
        }
    }

    Optional<Integer> classifyChannelTarget(String channelName) {
        String selectedChannel = requireChannel(channelName);
        ZLinkServiceTopologyRegistry currentTopology = topology;
        if (currentTopology != null
                && currentTopology.hasSelectableChannel(
                    selectedChannel, ignored -> true)) {
            return Optional.empty();
        }
        boolean knownDisconnectedTarget = knownPeerChannels.values().stream()
            .anyMatch(channels -> channels.containsKey(selectedChannel));
        return Optional.of(knownDisconnectedTarget
            ? ZLinkOneWayCalls.ROUTE_NOT_CONNECTED
            : ZLinkOneWayCalls.TARGET_NOT_FOUND);
    }

    CompletionStage<Void> sendSpot(
        String sourceSpotId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        byte[] metadata,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(targetNodeRid);
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.spotAuthorityOwnerGeneration(
                targetNodeRid, targetSpotId, targetSpotGeneration);
        long ownerLeaseGeneration =
            spots.spotAuthorityOwnerLeaseGeneration(
                targetNodeRid, targetSpotId, targetSpotGeneration);
        if (peer.isEmpty()
            || !isReadyPeer(peer.orElseThrow())
            || targetSpotGeneration <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            streamTrace(STREAM_TRACE ? "send spot reject source=" + sourceSpotId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " reason=" + spotRouteRejectReason(
                    peer, targetSpotGeneration,
                    authorityOwnerGeneration, ownerLeaseGeneration) : null);
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        UUID operation = UUID.randomUUID();
        frames.add(statefulWire.encodeSpotHeader(
            false,
            flags,
            null,
            operation.getMostSignificantBits(),
            operation.getLeastSignificantBits(),
            0,
            sourceSpotId,
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                targetSpotId,
                targetSpotGeneration,
                targetNodeRid,
                peer.orElseThrow().descriptor().lifecycleGeneration(),
                authorityOwnerGeneration,
                ownerLeaseGeneration)));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        return sendApplication(targetNodeRid, frames);
    }

    CompletionStage<ZLinkBackendReceived> requestSpot(
        String sourceSpotId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(targetNodeRid);
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.spotAuthorityOwnerGeneration(
                targetNodeRid, targetSpotId, targetSpotGeneration);
        long ownerLeaseGeneration =
            spots.spotAuthorityOwnerLeaseGeneration(
                targetNodeRid, targetSpotId, targetSpotGeneration);
        if (peer.isEmpty()
            || !isReadyPeer(peer.orElseThrow())
            || targetSpotGeneration <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            streamTrace(STREAM_TRACE ? "request spot reject source=" + sourceSpotId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " reason=" + spotRouteRejectReason(
                    peer, targetSpotGeneration,
                    authorityOwnerGeneration, ownerLeaseGeneration) : null);
            return CompletableFuture.failedFuture(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
        }
        long correlation = allocateCorrelation();
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        UUID operationId = UUID.randomUUID();
        frames.add(statefulWire.encodeSpotHeader(
            true,
            flags,
            correlation,
            operationId.getMostSignificantBits(),
            operationId.getLeastSignificantBits(),
            0,
            sourceSpotId,
            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                targetSpotId,
                targetSpotGeneration,
                targetNodeRid,
                peer.orElseThrow().descriptor().lifecycleGeneration(),
                authorityOwnerGeneration,
                ownerLeaseGeneration)));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        ZLinkServiceOperationRegistry.Operation<ZLinkBackendReceived> operation =
            operations.register(timeout);
        requestApplication(targetNodeRid, frames, timeout)
            .whenComplete((replyFrames, failure) -> completeSpotRequest(
                operation.id(),
                targetNodeRid,
                targetSpotId,
                correlation,
                requestResult(failure),
                replyFrames == null ? List.of() : replyFrames));
        return operation.completion();
    }

    private String spotRouteRejectReason(
        Optional<ZLinkServiceTopologyRegistry.Peer> peer,
        long targetSpotGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        if (peer.isEmpty()) {
            return "missing-peer";
        }
        if (!isReadyPeer(peer.orElseThrow())) {
            return "peer-not-ready selectedPair="
                + transportPairSummary(
                    transportPairs.get(peer.orElseThrow().connectionId()))
                + " applicationPair=" + transportPairSummary(
                    transportPairFor(
                        peer.orElseThrow().descriptor().nodeRoutingId()));
        }
        if (targetSpotGeneration <= 0) {
            return "missing-spot-generation";
        }
        if (authorityOwnerGeneration <= 0) {
            return "missing-authority-owner-generation";
        }
        if (ownerLeaseGeneration <= 0) {
            return "missing-owner-lease-generation";
        }
        return "unknown";
    }

    private void completeSpotRequest(
        UUID operationId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    backendResult(result),
                    Optional.of(targetNodeRid),
                    Optional.of(targetSpotId),
                    Optional.of(correlation),
                    List.of()));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new IllegalArgumentException(
                    "invalid Spot reply frame count");
            }
            ZLinkServiceM6AWireCodec.Reply header =
                wire.decodeReplyHeader(frames.getFirst());
            if (header.correlation() != correlation
                || (header.terminalResult() == 0) != (frames.size() == 2)) {
                throw new IllegalArgumentException(
                    "Spot reply terminal mismatch");
            }
            List<Message> replyParts = header.terminalResult() == 0
                ? decodeApplicationMessages(frames.get(1))
                : List.of();
            ZLinkBackendReceived received = new ZLinkBackendReceived(
                header.terminalResult() == 0
                    ? ZLinkBackendRequestResult.OK
                    : backendResult(header.terminalResult()),
                header.failureCode(),
                Optional.of(targetNodeRid),
                Optional.of(targetSpotId),
                Optional.of(correlation),
                replyParts);
            if (!operations.complete(operationId, received)) {
                received.close();
            }
        } catch (RuntimeException failure) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.PROTOCOL_ERROR,
                    Optional.of(targetNodeRid),
                    Optional.of(targetSpotId),
                    Optional.of(correlation),
                    List.of()));
        }
    }

    CompletionStage<Void> sendActor(
        ZLinkBackendActorRef actor,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty()
            || actor.generation() <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            streamTrace(STREAM_TRACE ? "send actor reject actor=" + actorSummary(actor)
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration : null);
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        if (!isReadyPeer(peer.orElseThrow())) {
            streamTrace(STREAM_TRACE ? "send actor reject actor=" + actorSummary(actor)
                + " reason=peer-not-ready" : null);
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        UUID operation = UUID.randomUUID();
        List<byte[]> frames = List.of(
            statefulWire.encodeActorHeader(
                false,
                0,
                null,
                operation.getMostSignificantBits(),
                operation.getLeastSignificantBits(),
                0,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    peer.orElseThrow().descriptor().lifecycleGeneration(),
                    authorityOwnerGeneration,
                    ownerLeaseGeneration)),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(requireStarted(), actor.nodeRid(), frames)
            .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                "send actor " + (failure == null ? "accepted" : "failed")
                    + " actor=" + actorSummary(actor) : null));
    }

    boolean forwardRelocationSpot(
        ZLinkServiceM6BWireCodec.SpotMessage stale,
        ZLinkServiceM6BWireCodec.SpotRouteFence target,
        byte[] metadata,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        Objects.requireNonNull(stale, "stale");
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(parts, "parts");
        Consumer<Throwable> onFailure = failure == null
            ? ignored -> { }
            : failure;
        if (stale.messageFollowHopCount()
                >= ZLinkServiceMessageFollowWireCodec.MAX_HOP_COUNT
            || !readyRelocationPeer(target.targetNodeRid(),
                target.targetNodeGeneration())) {
            return false;
        }
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeSpotHeader(
            stale.request(),
            stale.flags(),
            stale.correlation(),
            stale.operationHigh(),
            stale.operationLow(),
            stale.messageFollowHopCount() + 1,
            stale.sourceSpotId(),
            target));
        if ((stale.flags() & ServiceWireConstants.FLAG_METADATA) != 0) {
            frames.add(Objects.requireNonNull(metadata, "metadata").clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        if (stale.request()) {
            port.request(
                    requireStarted(),
                    target.targetNodeRid(),
                    frames,
                    Duration.ofSeconds(30))
                .whenComplete((replyFrames, requestFailure) ->
                    forwardRelocationReply(
                        stale.correlation(),
                        requestResult(requestFailure),
                        replyFrames == null ? List.of() : replyFrames,
                        reply,
                        onFailure));
        } else {
            port.send(requireStarted(), target.targetNodeRid(), frames)
                .whenComplete((ignored, sendFailure) -> {
                    if (sendFailure != null) {
                        onFailure.accept(unwrap(sendFailure));
                    }
                });
        }
        parts.forEach(Message::close);
        return true;
    }

    boolean forwardRelocationActor(
        ZLinkServiceM6BWireCodec.ActorMessage stale,
        ZLinkServiceM6BWireCodec.ActorRouteFence target,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        Objects.requireNonNull(stale, "stale");
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(parts, "parts");
        Consumer<Throwable> onFailure = failure == null
            ? ignored -> { }
            : failure;
        if (stale.messageFollowHopCount()
                >= ZLinkServiceMessageFollowWireCodec.MAX_HOP_COUNT
            || !readyRelocationPeer(target.actor().nodeRid(),
                target.targetNodeGeneration())) {
            return false;
        }
        ZLinkBackendActorRef sourceActor = stale.sourceActor() == null
            ? null
            : new ZLinkBackendActorRef(
                routingId,
                stale.sourceActor().actorId(),
                stale.sourceActor().generation());
        List<byte[]> frames = List.of(
            statefulWire.encodeActorHeader(
                stale.request(),
                stale.flags(),
                stale.correlation(),
                stale.operationHigh(),
                stale.operationLow(),
                stale.messageFollowHopCount() + 1,
                sourceActor,
                target,
                stale.boundSession()),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        if (stale.request()) {
            port.request(
                    requireStarted(),
                    target.actor().nodeRid(),
                    frames,
                    Duration.ofSeconds(30))
                .whenComplete((replyFrames, requestFailure) ->
                    forwardRelocationReply(
                        stale.correlation(),
                        requestResult(requestFailure),
                        replyFrames == null ? List.of() : replyFrames,
                        reply,
                        onFailure));
        } else {
            port.send(requireStarted(), target.actor().nodeRid(), frames)
                .whenComplete((ignored, sendFailure) -> {
                    if (sendFailure != null) {
                        onFailure.accept(unwrap(sendFailure));
                    }
                });
        }
        parts.forEach(Message::close);
        return true;
    }

    private boolean readyRelocationPeer(
        RoutingId targetNodeRid,
        long targetNodeGeneration) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer = topology == null
            ? Optional.empty()
            : topology.peer(targetNodeRid);
        return peer.isPresent()
            && isReadyPeer(peer.orElseThrow())
            && peer.orElseThrow().descriptor().lifecycleGeneration()
                == targetNodeGeneration;
    }

    /**
     * Carries the exact (terminalResult, failureCode) pair received from the
     * relocation-forward target so the relay reply preserves it
     * (spec 32-framework-error-model:83-92).
     */
    static final class ZLinkRelayedReplyTerminalException
        extends RuntimeException {
        final int terminalResult;
        final int failureCode;

        ZLinkRelayedReplyTerminalException(int terminalResult, int failureCode) {
            super("relocation-forward reply terminal=" + terminalResult
                + " failureCode=" + failureCode);
            this.terminalResult = terminalResult;
            this.failureCode = failureCode;
        }
    }

    /**
     * Maps a relocation-forward failure to the schema-valid wire pair the
     * relay reply encodes. A received terminal relays unchanged (it was
     * validated on decode); a transport failure relays its boundary terminal
     * with none; a malformed forwarded reply relays
     * protocolError+requestProtocolError (spec 32:91-92; the schema
     * terminal-failure-integrity rule requires typed terminals to carry their
     * exact fine code, so a bare 104+0 would itself be invalid); anything
     * else relays internalError+requestFailed (spec 32:119-120).
     */
    static int[] relayedFailurePair(Throwable relayFailure) {
        if (relayFailure instanceof ZLinkRelayedReplyTerminalException relayed) {
            return new int[] {relayed.terminalResult, relayed.failureCode};
        }
        if (relayFailure instanceof ZlinkRequestException transport
            && ServiceWireConstants.validTerminalFailure(
                transport.getResult().value(), 0)) {
            return new int[] {transport.getResult().value(), 0};
        }
        if (relayFailure instanceof IllegalArgumentException) {
            return new int[] {104, 16};
        }
        return new int[] {105, 17};
    }

    private void forwardRelocationReply(
        Long correlation,
        RequestResult result,
        List<byte[]> replyFrames,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        if (result != RequestResult.OK) {
            failure.accept(new ZlinkRequestException(result));
            return;
        }
        try {
            if (replyFrames.isEmpty() || replyFrames.size() > 2) {
                throw new IllegalArgumentException(
                    "invalid relocation-forward reply frame count");
            }
            ZLinkServiceM6AWireCodec.Reply response =
                wire.decodeReplyHeader(replyFrames.getFirst());
            if (!Objects.equals(response.correlation(), correlation)
                || (response.terminalResult() == 0)
                    != (replyFrames.size() == 2)) {
                throw new IllegalArgumentException(
                    "relocation-forward reply terminal differs");
            }
            if (response.terminalResult() != 0) {
                //  Spec 32-framework-error-model:83-92 — preserve the received
                //  terminal AND fine failure code so the relay re-encodes the
                //  real pair instead of collapsing every forwarded failure
                //  (the old path discarded response.failureCode()).
                failure.accept(new ZLinkRelayedReplyTerminalException(
                    response.terminalResult(), response.failureCode()));
                return;
            }
            List<Message> decoded = decodeApplicationMessages(
                replyFrames.get(1));
            try {
                if (reply == null) {
                    decoded.forEach(Message::close);
                } else {
                    reply.accept(decoded);
                }
            } catch (RuntimeException callbackFailure) {
                decoded.forEach(Message::close);
                throw callbackFailure;
            }
        } catch (RuntimeException invalid) {
            failure.accept(invalid);
        }
    }

    CompletionStage<Void> sendBoundActor(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty()
            || actor.generation() <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            streamTrace(STREAM_TRACE ? "send bound actor reject actor="
                + actorSummary(actor) + " sourceSession=" + sourceSessionRid
                + " binding=" + sourceBindingGeneration
                + " sequence=" + sourceSessionSequence
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration : null);
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        int boundFlags =
            ServiceWireConstants.FLAG_BOUND_SESSION
                | ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        UUID operation = UUID.randomUUID();
        List<byte[]> frames = List.of(
            statefulWire.encodeActorHeader(
                false,
                boundFlags,
                null,
                operation.getMostSignificantBits(),
                operation.getLeastSignificantBits(),
                0,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    peer.orElseThrow().descriptor()
                        .lifecycleGeneration(),
                    authorityOwnerGeneration,
                    ownerLeaseGeneration),
                new ZLinkServiceM6BWireCodec.BoundSessionTail(
                    sourceSessionRid,
                    sourceBindingGeneration,
                    sourceSessionSequence)),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(requireStarted(), actor.nodeRid(), frames)
            .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                "send bound actor "
                    + (failure == null ? "accepted" : "failed")
                    + " actor=" + actorSummary(actor)
                    + " sourceSession=" + sourceSessionRid
                    + " binding=" + sourceBindingGeneration
                    + " sequence=" + sourceSessionSequence : null));
    }

    CompletionStage<List<Message>> requestBoundActorAsync(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long streamRequestSequence,
        List<Message> parts,
        Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "bound Actor request timeout must be positive"));
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty()
            || actor.generation() <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            streamTrace(STREAM_TRACE ? "request bound actor reject actor="
                + actorSummary(actor) + " sourceSession=" + sourceSessionRid
                + " binding=" + sourceBindingGeneration
                + " sequence=" + sourceSessionSequence
                + " requestSequence=" + streamRequestSequence
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration : null);
            return CompletableFuture.failedFuture(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
        }
        int boundFlags =
            ServiceWireConstants.FLAG_BOUND_SESSION
                | ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        if (streamRequestSequence <= 0) {
            throw new IllegalArgumentException(
                "STREAM request sequence must be positive");
        }
        long correlation = streamRequestSequence;
        UUID operation = UUID.randomUUID();
        List<byte[]> frames = List.of(
            statefulWire.encodeActorHeader(
                true,
                boundFlags,
                correlation,
                operation.getMostSignificantBits(),
                operation.getLeastSignificantBits(),
                0,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    peer.orElseThrow().descriptor()
                        .lifecycleGeneration(),
                    authorityOwnerGeneration,
                    ownerLeaseGeneration),
                new ZLinkServiceM6BWireCodec.BoundSessionTail(
                    sourceSessionRid,
                    sourceBindingGeneration,
                    sourceSessionSequence)),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.request(
                requireStarted(),
                actor.nodeRid(),
                frames,
                timeout)
            .thenApply(replyFrames -> {
                try {
                    if (replyFrames.isEmpty() || replyFrames.size() > 2) {
                        //  Spec 32-framework-error-model:91-92 — a reply whose
                        //  shape can't be processed is ProtocolError.
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "invalid bound Actor request reply frame count");
                    }
                    ZLinkServiceM6AWireCodec.Reply response =
                        wire.decodeReplyHeader(replyFrames.getFirst());
                    if (response.correlation() != correlation
                        || (response.terminalResult() == 0)
                            != (replyFrames.size() == 2)) {
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "bound Actor request reply terminal mismatch");
                    }
                    if (response.terminalResult() != 0) {
                        //  Classify the carried terminal + fine failure code
                        //  via the authoritative ownership-aware translator
                        //  instead of discarding the fine code
                        //  (spec 32-framework-error-model:83-118). The raw
                        //  request exception stays as the cause so upstream
                        //  terminal-shape probes keep working.
                        throw new ZLinkFrameworkException(
                            ZLinkBackendRequestResult
                                .fromWireTerminal(response.terminalResult())
                                .toFrameworkErrorKind(response.failureCode()),
                            "bound Actor request failed: terminal="
                                + response.terminalResult()
                                + " failureCode=" + response.failureCode(),
                            new ZlinkRequestException(
                                RequestResult.fromValue(
                                    response.terminalResult())));
                    }
                    return decodeApplicationMessages(replyFrames.get(1));
                } catch (ZLinkFrameworkException failure) {
                    throw failure;
                } catch (IllegalArgumentException failure) {
                    //  A codec/malformed-wire failure (ZLinkServiceWireException
                    //  derives from IllegalArgumentException) means the reply
                    //  can't be processed -> ProtocolError
                    //  (spec 32-framework-error-model:91-92).
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        "bound Actor request reply could not be processed",
                        failure);
                }
            })
            .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                "request bound actor "
                    + (failure == null ? "accepted" : "failed")
                    + " actor=" + actorSummary(actor)
                    + " sourceSession=" + sourceSessionRid
                    + " binding=" + sourceBindingGeneration
                    + " sequence=" + sourceSessionSequence
                    + " requestSequence=" + streamRequestSequence : null));
    }

    private void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] mesh=" + meshName
                + " rid=" + routingId + " " + message);
        }
    }

    private static String actorSummary(
        ZLinkBackendActorRef actor) {
        return actor == null
            ? "null"
            : actor.actorId() + "@" + actor.nodeRid()
                + "/g=" + actor.generation();
    }

    CompletionStage<Void> sendInstanceSpot(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(route.targetNodeRid());
        if (peer.isEmpty()
            || peer.orElseThrow().descriptor().lifecycleGeneration()
                != route.targetNodeGeneration()
            || localDescriptor == null) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeInstanceSpotHeader(
            new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                flags,
                route,
                stableType,
                localDescriptor.lifecycleGeneration(),
                routingId,
                sourceSpotId,
                false,
                0,
                0,
                null)));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        return sendApplication(route.targetNodeRid(), frames)
            .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                "instance-send-submit target=" + route.targetSpotId()
                    + " targetNode=" + route.targetNodeRid()
                    + " pair=" + transportPairSummary(
                        transportPairFor(route.targetNodeRid()))
                    + " result=" + submitFailureSummary(failure) : null));
    }

    @Override
    public CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts) {
        return sendInstanceSpot(
            route, stableType, sourceSpotId, metadata, parts);
    }

    @Override
    public CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        Objects.requireNonNull(route, "route");
        Objects.requireNonNull(parts, "parts");
        Objects.requireNonNull(timeout, "timeout");
        return submitInstanceSpotSendWhenConnected(
            route,
            stableType,
            sourceSpotId,
            metadata,
            parts,
            addDeadlineNanos(timeout));
    }

    private CompletionStage<Void> submitInstanceSpotSendWhenConnected(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        long deadlineNanos) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(route.targetNodeRid());
        if (peer.isPresent()
            && peer.orElseThrow().descriptor().lifecycleGeneration()
                != route.targetNodeGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot target is not connected"));
        }
        if (peer.isEmpty() || localDescriptor == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot target is not connected"));
        }
        if (closed.get() || deadlineNanos - System.nanoTime() <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot send was not submitted"));
        }
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot send was not submitted"));
        }
        return sendInstanceSpot(
                route, stableType, sourceSpotId, metadata, parts)
            .toCompletableFuture()
            .orTimeout(remainingNanos, TimeUnit.NANOSECONDS);
    }

    @Override
    public CompletionStage<List<Message>> requestInstanceSpot(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        Objects.requireNonNull(route, "route");
        Objects.requireNonNull(parts, "parts");
        Objects.requireNonNull(timeout, "timeout");
        long deadlineNanos = addDeadlineNanos(timeout);
        return requestInstanceSpotWhenConnected(
            route,
            stableType,
            sourceSpotId,
            metadata,
            parts,
            deadlineNanos);
    }

    private CompletionStage<List<Message>> requestInstanceSpotWhenConnected(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        long deadlineNanos) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(route.targetNodeRid());
        if (peer.isPresent()
            && peer.orElseThrow().descriptor().lifecycleGeneration()
                != route.targetNodeGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot target is not connected"));
        }
        if (peer.isEmpty() || localDescriptor == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot target is not connected"));
        }
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0 || closed.get()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot target is not connected"));
        }
        Duration remainingTimeout = Duration.ofNanos(remainingNanos);
        long correlation = allocateCorrelation();
        ZLinkServiceOperationRegistry.Operation<List<Message>> operation =
            operations.register(remainingTimeout);
        UUID operationId = operation.id();
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeInstanceSpotHeader(
            new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                flags,
                route,
                stableType,
                localDescriptor.lifecycleGeneration(),
                routingId,
                sourceSpotId,
                true,
                operationId.getMostSignificantBits(),
                operationId.getLeastSignificantBits(),
                correlation)));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        requestApplication(
                route.targetNodeRid(), frames, remainingTimeout)
            .whenComplete((replyFrames, failure) -> {
                RequestResult result = requestResult(failure);
                List<byte[]> replies = replyFrames == null
                    ? List.of()
                    : replyFrames;
                if (!terminal.tryWin(requestTerminalCause(result))) {
                    return;
                }
                if (result != RequestResult.OK
                    || replies.isEmpty()) {
                    streamTrace(STREAM_TRACE ? "instance request reply failed target="
                        + route.targetSpotId()
                        + " targetNode=" + route.targetNodeRid()
                        + " objectGeneration=" + route.objectGeneration()
                        + " ownerGeneration="
                        + route.authorityOwnerGeneration()
                        + " result=" + result
                        + " frames=" + replies.size() : null);
                    operations.completeExceptionally(
                        operation.id(),
                        new IllegalStateException(
                            "remote Instance Spot request failed: " + result));
                    return;
                }
                try {
                    var header = wire.decodeReplyHeader(replies.getFirst());
                    streamTrace(STREAM_TRACE ? "instance request reply target="
                        + route.targetSpotId()
                        + " targetNode=" + route.targetNodeRid()
                        + " objectGeneration=" + route.objectGeneration()
                        + " ownerGeneration="
                        + route.authorityOwnerGeneration()
                        + " correlation=" + correlation
                        + " replyCorrelation=" + header.correlation()
                        + " terminal=" + header.terminalResult()
                        + " frames=" + replies.size() : null);
                    if (header.correlation() != correlation) {
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "Instance Spot request correlation mismatch");
                    }
                    if (header.terminalResult() != 0
                        || header.failureCode() != 0) {
                        //  Spec 51-internal-service-wire-protocol:97 — a
                        //  failed reply carries exactly the header frame; an
                        //  attached tail is rejected as a protocol error
                        //  before the carried terminal is honored.
                        if (replies.size() != 1) {
                            throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                                "invalid Instance Spot failed reply frame count");
                        }
                        //  Classify the carried terminal + fine failure code
                        //  via the authoritative ownership-aware translator
                        //  instead of collapsing to a generic rejection
                        //  (spec 32-framework-error-model:81-118, 99-108).
                        throw new ZLinkFrameworkException(
                            ZLinkBackendRequestResult
                                .fromWireTerminal(header.terminalResult())
                                .toFrameworkErrorKind(header.failureCode()),
                            "remote Instance Spot request failed: terminal="
                                + header.terminalResult()
                                + " failureCode=" + header.failureCode());
                    }
                    if (replies.size() != 2) {
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "invalid Instance Spot request reply frame count");
                    }
                    var payload = wire.decodeApplicationPayload(
                        replies.get(1));
                    List<Message> replyParts =
                        decodeApplicationMessages(payload);
                    if (!operations.complete(operation.id(), replyParts)) {
                        replyParts.forEach(Message::close);
                    }
                } catch (ZLinkFrameworkException decodeFailure) {
                    operations.completeExceptionally(
                        operation.id(), decodeFailure);
                } catch (IllegalArgumentException decodeFailure) {
                    //  A codec/malformed-wire failure (ZLinkServiceWireException
                    //  and friends derive from IllegalArgumentException) means
                    //  the reply can't be processed -> ProtocolError
                    //  (spec 32-framework-error-model:91-92).
                    operations.completeExceptionally(
                        operation.id(),
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            "remote Instance Spot request reply could not be"
                                + " processed",
                            decodeFailure));
                } catch (RuntimeException decodeFailure) {
                    operations.completeExceptionally(
                        operation.id(), decodeFailure);
                }
            });
        return operation.completion();
    }

    private long addDeadlineNanos(Duration timeout) {
        long now = System.nanoTime();
        long duration = timeout.toNanos();
        if (duration <= 0) {
            return now;
        }
        if (Long.MAX_VALUE - now < duration) {
            return Long.MAX_VALUE;
        }
        return now + duration;
    }

    CompletionStage<List<Message>> requestActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        Duration timeout) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        ZLinkJavaRawSpotNode spots = (ZLinkJavaRawSpotNode) spotNode();
        long authorityOwnerGeneration =
            spots.actorAuthorityOwnerGeneration(actor);
        long ownerLeaseGeneration =
            spots.actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty()
            || actor.generation() <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "remote Actor route is not connected"));
        }
        long correlation = allocateCorrelation();
        UUID operationId = UUID.randomUUID();
        List<byte[]> frames = List.of(
            statefulWire.encodeActorHeader(
                true,
                0,
                correlation,
                operationId.getMostSignificantBits(),
                operationId.getLeastSignificantBits(),
                0,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    peer.orElseThrow().descriptor().lifecycleGeneration(),
                    authorityOwnerGeneration,
                    ownerLeaseGeneration)),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        ZLinkServiceOperationRegistry.Operation<ZLinkBackendReceived> operation =
            operations.register(timeout);
        port.request(
                requireStarted(),
                actor.nodeRid(),
                frames,
                timeout)
            .whenComplete((replyFrames, failure) -> completeActorRequest(
                operation.id(),
                actor,
                correlation,
                requestResult(failure),
                replyFrames == null ? List.of() : replyFrames));
            return operation.completion().thenCompose(received -> {
            if (received.result() != ZLinkBackendRequestResult.OK) {
                //  Ownership-aware translator: the coarse remote terminal
                //  refined by the fine failure code (spec 32:81-118, 99-103),
                //  replacing the divergent hand-rolled switch.
                ZLinkFrameworkErrorKind errorKind = received.result()
                    .toFrameworkErrorKind(received.failureCode());
                received.close();
                return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                        errorKind,
                        "Actor request failed: " + received.result()));
            }
            return CompletableFuture.completedFuture(received.parts());
        });
    }

    CompletionStage<Void> bindRemoteStreamSession(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration,
        long bindingGeneration,
        boolean active,
        Duration timeout) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        long ownerLeaseGeneration =
            ((ZLinkJavaRawSpotNode) spotNode())
                .actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty() || ownerLeaseGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
        }
        if (actor.generation() <= 0
            || bindingGeneration == 0
            || authorityOwnerGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "remote Actor binding fence is invalid"));
        }
        long correlation = allocateCorrelation();
        byte[] frame = statefulWire.encodeBoundSessionBindHeader(
            new ZLinkServiceM6BWireCodec.BoundSessionBind(
                correlation,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    peer.orElseThrow().descriptor()
                        .lifecycleGeneration(),
                    authorityOwnerGeneration,
                    ownerLeaseGeneration),
                sessionRid,
                active,
                bindingGeneration));
        CompletableFuture<Void> completion = new CompletableFuture<>();
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        port.request(
                requireStarted(),
                actor.nodeRid(),
                List.of(frame),
                timeout)
            .whenComplete((replyFrames, failure) -> {
                RequestResult result = requestResult(failure);
                List<byte[]> replies = replyFrames == null
                    ? List.of()
                    : replyFrames;
                if (!terminal.tryWin(requestTerminalCause(result))) {
                    return;
                }
                if (result
                    != RequestResult.OK) {
                    completion.completeExceptionally(
                        new ZlinkRequestException(result));
                    return;
                }
                try {
                    if (replies.size() != 1) {
                        throw new IllegalArgumentException(
                            "bound session binding reply frame count");
                    }
                    ZLinkServiceM6AWireCodec.Reply reply =
                        wire.decodeReplyHeader(replies.getFirst());
                    if (reply.correlation() != correlation
                        || reply.terminalResult() != 0
                        || reply.failureCode() != 0) {
                        throw new IllegalArgumentException(
                            "bound session binding was rejected");
                    }
                    completion.complete(null);
                } catch (RuntimeException decodeFailure) {
                    completion.completeExceptionally(decodeFailure);
                }
            });
        return completion;
    }

    @Override
    public void setUserSpotOperationHandler(
        ZLinkInternalMeshNode.UserSpotOperationHandler handler) {
        userSpotOperationHandler =
            Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setActorCreateOperationHandler(
        ZLinkInternalMeshNode.ActorCreateOperationHandler handler) {
        actorCreateOperationHandler =
            Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setCanonicalActorJoinHandler(
        ZLinkInternalMeshNode.CanonicalActorJoinHandler handler) {
        canonicalActorJoinHandler = Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setApplicationStreamCodecResolver(
        Function<String, Optional<ZLinkStreamCodec>> resolver) {
        applicationStreamCodecResolver =
            Objects.requireNonNull(resolver, "resolver");
    }

    @Override
    public void setRelocationControlHandler(
        ZLinkInternalMeshNode.RelocationControlHandler handler) {
        relocationControlHandler =
            Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestRelocationControl(
        RoutingId targetNodeRid,
        byte[] command,
        Duration timeout) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] payload = Objects.requireNonNull(
            command, "command").clone();
        Objects.requireNonNull(timeout, "timeout");
        if (payload.length == 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "relocation command payload is required"));
        }
        if (targetNodeRid.equals(routingId)) {
            var handler = relocationControlHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local relocation command handler is unavailable"));
            }
            return handler.handle(routingId, payload)
                .thenApply(byte[]::clone);
        }
        List<Message> parts = List.of(
            Message.from(RELOCATION_CONTROL_PACKET.getBytes(
                StandardCharsets.UTF_8)),
            Message.from(payload));
        CompletionStage<ZLinkBackendReceived> request;
        try {
            request = requestNode(
                targetNodeRid,
                new byte[0],
                parts,
                timeout);
        } finally {
            parts.forEach(Message::close);
        }
        return request.thenApply(received -> {
            try {
                if (received.result() != ZLinkBackendRequestResult.OK
                    || received.parts().size() != 1) {
                    throw new IllegalStateException(
                        "remote relocation command failed: "
                            + received.result());
                }
                return received.parts().getFirst().toByteArray();
            } finally {
                received.close();
            }
        });
    }

    @Override
    public void setCanonicalRelocationControlHandler(
        ZLinkInternalMeshNode.CanonicalRelocationControlHandler handler) {
        canonicalRelocationControlHandler =
            Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendCanonicalRelocationControl(
        RoutingId targetNodeRid,
        byte[] command) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] record = Objects.requireNonNull(
            command, "command").clone();
        validateCanonicalRelocationControl(record);
        if (targetNodeRid.equals(routingId)) {
            var handler = canonicalRelocationControlHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local canonical relocation handler is unavailable"));
            }
            try {
                return Objects.requireNonNull(
                    handler.handle(routingId, null, record),
                    "canonical relocation handler returned null")
                    .thenApply(ignored -> null);
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        requireStarted();
        return port.send(
            requireStarted(),
            targetNodeRid,
            List.of(record));
    }

    @Override
    public CompletionStage<byte[]> requestCanonicalRelocationPrepare(
        RoutingId targetNodeRid,
        byte[] command,
        Duration timeout) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] record = Objects.requireNonNull(command, "command").clone();
        Objects.requireNonNull(timeout, "timeout");
        validateCanonicalRelocationControl(record);
        if (Byte.toUnsignedInt(record[3])
            != ServiceWireConstants.COMMAND_RELOCATION_PREPARE) {
            return CompletableFuture.failedFuture(new IllegalArgumentException(
                "only canonical relocation prepare is request/reply"));
        }
        if (targetNodeRid.equals(routingId)) {
            var handler = canonicalRelocationControlHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local canonical relocation handler is unavailable"));
            }
            try {
                return Objects.requireNonNull(
                    handler.handle(routingId, 1L, record),
                    "canonical relocation handler returned null")
                    .thenApply(ZLinkJavaRawMeshNode::canonicalPrepareReply);
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        RouterSocket router = requireStarted();
        return port.request(router, targetNodeRid, List.of(record), timeout)
            .thenApply(parts -> {
                if (parts.size() != 1) {
                    throw new IllegalStateException(
                        "canonical relocation prepare reply is invalid");
                }
                return canonicalPrepareReply(parts.getFirst());
            });
    }

    @Override
    public void setActorLeftHandler(
        ZLinkInternalMeshNode.ActorLeftHandler handler) {
        actorLeftHandler = Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendActorLeft(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.ActorLeft left) {
        Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        ZLinkServiceM6BWireCodec.ActorLeft command =
            Objects.requireNonNull(left, "left");
        if (sourceNodeRid.equals(routingId)) {
            var handler = actorLeftHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Actor Left handler is unavailable"));
            }
            try {
                CompletionStage<Void> started = Objects.requireNonNull(
                    handler.handle(routingId, command),
                    "Actor Left handler returned null");
                started.exceptionally(failure -> null);
                return CompletableFuture.completedFuture(null);
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        requireStarted();
        return port.send(
            requireStarted(),
            sourceNodeRid,
            List.of(statefulWire.encodeActorLeft(command)));
    }

    @Override
    public void setMessageFollowHandler(
        ZLinkInternalMeshNode.MessageFollowHandler handler) {
        messageFollowHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendMessageFollow(
        RoutingId targetNodeRid,
        ZLinkServiceMessageFollowWireCodec.Notice notice) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] record = new ZLinkServiceMessageFollowWireCodec().encode(
            Objects.requireNonNull(notice, "notice"));
        if (targetNodeRid.equals(routingId)) {
            var handler = messageFollowHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Message Follow handler is unavailable"));
            }
            try {
                handler.handle(routingId, notice);
                return CompletableFuture.completedFuture(null);
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        requireStarted();
        return port.send(
            requireStarted(),
            targetNodeRid,
            List.of(record));
    }

    @Override
    public void setBoundSessionReplacedHandler(
        ZLinkInternalMeshNode.BoundSessionReplacedHandler handler) {
        boundSessionReplacedHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendBoundSessionReplaced(
        RoutingId sessionOwnerNodeRid,
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement) {
        Objects.requireNonNull(sessionOwnerNodeRid,
            "sessionOwnerNodeRid");
        byte[] record = statefulWire.encodeBoundSessionReplaced(
            Objects.requireNonNull(replacement, "replacement"));
        if (sessionOwnerNodeRid.equals(routingId)) {
            var handler = boundSessionReplacedHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local bound Session replacement handler is unavailable"));
            }
            try {
                handler.handle(routingId,
                    statefulWire.decodeBoundSessionReplaced(record));
                return CompletableFuture.completedFuture(null);
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        requireStarted();
        return port.send(
            requireStarted(),
            sessionOwnerNodeRid,
            List.of(record));
    }

    @Override
    public void setRelocationReplyRelayHandler(
        ZLinkInternalMeshNode.RelocationReplyRelayHandler handler) {
        relocationReplyRelayHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestRelocationReplyRelay(
        RoutingId landingNodeRid,
        ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
        byte[] command33,
        List<byte[]> payload,
        Duration timeout) {
        Objects.requireNonNull(landingNodeRid, "landingNodeRid");
        Objects.requireNonNull(expectedSource, "expectedSource");
        byte[] command = Objects.requireNonNull(
            command33, "command33").clone();
        List<byte[]> application = cloneFrames(
            Objects.requireNonNull(payload, "payload"), 0);
        Objects.requireNonNull(timeout, "timeout");
        var relocationWire = new ZLinkServiceRelocationWireCodec();
        var relay = relocationWire.decodeReplyRelay(command);
        //  The landing node owns the reply capability after relocation. It is
        //  not required to equal the request-source fence: the command 46 ACK
        //  is validated against the exact expected source below.
        if (landingNodeRid.equals(routingId)) {
            var handler = relocationReplyRelayHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local relocation reply relay is unavailable"));
            }
            return handler.handle(routingId, command, application)
                .thenApply(reply -> {
                    validateReplyRelayAck(
                        relay,
                        expectedSource,
                        relocationWire.decodeReplyRelayAck(reply));
                    return reply.clone();
                });
        }
        List<byte[]> frames = new ArrayList<>(application.size() + 1);
        frames.add(command);
        frames.addAll(application);
        requireStarted();
        ReplyRelayPendingKey key = ReplyRelayPendingKey.from(
            landingNodeRid, relay);
        var pending = new ReplyRelayPending(relay, expectedSource);
        if (pendingReplyRelays.putIfAbsent(key, pending) != null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "command 33 pending fence already exists"));
        }
        long timeoutNanos;
        try {
            timeoutNanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            timeoutNanos = Long.MAX_VALUE;
        }
        try {
            pending.armTimeout(deadlines.schedule(
                () -> {
                    if (pendingReplyRelays.remove(key, pending)) {
                        pending.completion.completeExceptionally(
                            new TimeoutException(
                                "command 46 ACK was not received"));
                    }
                },
                Math.max(0L, timeoutNanos),
                TimeUnit.NANOSECONDS));
        } catch (RuntimeException rejected) {
            pendingReplyRelays.remove(key, pending);
            pending.cancelTimeout();
            pending.completion.completeExceptionally(rejected);
            return pending.completion;
        }
        try {
            port.send(requireStarted(), landingNodeRid, frames)
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        return;
                    }
                    if (pendingReplyRelays.remove(key, pending)) {
                        pending.cancelTimeout();
                        pending.completion.completeExceptionally(
                            unwrap(failure));
                    }
                });
        } catch (RuntimeException failure) {
            if (pendingReplyRelays.remove(key, pending)) {
                pending.cancelTimeout();
                pending.completion.completeExceptionally(failure);
            }
        }
        return pending.completion;
    }

    @Override
    public void setSessionRelocationRouteHandler(
        ZLinkInternalMeshNode.SessionRelocationRouteHandler handler) {
        sessionRelocationRouteHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public void setSessionRelocationSealHandler(
        ZLinkInternalMeshNode.SessionRelocationSealHandler handler) {
        sessionRelocationSealHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public void setBoundSessionSendHandler(
        ZLinkInternalMeshNode.BoundSessionSendHandler handler) {
        boundSessionSendHandler = Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestSessionRelocationSeal(
        RoutingId sessionOwnerNodeRid,
        byte[] command42,
        Duration timeout) {
        Objects.requireNonNull(
            sessionOwnerNodeRid, "sessionOwnerNodeRid");
        byte[] record = Objects.requireNonNull(
            command42, "command42").clone();
        Objects.requireNonNull(timeout, "timeout");
        statefulWire.decodeSessionRelocationSeal(record);
        streamTrace(STREAM_TRACE ? "request session seal target=" + sessionOwnerNodeRid
            + " local=" + sessionOwnerNodeRid.equals(routingId)
            + " timeout=" + timeout : null);
        if (sessionOwnerNodeRid.equals(routingId)) {
            var handler = sessionRelocationSealHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Session relocation seal handler is unavailable"));
            }
            return handler.handle(routingId, record).thenApply(reply -> {
                statefulWire.decodeSessionRelocationSealed(reply);
                streamTrace(STREAM_TRACE ? "request session seal local ACK target="
                    + sessionOwnerNodeRid : null);
                return reply.clone();
            });
        }
        return port.request(
                requireStarted(),
                sessionOwnerNodeRid,
                List.of(record),
                timeout)
            .thenApply(reply -> {
                streamTrace(STREAM_TRACE ? "request session seal result target="
                    + sessionOwnerNodeRid + " result=" + RequestResult.OK
                    + " replyFrames=" + reply.size() : null);
                if (reply.size() != 1) {
                    throw new IllegalArgumentException(
                        "Session relocation seal reply frame count");
                }
                statefulWire.decodeSessionRelocationSealed(reply.getFirst());
                return reply.getFirst().clone();
            });
    }

    @Override
    public CompletionStage<Void> sendSessionRelocationRoute(
        RoutingId sessionOwnerNodeRid,
        byte[] command44) {
        Objects.requireNonNull(
            sessionOwnerNodeRid, "sessionOwnerNodeRid");
        byte[] record = Objects.requireNonNull(
            command44, "command44").clone();
        statefulWire.decodeSessionRelocationRoute(record);
        streamTrace(STREAM_TRACE ? "send session route target=" + sessionOwnerNodeRid
            + " local=" + sessionOwnerNodeRid.equals(routingId) : null);
        if (sessionOwnerNodeRid.equals(routingId)) {
            var handler = sessionRelocationRouteHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Session relocation route handler is unavailable"));
            }
            return handler.handle(routingId, record);
        }
        requireStarted();
        return port.send(
            requireStarted(),
            sessionOwnerNodeRid,
            List.of(record));
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        requestActorCreate(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.ActorCreateIntent intent,
            Duration timeout) {
        return requestActorCreate(
            targetNodeRid,
            intent,
            timeout,
            intent.operationHigh(),
            intent.operationLow());
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        requestActorCreate(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.ActorCreateIntent intent,
            Duration timeout,
            long operationHigh,
            long operationLow) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        Objects.requireNonNull(intent, "intent");
        if (targetNodeRid.equals(routingId)) {
            long localGeneration = lifecycleGeneration();
            if (intent.reservation().targetNodeGeneration()
                    != localGeneration
                || actorCreateOperationHandler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Actor create handler or lifecycle is unavailable"));
            }
            return actorCreateOperationHandler.create(
                new ZLinkInternalMeshNode.ActorCreateRequest(
                    routingId,
                    localGeneration,
                    operationHigh,
                    operationLow,
                    intent));
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(targetNodeRid);
        if (peer.isEmpty() || localDescriptor == null
            || !isReadyPeer(peer.orElseThrow())) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Actor create target is not connected"));
        }
        if (!intent.reservation().targetNodeRid()
                .equals(targetNodeRid)
            || intent.reservation().targetNodeGeneration()
                != peer.orElseThrow().descriptor()
                    .lifecycleGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Actor create target is not connected"));
        }
        ZLinkServiceOperationRegistry.Operation<
            ZLinkInternalMeshNode.ActorCreateResponse> operation =
                operations.register(timeout);
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        operation.completion().whenComplete((ignored, failure) ->
            terminal.tryWin(completionTerminalCause(failure)));
        long attemptCorrelation = allocateCorrelation();
        var command = new ZLinkServiceM6BWireCodec.ActorCreate(
            attemptCorrelation,
            operationHigh,
            operationLow,
            routingId,
            localDescriptor.lifecycleGeneration(),
            intent.actorId(),
            intent.stableType(),
            intent.reservation(),
            intent.deadlineUnixMs());
        requestApplication(
                targetNodeRid,
                List.of(statefulWire.encodeActorCreateHeader(command)),
                timeout)
            .whenComplete((replyFrames, failure) -> {
                RequestResult result = requestResult(failure);
                if (!terminal.tryWin(requestTerminalCause(result))) {
                    return;
                }
                completeActorCreate(
                    operation.id(),
                    attemptCorrelation,
                    result,
                    replyFrames == null ? List.of() : replyFrames);
            });
        return operation.completion();
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse>
        requestUserSpotCreate(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.UserSpotCreateIntent intent,
            Duration timeout) {
        return requestUserSpotCreate(
            targetNodeRid, intent, timeout, 0, allocateCorrelation());
    }

    CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse>
        requestUserSpotCreate(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.UserSpotCreateIntent intent,
            Duration timeout,
            long operationHigh,
            long operationLow) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        Objects.requireNonNull(intent, "intent");
        if (targetNodeRid.equals(routingId)) {
            long localGeneration = lifecycleGeneration();
            if (intent.reservation().targetNodeGeneration()
                    != localGeneration
                || userSpotOperationHandler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local User Spot create handler or lifecycle is unavailable"
                            + " [expectedGeneration="
                            + intent.reservation().targetNodeGeneration()
                            + ", localGeneration=" + localGeneration
                            + ", handler="
                            + (userSpotOperationHandler != null) + "]"));
            }
            return userSpotOperationHandler.create(
                new ZLinkInternalMeshNode.UserSpotCreateRequest(
                    routingId,
                    localGeneration,
                    operationHigh,
                    operationLow,
                    intent));
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(targetNodeRid);
        if (peer.isEmpty() || localDescriptor == null
            || !isReadyPeer(peer.orElseThrow())) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote User Spot create target is not connected"));
        }
        if (!intent.reservation().targetNodeRid()
                .equals(targetNodeRid)
            || intent.reservation().targetNodeGeneration()
                != peer.orElseThrow().descriptor()
                    .lifecycleGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote User Spot create target is not connected"));
        }
        ZLinkServiceOperationRegistry.Operation<
            ZLinkInternalMeshNode.UserSpotCreateResponse> operation =
                operations.register(timeout);
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        operation.completion().whenComplete((ignored, failure) ->
            terminal.tryWin(completionTerminalCause(failure)));
        long attemptCorrelation = allocateCorrelation();
        ZLinkServiceM6BWireCodec.UserSpotCreate command =
            new ZLinkServiceM6BWireCodec.UserSpotCreate(
                attemptCorrelation,
                operationHigh,
                operationLow,
                routingId,
                localDescriptor.lifecycleGeneration(),
                intent.spotId(),
                intent.stableType(),
                intent.reservation(),
                intent.deadlineUnixMs());
        requestApplication(
                targetNodeRid,
                List.of(statefulWire.encodeUserSpotCreateHeader(command)),
                timeout)
            .whenComplete((replyFrames, failure) -> {
                RequestResult result = requestResult(failure);
                if (!terminal.tryWin(requestTerminalCause(result))) {
                    return;
                }
                completeUserSpotCreate(
                    operation.id(),
                    attemptCorrelation,
                    result,
                    replyFrames == null ? List.of() : replyFrames);
            });
        return operation.completion();
    }

    private static ZLinkTerminalWinner.Cause requestTerminalCause(
        RequestResult result) {
        return result == RequestResult.OK
            ? ZLinkTerminalWinner.Cause.RESPONSE
            : result == RequestResult.TIMED_OUT
                ? ZLinkTerminalWinner.Cause.TIMEOUT
                : result == RequestResult.TERMINATED
                    ? ZLinkTerminalWinner.Cause.SHUTDOWN
                    : result == RequestResult.NOT_CONNECTED
                        ? ZLinkTerminalWinner.Cause.DISCONNECT
                        : ZLinkTerminalWinner.Cause.FAILURE;
    }

    private static RequestResult requestResult(Throwable failure) {
        Throwable current = unwrap(failure);
        if (current == null) {
            return RequestResult.OK;
        }
        if (current instanceof ZlinkRequestException requestFailure) {
            return requestFailure.getResult();
        }
        if (current instanceof TimeoutException) {
            return RequestResult.TIMED_OUT;
        }
        return RequestResult.INTERNAL_ERROR;
    }

    private static ZLinkTerminalWinner.Cause completionTerminalCause(
        Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            || current instanceof ExecutionException) {
            current = current.getCause();
        }
        return current == null
            ? ZLinkTerminalWinner.Cause.RESPONSE
            : current instanceof TimeoutException
                ? ZLinkTerminalWinner.Cause.TIMEOUT
                : ZLinkTerminalWinner.Cause.FAILURE;
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse>
        requestUserSpotClose(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.UserSpotCloseIntent intent,
            Duration timeout) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        Objects.requireNonNull(intent, "intent");
        if (targetNodeRid.equals(routingId)) {
            long localGeneration = lifecycleGeneration();
            if (intent.target().targetNodeGeneration()
                    != localGeneration
                || userSpotOperationHandler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local User Spot close handler or lifecycle is unavailable"
                            + " [expectedGeneration="
                            + intent.target().targetNodeGeneration()
                            + ", localGeneration=" + localGeneration
                            + ", handler="
                            + (userSpotOperationHandler != null) + "]"));
            }
            long operation = allocateCorrelation();
            return userSpotOperationHandler.close(
                new ZLinkInternalMeshNode.UserSpotCloseRequest(
                    routingId,
                    localGeneration,
                    0,
                    operation,
                    intent));
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(targetNodeRid);
        if (peer.isEmpty() || localDescriptor == null
            || !intent.target().targetNodeRid().equals(targetNodeRid)
            || intent.target().targetNodeGeneration()
                != peer.orElseThrow().descriptor()
                    .lifecycleGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote User Spot close target is not connected"));
        }
        long correlation = allocateCorrelation();
        ZLinkServiceOperationRegistry.Operation<
            ZLinkInternalMeshNode.UserSpotCloseResponse> operation =
                operations.register(timeout);
        ZLinkServiceM6BWireCodec.UserSpotClose command =
            new ZLinkServiceM6BWireCodec.UserSpotClose(
                correlation,
                0,
                correlation,
                routingId,
                localDescriptor.lifecycleGeneration(),
                intent.target(),
                intent.deadlineUnixMs());
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        port.request(
                requireStarted(),
                targetNodeRid,
                List.of(statefulWire.encodeUserSpotCloseHeader(command)),
                timeout)
            .whenComplete((replyFrames, failure) -> {
                RequestResult result = requestResult(failure);
                if (!terminal.tryWin(requestTerminalCause(result))) {
                    return;
                }
                completeUserSpotClose(
                    operation.id(),
                    correlation,
                    result,
                    replyFrames == null ? List.of() : replyFrames);
            });
        return operation.completion();
    }

    @Override
    public void rememberSpotAuthority(
        ZLinkInternalMeshNode.SpotAuthorityRoute route) {
        ((ZLinkJavaRawSpotNode) spotNode()).rememberSpotAuthority(
            route.targetNodeRid(),
            route.spotId(),
            route.objectGeneration(),
            route.authorityOwnerGeneration(),
            route.ownerLeaseGeneration());
    }

    @Override
    public void forgetSpotAuthority(
        ZLinkInternalMeshNode.SpotAuthorityRoute route) {
        ((ZLinkJavaRawSpotNode) spotNode()).forgetSpotAuthority(
            route.targetNodeRid(),
            route.spotId(),
            route.objectGeneration(),
            route.authorityOwnerGeneration());
    }

    @Override
    public void registerInstanceIntent(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        ((ZLinkJavaRawSpotNode) spotNode())
            .reconcileInstanceSpotAuthority(stableType, route);
    }

    @Override
    public void registerInstanceSpotType(String stableType) {
        ((ZLinkJavaRawSpotNode) spotNode())
            .registerInstanceSpotType(stableType);
    }

    @Override
    public void registerInstanceSpotType(
        String stableType,
        ZLinkInternalMeshNode.InstanceSpotActivationHandler handler) {
        ((ZLinkJavaRawSpotNode) spotNode())
            .registerInstanceSpotType(stableType, handler);
    }

    @Override
    public void forgetInstanceIntent(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        ((ZLinkJavaRawSpotNode) spotNode())
            .forgetInstanceSpotAuthority(route);
    }

    private void completeUserSpotCreate(
        UUID operationId,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote User Spot create transport failed: "
                        + result));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "invalid User Spot create reply frame count");
            }
            ZLinkServiceM6BWireCodec.UserSpotCreateReply reply =
                statefulWire.decodeUserSpotCreateReply(
                    frames.getFirst());
            if (reply.correlation() != correlation) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot create correlation mismatch");
            }
            if (reply.terminalResult() != 0 || reply.failureCode() != 0) {
                //  Classify the carried terminal + fine failure code via the
                //  authoritative ownership-aware translator instead of
                //  collapsing to a generic rejection
                //  (spec 32-framework-error-model:81-118, 99-108).
                throw new ZLinkFrameworkException(
                    ZLinkBackendRequestResult
                        .fromWireTerminal(reply.terminalResult())
                        .toFrameworkErrorKind(reply.failureCode()),
                    "remote User Spot create failed: terminal="
                        + reply.terminalResult()
                        + " failureCode=" + reply.failureCode());
            }
            if (reply.success() == null
                || (reply.success().result()
                        == ZLinkServiceM6BWireCodec
                            .UserSpotCreateResult.EXISTING
                    && frames.size() != 1)) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot create reply was malformed");
            }
            List<Message> applicationReply;
            if (frames.size() == 2) {
                applicationReply = decodeApplicationMessages(frames.get(1));
            } else {
                applicationReply = List.of();
            }
            ZLinkInternalMeshNode.UserSpotCreateResponse response =
                new ZLinkInternalMeshNode.UserSpotCreateResponse(
                    reply.success().result(),
                    reply.success().spotId(),
                    reply.success().objectGeneration(),
                    applicationReply);
            if (!operations.complete(operationId, response)) {
                response.applicationReply().forEach(Message::close);
            }
        } catch (ZLinkFrameworkException failure) {
            operations.completeExceptionally(operationId, failure);
        } catch (IllegalArgumentException failure) {
            //  A codec/malformed-wire failure (ZLinkServiceWireException and
            //  friends derive from IllegalArgumentException) means the reply
            //  can't be processed -> ProtocolError
            //  (spec 32-framework-error-model:91-92).
            operations.completeExceptionally(
                operationId,
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot create reply could not be processed",
                    failure));
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    private void completeActorCreate(
        UUID operationId,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote Actor create transport failed: " + result));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "invalid Actor create reply frame count");
            }
            var reply = statefulWire.decodeActorCreateReply(
                frames.getFirst(), meshName);
            if (reply.correlation() != correlation) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor create correlation mismatch");
            }
            var terminal = reply.terminal();
            if (frames.size() == 2) {
                //  Spec 51-internal-service-wire-protocol:97 — a failed
                //  creation terminal carries no application payload tail; an
                //  attached second frame on a failed terminal is rejected as a
                //  protocol error instead of honoring the carried failure.
                if (terminal.terminalResult() != 0) {
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        "Actor create failed terminal carries a payload");
                }
                wire.decodeApplicationPayload(frames.get(1));
                terminal =
                    new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                        terminal.terminalResult(),
                        terminal.failureCode(),
                        terminal.creation(),
                        frames.get(1));
            }
            operations.complete(
                operationId,
                new ZLinkInternalMeshNode.ActorCreateResponse(
                    statefulWire.encodeCreationOperationTerminal(
                        terminal)));
        } catch (ZLinkFrameworkException failure) {
            operations.completeExceptionally(operationId, failure);
        } catch (IllegalArgumentException failure) {
            //  A codec/malformed-wire failure (ZLinkServiceWireException and
            //  friends derive from IllegalArgumentException) means the reply
            //  can't be processed -> ProtocolError
            //  (spec 32-framework-error-model:91-92).
            operations.completeExceptionally(
                operationId,
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote Actor create reply could not be processed",
                    failure));
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    private void completeUserSpotClose(
        UUID operationId,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote User Spot close transport failed: "
                        + result));
            return;
        }
        try {
            if (frames.size() != 1) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "invalid User Spot close reply frame count");
            }
            ZLinkServiceM6BWireCodec.UserSpotCloseReply reply =
                statefulWire.decodeUserSpotCloseReply(
                    frames.getFirst());
            if (reply.correlation() != correlation) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot close correlation mismatch");
            }
            if (reply.terminalResult() != 0 || reply.failureCode() != 0) {
                //  Classify the carried terminal + fine failure code via the
                //  authoritative ownership-aware translator instead of
                //  collapsing to a generic rejection
                //  (spec 32-framework-error-model:81-118, 99-108).
                throw new ZLinkFrameworkException(
                    ZLinkBackendRequestResult
                        .fromWireTerminal(reply.terminalResult())
                        .toFrameworkErrorKind(reply.failureCode()),
                    "remote User Spot close failed: terminal="
                        + reply.terminalResult()
                        + " failureCode=" + reply.failureCode());
            }
            if (reply.closed() == null) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot close reply was malformed");
            }
            operations.complete(
                operationId,
                new ZLinkInternalMeshNode.UserSpotCloseResponse(
                    reply.closed()));
        } catch (ZLinkFrameworkException failure) {
            operations.completeExceptionally(operationId, failure);
        } catch (IllegalArgumentException failure) {
            //  A codec/malformed-wire failure (ZLinkServiceWireException and
            //  friends derive from IllegalArgumentException) means the reply
            //  can't be processed -> ProtocolError
            //  (spec 32-framework-error-model:91-92).
            operations.completeExceptionally(
                operationId,
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "remote User Spot close reply could not be processed",
                    failure));
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    CompletionStage<Void> sendBoundSession(
        ZLinkJavaRawSpotNode.RemoteStreamBinding binding,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(binding.sessionOwnerNodeRid());
        if (peer.isEmpty()
            || peer.orElseThrow().descriptor().lifecycleGeneration()
                != binding.sessionOwnerNodeGeneration()
            || !isReadyPeer(binding.sessionOwnerNodeRid())) {
            streamTrace(STREAM_TRACE ? "send bound session rejected actor="
                + actorSummary(binding.actor())
                + " session=" + binding.sessionRid()
                + " binding=" + binding.bindingGeneration()
                + " reason=route-or-binding-fence"
                + " peerPresent=" + peer.isPresent()
                + " peerGeneration=" + peer.map(value ->
                    value.descriptor().lifecycleGeneration()).orElse(-1L)
                + " expectedOwnerGeneration="
                + binding.sessionOwnerNodeGeneration()
                + " ready=" + isReadyPeer(binding.sessionOwnerNodeRid()) : null);
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        List<byte[]> frames = List.of(
            statefulWire.encodeBoundSessionSendHeader(
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    binding.actor(),
                    binding.targetNodeGeneration(),
                    binding.authorityOwnerGeneration(),
                    binding.actorOwnerLeaseGeneration()),
                binding.bindingGeneration()),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(
                requireStarted(),
                binding.sessionOwnerNodeRid(),
                frames)
            .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                "send bound session "
                    + (failure == null ? "accepted" : "failed")
                    + " actor=" + actorSummary(binding.actor())
                    + " session=" + binding.sessionRid()
                    + " binding=" + binding.bindingGeneration() : null));
    }

    private void completeActorRequest(
        UUID operationId,
        ZLinkBackendActorRef actor,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    backendResult(result),
                    Optional.of(actor.nodeRid()),
                    Optional.empty(),
                    Optional.of(correlation),
                    List.of()));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new IllegalArgumentException(
                    "invalid Actor reply frame count");
            }
            //  Generic application request-to-Actor completion path: not one
            //  of request-specific-tail's tail-bearing originalOperationKind
            //  cases, so the tail MUST be empty (schema "otherwise" branch).
            //  See completeRequest() below for the full rationale.
            if (frames.getFirst().length != 21) {
                throw new IllegalArgumentException(
                    "generic Actor reply carries an operation-specific tail");
            }
            ZLinkServiceM6AWireCodec.Reply header =
                wire.decodeReplyHeader(frames.getFirst());
            if (header.correlation() != correlation
                || (header.terminalResult() == 0) != (frames.size() == 2)) {
                throw new IllegalArgumentException(
                    "Actor reply terminal mismatch");
            }
            List<Message> replyParts = header.terminalResult() == 0
                ? decodeApplicationMessages(frames.get(1))
                : List.of();
            ZLinkBackendReceived received = new ZLinkBackendReceived(
                header.terminalResult() == 0
                    ? ZLinkBackendRequestResult.OK
                    : backendResult(header.terminalResult()),
                header.failureCode(),
                Optional.of(actor.nodeRid()),
                Optional.empty(),
                Optional.of(correlation),
                replyParts);
            if (!operations.complete(operationId, received)) {
                received.close();
            }
        } catch (RuntimeException failure) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.PROTOCOL_ERROR,
                    Optional.of(actor.nodeRid()),
                    Optional.empty(),
                    Optional.of(correlation),
                    List.of()));
        }
    }

    private CompletionStage<ZLinkBackendReceived> request(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        Duration timeout,
        String channelName) {
        long correlation = allocateCorrelation();
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        List<byte[]> frames = new ArrayList<>();
        frames.add(channelName == null
            ? wire.encodeNodeRequestHeader(correlation, flags)
            : wire.encodeChannelRequestHeader(correlation, channelName, flags));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        ZLinkServiceOperationRegistry.Operation<ZLinkBackendReceived> operation =
            operations.register(timeout);
        operation.completion().whenComplete((reply, failure) -> {
            if (channelName != null) {
                streamTrace(STREAM_TRACE ? "request-channel-complete channel="
                    + channelName + " target=" + target
                    + " result=" + (failure == null
                        ? reply.result()
                        : requestResult(failure)) : null);
            }
        });
        TransportPair pair = transportPairFor(target);
        if (channelName != null) {
            streamTrace(STREAM_TRACE ? "request-channel-pair channel=" + channelName
                + " target=" + target
                + " pair=" + (pair == null
                    ? "none"
                    : pair.id() + "/" + pair.generation()) : null);
        }
        CompletionStage<List<byte[]>> submitted = pair == null
            ? port.request(
                requireStarted(),
                target,
                frames,
                timeout)
            : port.request(
                requireStarted(),
                target,
                pair.id(),
                pair.generation(),
                frames,
                timeout);
        submitted.whenComplete((replyFrames, failure) -> completeRequest(
            operation.id(),
            target,
            correlation,
            requestResult(failure),
            replyFrames == null ? List.of() : replyFrames));
        return operation.completion();
    }

    private void completeRequest(
        UUID operationId,
        RoutingId target,
        long correlation,
        RequestResult result,
        List<byte[]> frames) {
        if (result != RequestResult.OK) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    backendResult(result),
                    Optional.of(target),
                    Optional.empty(),
                    Optional.empty(),
                    List.of()));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new IllegalArgumentException("invalid service reply frame count");
            }
            //  This is the generic requestToNode/requestToChannel completion
            //  path: its originalOperationKind is not one of
            //  request-specific-tail's tail-bearing cases
            //  (service-wire-v1.schema.json), so the schema's "otherwise"
            //  branch applies and the tail MUST be empty.
            //  decodeReplyHeader itself now permissively accepts
            //  tail-bearing frames (fix for the residual-convergence tail
            //  rejection bug), so this generic caller enforces the
            //  empty-tail contract explicitly, matching Node's
            //  raw-service-mesh-runtime.ts generic reply guard ("Generic
            //  node/channel reply carries an operation-specific tail.").
            if (frames.getFirst().length != 21) {
                throw new IllegalArgumentException(
                    "generic service reply carries an operation-specific tail");
            }
            ZLinkServiceM6AWireCodec.Reply header =
                wire.decodeReplyHeader(frames.getFirst());
            if (header.correlation() != correlation
                || (header.terminalResult() == 0) != (frames.size() == 2)) {
                throw new IllegalArgumentException("service reply terminal mismatch");
            }
            List<Message> parts = header.terminalResult() == 0
                ? decodeApplicationMessages(frames.get(1))
                : List.of();
            ZLinkBackendRequestResult terminal = header.terminalResult() == 0
                ? ZLinkBackendRequestResult.OK
                : backendResult(header.terminalResult());
            ZLinkBackendReceived received = new ZLinkBackendReceived(
                terminal,
                header.failureCode(),
                Optional.of(target),
                Optional.empty(),
                Optional.empty(),
                parts);
            if (!operations.complete(operationId, received)) {
                received.close();
            }
        } catch (RuntimeException failure) {
            operations.complete(
                operationId,
                new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.PROTOCOL_ERROR,
                    Optional.of(target),
                    Optional.empty(),
                    Optional.empty(),
                    List.of()));
        }
    }

    void admitPeerChannels(
        RoutingId peerRoutingId,
        Map<String, Integer> channels) {
        Objects.requireNonNull(peerRoutingId, "peerRoutingId");
        Map<String, Integer> validated = new LinkedHashMap<>();
        channels.entrySet().stream()
            .sorted(Map.Entry.comparingByKey())
            .forEach(entry -> {
                String name = requireChannel(entry.getKey());
                int weight = entry.getValue();
                if (weight < 0 || weight > 10_000) {
                    throw new IllegalArgumentException(
                        "peer channel weight must be in 0..10000");
                }
                validated.put(name, weight);
            });
        Map<String, Integer> snapshot = Map.copyOf(validated);
        admittedPeerChannels.put(peerRoutingId, snapshot);
        knownPeerChannels.put(peerRoutingId, snapshot);
        disconnectedPeers.remove(peerRoutingId);
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        state = MeshNodeState.STOPPED;
        ExecutorService currentPump = pump;
        if (currentPump != null) {
            currentPump.shutdownNow();
            awaitExecutorTermination(currentPump);
        }
        ZLinkServiceMailbox currentMailbox = mailbox;
        if (currentMailbox != null) {
            currentMailbox.close();
        }
        dispatchEnvelopes.values().forEach(ZLinkMeshDispatchRecord::close);
        dispatchEnvelopes.clear();
        pendingReplyRelays.values().forEach(pending ->
            {
                pending.cancelTimeout();
                pending.completion.completeExceptionally(
                    new IllegalStateException("MeshNode is closed"));
            });
        pendingReplyRelays.clear();
        admittedPeerChannels.clear();
        knownPeerChannels.clear();
        connectionIds.clear();
        transportPairs.clear();
        applicationTransportPairs.clear();
        admissionControlReadyConnections.clear();
        pendingConnectionIds.clear();
        monitorConnectionIds.clear();
        operations.close();
        SocketMonitor currentMonitor = rawMonitor;
        if (currentMonitor != null) {
            currentMonitor.close();
        }
        port.close();
    }

    private static void awaitExecutorTermination(ExecutorService executor) {
        boolean interrupted = false;
        try {
            if (!executor.awaitTermination(5, TimeUnit.SECONDS)) {
                executor.shutdownNow();
                executor.awaitTermination(5, TimeUnit.SECONDS);
            }
        } catch (InterruptedException interruption) {
            interrupted = true;
            executor.shutdownNow();
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private void startPump() {
        pump = Executors.newSingleThreadExecutor(Thread.ofVirtual()
            .name("zlink-jvm-raw-mesh-" + meshName)
            .factory());
        pump.execute(() -> {
            while (!closed.get()) {
                long now = System.nanoTime();
                drainMonitorEvents();
                drainPeerCloseRequests();
                announceExpectedPeers(now);
                tickLiveness(now);
                Optional<ZLinkJavaRawServicePort.Inbound> inbound;
                try {
                    inbound = port.receive(requireStarted());
                } catch (RuntimeException ignored) {
                    if (closed.get()) {
                        return;
                    }
                    // A transient transport receive failure must not stop the
                    // only service pump. Retry on the next pump iteration.
                    LockSupport.parkNanos(
                        Duration.ofMillis(1).toNanos());
                    continue;
                }
                if (inbound.isEmpty()) {
                    LockSupport.parkNanos(
                        Duration.ofMillis(1).toNanos());
                    continue;
                }
                dispatch(inbound.orElseThrow());
            }
        });
    }

    private boolean hasCurrentInfrastructureControlSource(
        RoutingId source,
        int command) {
        if (command == ServiceWireConstants.COMMAND_HELLO
            || command == ServiceWireConstants.COMMAND_ADMIT
            || command == ServiceWireConstants.COMMAND_REJECT
            || command == ServiceWireConstants.COMMAND_UPDATE) {
            return true;
        }
        ZLinkServiceTopologyRegistry current = topology;
        if (current == null) {
            return false;
        }
        return current.peer(source)
            .filter(peer ->
                // lifecycleGeneration is a non-zero opaque equality token
                // (spec 01-glossary "Lifecycle generation": ".NET 표기: ulong
                // LifecycleGeneration" / "숫자 크기로 실행 순서를 판단하지
                // 않는다"). A remote ulong value with its top bit set decodes
                // to a negative Java long; a signed `> 0` sentinel silently
                // treats that legitimate generation as unassigned and drops
                // every non-admission control frame (liveness probe/ack,
                // relocation control) for the whole connection.
                peer.descriptor().lifecycleGeneration() != 0
                    && peer.connectionId().equals(connectionIds.get(source)))
            .isPresent();
    }

    static int allowedInfrastructureControlCommand(List<byte[]> parts) {
        if (parts == null
            || parts.isEmpty()
            || parts.size() > MAX_INFRASTRUCTURE_CONTROL_PARTS) {
            return -1;
        }
        int command =
            allowedInfrastructureControlCommand(parts.getFirst());
        if (!isValidInfrastructureControlShape(command, parts.size())) {
            return -1;
        }
        long maximumBytes = isPayloadBearingInfrastructureCommand(command)
            ? MAX_INFRASTRUCTURE_PAYLOAD_BYTES
            : MAX_INFRASTRUCTURE_CONTROL_BYTES;
        long totalBytes = 0;
        for (byte[] part : parts) {
            if (part == null) {
                return -1;
            }
            totalBytes += part.length;
            if (totalBytes > maximumBytes) {
                return -1;
            }
        }
        return command;
    }

    private static int allowedInfrastructureControlCommand(byte[] head) {
        if (head == null
            || head.length < PREFIX_BYTES
            || head[0] != (byte) ServiceWireConstants.MAGIC_0
            || head[1] != (byte) ServiceWireConstants.MAGIC_1
            || head[2] != (byte) ServiceWireConstants.WIRE_MAJOR) {
            return -1;
        }
        int command = Byte.toUnsignedInt(head[3]);
        return isAllowedInfrastructureControlCommand(command) ? command : -1;
    }

    private static boolean isAllowedInfrastructureControlCommand(int command) {
        return command == ServiceWireConstants.COMMAND_HELLO
            || command == ServiceWireConstants.COMMAND_ADMIT
            || command == ServiceWireConstants.COMMAND_REJECT
            || command == ServiceWireConstants.COMMAND_UPDATE
            || command == ServiceWireConstants.COMMAND_LIVENESS_PROBE
            || command == ServiceWireConstants.COMMAND_LIVENESS_ACK
            || command == ServiceWireConstants.COMMAND_REPLY
            || command == ServiceWireConstants.COMMAND_ACTOR_LEFT
            || command == ServiceWireConstants.COMMAND_RELOCATION_READY
            || command == ServiceWireConstants.COMMAND_RELOCATION_FAILED
            || command == ServiceWireConstants.COMMAND_RELOCATION_DATA
            || command == ServiceWireConstants.COMMAND_REPLY_RELAY
            || command == ServiceWireConstants.COMMAND_RELOCATION_CUTOVER
            || command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE
            || command == ServiceWireConstants.COMMAND_RELOCATION_STATE
            || command == ServiceWireConstants.COMMAND_REPLY_RELAY_ACK
            || command == ServiceWireConstants.COMMAND_MESSAGE_FOLLOW
            || command == ServiceWireConstants.COMMAND_BOUND_SESSION_REPLACED
            || command == ServiceWireConstants.COMMAND_SESSION_RELOCATION_SEAL
            || command == ServiceWireConstants.COMMAND_SESSION_RELOCATION_SEALED
            || command == ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE;
    }

    private static boolean isValidInfrastructureControlShape(
        int command,
        int partCount) {
        if (command < 0) {
            return false;
        }
        return switch (command) {
            case ServiceWireConstants.COMMAND_REPLY,
                 ServiceWireConstants.COMMAND_REPLY_RELAY ->
                partCount == 1 || partCount == 2;
            default -> partCount == 1;
        };
    }

    private static boolean isPayloadBearingInfrastructureCommand(
        int command) {
        return command == ServiceWireConstants.COMMAND_REPLY
            || command == ServiceWireConstants.COMMAND_REPLY_RELAY
            || command == ServiceWireConstants.COMMAND_RELOCATION_DATA
            || command == ServiceWireConstants.COMMAND_RELOCATION_STATE;
    }

    private void dispatch(ZLinkJavaRawServicePort.Inbound inbound) {
        boolean handedOff = false;
        try {
        List<byte[]> frames = inbound.frames();
        if (frames.isEmpty() || frames.getFirst().length < PREFIX_BYTES) {
            return;
        }
        byte[] head = frames.getFirst();
        int infrastructureCommand =
            allowedInfrastructureControlCommand(head);
        if (infrastructureCommand >= 0
            && (allowedInfrastructureControlCommand(frames)
                    != infrastructureCommand
                || !hasCurrentInfrastructureControlSource(
                    inbound.source(), infrastructureCommand))) {
            if (STREAM_TRACE) {
                Optional<ZLinkServiceTopologyRegistry.Peer> tracedPeer =
                    topology.peer(inbound.source());
                streamTrace("infrastructure-control-dropped command="
                    + infrastructureCommand
                    + " source=" + inbound.source()
                    + " shapeMismatch=" + (allowedInfrastructureControlCommand(frames)
                        != infrastructureCommand)
                    + " peerPresent=" + tracedPeer.isPresent()
                    + " peerConnectionId=" + tracedPeer
                        .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                        .orElse("none")
                    + " trackedConnectionId=" + connectionIds.get(inbound.source())
                    + " lifecycleGeneration=" + tracedPeer
                        .map(peer -> peer.descriptor().lifecycleGeneration())
                        .map(String::valueOf)
                        .orElse("none"));
            }
            return;
        }
        ZLinkServiceM6AWireCodec.Header header;
        try {
            header = wire.decodeHeader(head);
        } catch (RuntimeException invalid) {
            return;
        }
        int command = header.command();
        int flags = header.flags();
        streamTrace(STREAM_TRACE ? "service-received command=" + command
            + " source=" + inbound.source()
            + " requestSequence=" + inbound.requestSequence()
            + " frameCount=" + frames.size() : null);
        if (command == ServiceWireConstants.COMMAND_HELLO
            || command == ServiceWireConstants.COMMAND_ADMIT
            || command == ServiceWireConstants.COMMAND_UPDATE) {
            dispatchAdmission(inbound, command);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_REJECT) {
            if (frames.size() == 1) {
                try {
                    int reason = wire.decodeReject(head);
                    streamTrace(STREAM_TRACE ? "reject-received source=" + inbound.source()
                        + " reason=" + reason : null);
                    TransportPair rejectedPair = transportPair(inbound);
                    if (rejectedPair != null && reason == 3) {
                        // The reply is pinned to the admission pair, so it is
                        // authoritative only for that losing physical
                        // candidate. If the local topology still calls that
                        // exact pair current (for example after an expected
                        // route mismatch), remove the corresponding logical
                        // admission too; otherwise leave the survivor alone.
                        String rejectedConnectionId = topology
                            .peer(inbound.source())
                            .filter(current -> selectedTransportPair(current)
                            .filter(rejectedPair::equals)
                            .isPresent())
                            .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                            .orElse(null);
                        disconnectTransportPair(rejectedPair);
                        if (rejectedConnectionId != null) {
                            disconnectAdmitted(
                                inbound.source(), rejectedConnectionId);
                        }
                    }
                    if (reason == 4) {
                        disconnectAdmitted(inbound.source());
                        admittedPeerObjectRoles.put(
                            inbound.source(),
                            ZLinkServiceNodeDescriptor.ObjectRole.CLIENT);
                        notRequiredPeers.add(inbound.source());
                        if (rejectedPair == null) {
                            disconnectNotRequiredTransport(inbound.source());
                        } else {
                            disconnectTransportPair(rejectedPair);
                        }
                    }
                } catch (RuntimeException ignored) {
                }
            }
            return;
        }
        if (command == ServiceWireConstants.COMMAND_LIVENESS_PROBE
            || command == ServiceWireConstants.COMMAND_LIVENESS_ACK) {
            dispatchLiveness(inbound, command);
            return;
        }
        if (topology.peer(inbound.source()).isEmpty()) {
            return;
        }
        if (command == ServiceWireConstants.COMMAND_MESSAGE_FOLLOW) {
            dispatchMessageFollow(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_ACTOR_LEFT) {
            dispatchActorLeft(inbound);
            return;
        }
        if (isCanonicalRelocationControl(command)) {
            dispatchCanonicalRelocationControl(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_REPLY_RELAY) {
            dispatchRelocationReplyRelay(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_REPLY_RELAY_ACK) {
            dispatchRelocationReplyRelayAck(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE) {
            dispatchSessionRelocationRoute(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_SESSION_RELOCATION_SEAL) {
            dispatchSessionRelocationSeal(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_BOUND_SESSION_REPLACED) {
            dispatchBoundSessionReplaced(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_SPOT_SEND
            || command == ServiceWireConstants.COMMAND_SPOT_REQUEST) {
            handedOff = dispatchSpot(inbound, flags);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_LOGICAL_MULTICAST) {
            dispatchLogicalMulticast(inbound, flags);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_ACTOR_SEND
            || command == ServiceWireConstants.COMMAND_ACTOR_REQUEST) {
            handedOff = dispatchActor(inbound, flags);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_ACTOR_JOIN) {
            dispatchCanonicalActorJoin(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_INSTANCE_SPOT) {
            dispatchInstanceSpot(inbound, flags);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_USER_SPOT_CREATE) {
            dispatchUserSpotCreate(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_USER_SPOT_CLOSE) {
            dispatchUserSpotClose(inbound);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_ACTOR_CREATE) {
            dispatchActorCreate(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_BOUND_SESSION_BIND) {
            dispatchBoundSessionBind(inbound);
            return;
        }
        if (command
            == ServiceWireConstants.COMMAND_BOUND_SESSION_SEND) {
            dispatchBoundSessionSend(inbound);
            return;
        }
        RecordKind kind;
        String channelName = null;
        Long correlation = null;
        if (command == ServiceWireConstants.COMMAND_NODE_SEND) {
            if (head.length != PREFIX_BYTES) {
                return;
            }
            kind = RecordKind.NODE_SEND;
        } else if (command == ServiceWireConstants.COMMAND_NODE_REQUEST) {
            kind = RecordKind.NODE_REQUEST;
            correlation = wire.decodeNodeRequestHeader(head);
        } else if (command == ServiceWireConstants.COMMAND_CHANNEL_SEND) {
            kind = RecordKind.CHANNEL_SEND;
            channelName = wire.decodeChannelSendHeader(head);
        } else if (command == ServiceWireConstants.COMMAND_CHANNEL_REQUEST) {
            kind = RecordKind.CHANNEL_REQUEST;
            ZLinkServiceM6AWireCodec.ChannelRequest request =
                wire.decodeChannelRequestHeader(head);
            correlation = request.correlation();
            channelName = request.channelName();
        } else {
            return;
        }
        if (kind == RecordKind.CHANNEL_REQUEST
            || kind == RecordKind.CHANNEL_SEND) {
            streamTrace(STREAM_TRACE ? "channel-application-received kind=" + kind
                + " channel=" + channelName
                + " source=" + inbound.source()
                + " requestSequence=" + inbound.requestSequence()
                + " correlation=" + correlation : null);
        }
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1
            || (correlation != null && inbound.requestSequence() == null)) {
            replyApplicationProtocolFailure(inbound, correlation);
            return;
        }
        byte[] metadata = payloadOffset == 2
            ? frames.get(1).clone()
            : new byte[0];
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        List<Message> messages;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
            messages = decodeApplicationMessages(payload);
        } catch (RuntimeException invalid) {
            replyApplicationProtocolFailure(inbound, correlation);
            return;
        }
        boolean relocationControl = false;
        if (channelName == null && correlation != null && messages.size() == 2) {
            try {
                relocationControl = RELOCATION_CONTROL_PACKET.equals(
                    messages.getFirst().toUtf8String());
            } catch (RuntimeException invalidPacketName) {
                relocationControl = false;
            }
        }
        if (relocationControl) {
            try {
                byte[] relocationCommand = messages.get(1).toByteArray();
                dispatchRelocationControl(
                    inbound, correlation, relocationCommand);
            } finally {
                messages.forEach(Message::close);
            }
            return;
        }
        String contentType;
        try {
            contentType = applicationContentType(messages);
        } catch (RuntimeException failure) {
            messages.forEach(Message::close);
            throw failure;
        }
        ReceiveRecord receive = new ReceiveRecord(
            kind,
            ReadyDomain.APPLICATION.value(),
            inbound.source(),
            null,
            null,
            null,
            OperationKind.NONE,
            null,
            channelName,
            null,
            contentType,
            correlation,
            metadata,
            0,
            0,
            0,
            messages.size());
        Long requestCorrelation = correlation;
        AtomicBoolean replied = new AtomicBoolean();
        Consumer<List<Message>> reply = correlation == null
            ? null
            : replyParts -> {
                if (!replied.compareAndSet(false, true)) {
                    replyParts.forEach(Message::close);
                    throw new IllegalStateException(
                        "service request already has a terminal reply");
                }
                try {
                    List<byte[]> replyFrames = List.of(
                        wire.encodeReplyHeader(requestCorrelation, 0, 0),
                        wire.encodeApplicationPayload(
                            applicationPayload(replyParts)));
                    port.reply(
                        requireStarted(),
                        inbound.source(),
                        inbound.requestSequence(),
                        replyFrames);
                } finally {
                    replyParts.forEach(Message::close);
                }
            };
        ZLinkMeshDispatchRecord dispatch = new ZLinkMeshDispatchRecord(
            new ReadyRecord(
                OwnerKind.NODE,
                ReadyDomain.APPLICATION.value(),
                null,
                null),
            receive,
            messages,
            reply,
            inbound::close);
        handedOff = true;
        long envelopeId = nextDispatchEnvelope.getAndIncrement();
        ZLinkServiceMailbox currentMailbox = mailbox;
        String owner = channelName == null
            ? "node:" + routingId
            : "channel:" + channelName;
        boolean enqueued = currentMailbox != null
            && currentMailbox.tryEnqueue(new ZLinkServiceMailbox.Record(
                owner,
                ZLinkServiceMailbox.Domain.APPLICATION,
                frames,
                inbound.source().toBytes(),
                null,
                envelopeId));
        if (!enqueued) {
            streamTrace(STREAM_TRACE ? "channel-application-mailbox-rejected channel="
                + channelName + " source=" + inbound.source()
                + " correlation=" + correlation
                + " mailbox=" + (currentMailbox != null) : null);
            dispatch.close();
            return;
        }
        dispatchEnvelopes.put(envelopeId, dispatch);
        streamTrace(STREAM_TRACE ? "channel-application-mailbox-enqueued channel="
            + channelName + " source=" + inbound.source()
            + " correlation=" + correlation + " envelope=" + envelopeId : null);
        drainApplicationMailbox();
        } finally {
            if (!handedOff) {
                inbound.close();
            }
        }
    }

    private void replyApplicationProtocolFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        Long correlation) {
        if (correlation == null || inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(correlation, 104, 12)));
    }

    private void dispatchRelocationControl(
        ZLinkJavaRawServicePort.Inbound inbound,
        long correlation,
        byte[] command) {
        ZLinkInternalMeshNode.RelocationControlHandler handler =
            relocationControlHandler;
        if (handler == null || command.length == 0) {
            port.reply(
                requireStarted(),
                inbound.source(),
                inbound.requestSequence(),
                List.of(wire.encodeReplyHeader(correlation, 105, 2)));
            return;
        }
        CompletionStage<byte[]> completion;
        try {
            completion = Objects.requireNonNull(
                handler.handle(inbound.source(), command.clone()),
                "relocation handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        ZLinkTerminalWinner terminal =
            new ZLinkTerminalWinner();
        completion.whenComplete((reply, failure) -> {
            if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                    .completion.ZLinkTerminalWinner.Cause.RESPONSE)) {
                return;
            }
            if (failure != null || reply == null || reply.length == 0) {
                port.reply(
                    requireStarted(),
                    inbound.source(),
                    inbound.requestSequence(),
                    List.of(wire.encodeReplyHeader(correlation, 105, 2)));
                return;
            }
            List<Message> replyParts = List.of(Message.from(reply));
            try {
                port.reply(
                    requireStarted(),
                    inbound.source(),
                    inbound.requestSequence(),
                    List.of(
                        wire.encodeReplyHeader(correlation, 0, 0),
                        wire.encodeApplicationPayload(
                            applicationPayload(replyParts))));
            } finally {
                replyParts.forEach(Message::close);
            }
        });
    }

    private void dispatchCanonicalRelocationControl(
        ZLinkJavaRawServicePort.Inbound inbound) {
        ZLinkInternalMeshNode.CanonicalRelocationControlHandler handler =
            canonicalRelocationControlHandler;
        if (handler == null || inbound.frames().size() != 1) {
            return;
        }
        byte[] command = inbound.frames().getFirst().clone();
        try {
            validateCanonicalRelocationControl(command);
            int code = Byte.toUnsignedInt(command[3]);
            if (inbound.requestSequence() != null
                && code != ServiceWireConstants.COMMAND_RELOCATION_PREPARE) {
                return;
            }
            CompletionStage<byte[]> completion =
                Objects.requireNonNull(
                    handler.handle(
                        inbound.source(), inbound.requestSequence(), command),
                    "canonical relocation handler returned null");
            completion.whenComplete((reply, failure) -> {
                if (failure != null) {
                    streamTrace(STREAM_TRACE
                        ? "canonical-relocation-control-failed source="
                            + inbound.source() + " failure=" + failure
                        : null);
                    return;
                }
                if (inbound.requestSequence() == null) {
                    return;
                }
                try {
                    port.reply(
                        requireStarted(),
                        inbound.source(),
                        inbound.requestSequence(),
                        List.of(canonicalPrepareReply(reply)));
                } catch (RuntimeException rejected) {
                    streamTrace(STREAM_TRACE
                        ? "canonical-relocation-reply-rejected source="
                            + inbound.source() + " failure=" + rejected
                        : null);
                }
            });
        } catch (RuntimeException failure) {
            // Invalid or rejected maintenance records never enter an
            // application mailbox and have no request/reply terminal.
            streamTrace(STREAM_TRACE ? "canonical-relocation-control-rejected source="
                + inbound.source()
                + " failure=" + failure : null);
        }
    }

    private void dispatchActorLeft(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.requestSequence() != null
            || inbound.frames().size() != 1) {
            return;
        }
        ZLinkServiceM6BWireCodec.ActorLeft left;
        try {
            left = statefulWire.decodeActorLeft(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        var handler = actorLeftHandler;
        if (handler == null) {
            return;
        }
        try {
            CompletionStage<Void> completion = Objects.requireNonNull(
                handler.handle(inbound.source(), left),
                "Actor Left handler returned null");
            completion.exceptionally(failure -> {
                LOGGER.warning("Actor Left handler failed: "
                    + unwrap(failure));
                return null;
            });
        } catch (RuntimeException failure) {
            LOGGER.warning("Actor Left handler failed to start: " + failure);
        }
    }

    private void dispatchMessageFollow(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.requestSequence() != null
            || inbound.frames().size() != 1) {
            return;
        }
        ZLinkServiceMessageFollowWireCodec.Notice notice;
        try {
            notice = new ZLinkServiceMessageFollowWireCodec().decode(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        ZLinkInternalMeshNode.MessageFollowHandler handler =
            messageFollowHandler;
        if (handler == null) {
            return;
        }
        try {
            handler.handle(inbound.source(), notice);
        } catch (RuntimeException ignored) {
            // A rejected maintenance notice never enters an application
            // mailbox and cannot alter the transport receive loop.
        }
    }

    private void dispatchRelocationReplyRelay(
        ZLinkJavaRawServicePort.Inbound inbound) {
        var handler = relocationReplyRelayHandler;
        if (handler == null
            || inbound.requestSequence() != null
            || inbound.frames().isEmpty()) {
            return;
        }
        byte[] command33 = inbound.frames().getFirst();
        var relocationWire = new ZLinkServiceRelocationWireCodec();
        ZLinkServiceRelocationWireCodec.ReplyRelay relay;
        try {
            relay = relocationWire.decodeReplyRelay(command33);
            validateReplyRelayPayload(relay, inbound.frames().size() - 1);
        } catch (RuntimeException invalid) {
            return;
        }
        List<byte[]> payload = cloneFrames(inbound.frames(), 1);
        CompletionStage<byte[]> completion;
        try {
            completion = Objects.requireNonNull(
                handler.handle(inbound.source(), command33.clone(), payload),
                "relocation reply handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        completion.whenComplete((ack, failure) -> {
            if (failure != null || ack == null) {
                Throwable cause = failure == null ? null : unwrap(failure);
                String diagnostic = "relocation-reply-relay-failed source="
                    + inbound.source()
                    + " error=" + (cause == null
                        ? "missing-ack"
                        : cause.getClass().getSimpleName() + ": "
                            + String.valueOf(cause.getMessage()));
                LOGGER.warning(diagnostic);
                streamTrace(STREAM_TRACE ? diagnostic : null);
                return;
            }
            try {
                relocationWire.decodeReplyRelayAck(ack);
                port.send(
                    requireStarted(),
                    inbound.source(),
                    List.of(ack.clone()));
            } catch (RuntimeException invalid) {
                // Invalid ACKs are never published as terminal evidence.
            }
        });
    }

    private void dispatchRelocationReplyRelayAck(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.requestSequence() != null || inbound.frames().size() != 1) {
            return;
        }
        byte[] encoded = inbound.frames().getFirst();
        var relocationWire = new ZLinkServiceRelocationWireCodec();
        ZLinkServiceRelocationWireCodec.ReplyRelayAck ack;
        try {
            ack = relocationWire.decodeReplyRelayAck(encoded);
            //  The ACK is sent by the landing node that owns the relocated
            //  reply capability, which may differ from the request-source
            //  fence inside the ACK. The pending key below pins the ACK to
            //  the exact addressed landing node and validateReplyRelayAck
            //  pins the full expected request-source fence.
            topology.peer(inbound.source()).orElseThrow();
        } catch (RuntimeException invalid) {
            return;
        }
        ReplyRelayPendingKey key = ReplyRelayPendingKey.from(
            inbound.source(), ack);
        ReplyRelayPending pending = pendingReplyRelays.get(key);
        if (pending == null) {
            return;
        }
        try {
            validateReplyRelayAck(pending.relay, pending.expectedSource, ack);
            if (!pendingReplyRelays.remove(key, pending)) {
                return;
            }
            pending.cancelTimeout();
            pending.completion.complete(encoded.clone());
        } catch (RuntimeException invalid) {
            // A forged or stale ACK cannot close or poison a valid pending
            // relay. A later exact ACK may still complete it before timeout.
        }
    }

    static void validateReplyRelayPayload(
        ZLinkServiceRelocationWireCodec.ReplyRelay relay,
        int payloadFrames) {
        if (payloadFrames < 0
            || relay.terminalResult() == 0 && payloadFrames != 1
            || relay.terminalResult() != 0 && payloadFrames != 0) {
            throw new IllegalArgumentException(
                "command 33 terminal payload boundary differs");
        }
    }

    private static void validateReplyRelayAck(
        ZLinkServiceRelocationWireCodec.ReplyRelay relay,
        ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
        ZLinkServiceRelocationWireCodec.ReplyRelayAck ack) {
        if (!ack.relocation().equals(relay.relocation())
            || !ack.coordinator().equals(relay.coordinator())
            || !ack.operation().equals(relay.operation())
            || ack.replyRouteId() != relay.replyRouteId()
            || !ack.requestSource().equals(expectedSource)) {
            throw new IllegalArgumentException(
                "command 46 does not close the pending command 33 fence");
        }
    }

    private void dispatchSessionRelocationRoute(
        ZLinkJavaRawServicePort.Inbound inbound) {
        var handler = sessionRelocationRouteHandler;
        if (handler == null
            || inbound.requestSequence() != null
            || inbound.frames().size() != 1) {
            return;
        }
        byte[] command44 = inbound.frames().getFirst();
        try {
            statefulWire.decodeSessionRelocationRoute(command44);
        } catch (RuntimeException invalid) {
            return;
        }
        CompletionStage<Void> completion;
        try {
            completion = Objects.requireNonNull(
                handler.handle(inbound.source(), command44.clone()),
                "Session relocation route handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        completion.whenComplete((ignored, failure) -> {
            if (failure != null) {
                Throwable cause = unwrap(failure);
                String diagnostic = "session-route-handler-failed source="
                    + inbound.source()
                    + " error=" + cause.getClass().getSimpleName() + ": "
                    + String.valueOf(cause.getMessage());
                LOGGER.warning(diagnostic);
                streamTrace(STREAM_TRACE ? diagnostic : null);
            }
        });
    }

    private void dispatchSessionRelocationSeal(
        ZLinkJavaRawServicePort.Inbound inbound) {
        var handler = sessionRelocationSealHandler;
        if (handler == null) {
            return;
        }
        if (inbound.requestSequence() == null
            || inbound.frames().size() != 1) {
            //  A malformed command 42 (seal request) is a transport-boundary
            //  violation; drop it before the handler but record a bounded
            //  diagnostic (a conforming sender never reaches this branch).
            streamTrace(STREAM_TRACE ? "session-seal-command42-malformed-shape source="
                + inbound.source()
                + " requestSequence=" + inbound.requestSequence()
                + " frames=" + inbound.frames().size() : null);
            return;
        }
        byte[] command42 = inbound.frames().getFirst();
        try {
            statefulWire.decodeSessionRelocationSeal(command42);
        } catch (RuntimeException invalid) {
            streamTrace(STREAM_TRACE ? "session-seal-command42-decode-failed source="
                + inbound.source()
                + " error=" + invalid.getClass().getSimpleName()
                + ": " + String.valueOf(invalid.getMessage()) : null);
            return;
        }
        CompletionStage<byte[]> completion;
        try {
            completion = Objects.requireNonNull(
                handler.handle(inbound.source(), command42.clone()),
                "Session relocation seal handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        completion.whenComplete((ack, failure) -> {
            if (failure != null || ack == null) {
                Throwable cause = failure == null ? null : unwrap(failure);
                String diagnostic = "session-seal-handler-failed source="
                    + inbound.source()
                    + " request=" + inbound.requestSequence()
                    + " error=" + (cause == null
                        ? "missing-ack"
                        : cause.getClass().getSimpleName() + ": "
                            + String.valueOf(cause.getMessage()));
                LOGGER.warning(diagnostic);
                streamTrace(STREAM_TRACE ? diagnostic : null);
                return;
            }
            try {
                statefulWire.decodeSessionRelocationSealed(ack);
                port.reply(
                    requireStarted(),
                    inbound.source(),
                    inbound.requestSequence(),
                    List.of(ack));
            } catch (RuntimeException invalid) {
                // Invalid ACKs never cross the infrastructure boundary.
                streamTrace(STREAM_TRACE ? "session-seal-ack-invalid source="
                    + inbound.source() + " error=" + invalid.getMessage() : null);
            }
        });
    }

    private boolean dispatchSpot(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1) {
            return false;
        }
        ZLinkServiceM6BWireCodec.SpotMessage header;
        try {
            header = statefulWire.decodeSpotHeader(frames.getFirst());
        } catch (RuntimeException invalid) {
            return false;
        }
        streamTrace(STREAM_TRACE ? "spot-received request=" + header.request()
            + " source=" + inbound.source()
            + " target=" + header.target().targetNodeRid()
            + " spot=" + header.target().spotId()
            + " generation=" + header.target().spotGeneration() : null);
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
        } catch (RuntimeException invalidPayload) {
            replySpotFailure(inbound, header, 104, 12);
            return false;
        }
        if (header.request() != (inbound.requestSequence() != null)
            || !header.target().targetNodeRid().equals(routingId)
            || localDescriptor == null
            || header.target().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replySpotFailure(inbound, header, 102, 1);
            return false;
        }
        byte[] metadata = payloadOffset == 2
            ? frames.get(1).clone()
            : new byte[0];
        resolveAcceptedAuthorities(inbound).whenComplete((authorities, failure) -> {
            streamTrace(STREAM_TRACE ? "spot-authorities request=" + header.request()
                + " target=" + header.target().targetNodeRid()
                + " spot=" + header.target().spotId()
                + " present=" + (authorities != null && authorities.isPresent())
                + " failure=" + (failure == null
                    ? "none"
                    : failure.getClass().getSimpleName()) : null);
            if (failure != null || authorities.isEmpty()) {
                replySpotFailure(inbound, header, 107, 33);
                inbound.close();
                return;
            }
            AcceptedAuthorities acceptedAuthorities =
                authorities.orElseThrow();
            List<Message> messages = null;
            String contentType;
            try {
                messages = decodeApplicationMessages(payload);
                contentType = applicationContentType(messages);
            } catch (RuntimeException invalidPayload) {
                if (messages != null) {
                    messages.forEach(Message::close);
                }
                replySpotFailure(inbound, header, 104, 12);
                inbound.close();
                return;
            }
            ZLinkTerminalWinner terminal =
                new ZLinkTerminalWinner();
            int acceptedRecordSizeHint = (int) Math.min(
                Integer.MAX_VALUE,
                applicationPayloadBytes(messages) + metadata.length + 512L);
            boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
                .enqueueRemoteSpotLazy(
                    acceptedAuthorities.source(),
                    header,
                    metadata,
                    () -> ZLinkServiceFrozenRecordCodec.encodeSpot(
                        acceptedAuthorities.source(),
                        acceptedAuthorities.targetOwner(),
                        header,
                        metadata,
                        wire.encodeApplicationPayload(payload)),
                    acceptedRecordSizeHint,
                    messages,
                    contentType,
                    replyParts -> {
                        if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                                .completion.ZLinkTerminalWinner.Cause.RESPONSE)) {
                            replyParts.forEach(Message::close);
                            return;
                        }
                        try {
                            port.reply(
                                requireStarted(),
                                inbound.source(),
                                inbound.requestSequence(),
                                List.of(
                                    wire.encodeReplyHeader(
                                        header.correlation(), 0, 0),
                                    wire.encodeApplicationPayload(
                                        applicationPayload(replyParts))));
                        } finally {
                            replyParts.forEach(Message::close);
                        }
                    },
                    relayFailure -> {
                        if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                                .completion.ZLinkTerminalWinner.Cause.FAILURE)) {
                            return;
                        }
                        //  Relay the real received/classified pair instead of a
                        //  fixed 102+1 (spec 32-framework-error-model:83-92).
                        int[] pair = relayedFailurePair(relayFailure);
                        replySpotFailure(inbound, header, pair[0], pair[1]);
                    },
                    inbound::close);
            if (!accepted) {
                streamTrace(STREAM_TRACE ? "spot-enqueue-rejected target="
                    + header.target().targetNodeRid()
                    + " spot=" + header.target().spotId() : null);
                messages.forEach(Message::close);
                replySpotFailure(inbound, header, 102, 1);
                inbound.close();
            }
        });
        return true;
    }

    private void replySpotFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        int terminalResult,
        int failureCode) {
        if (!header.request() || inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(
                header.correlation(),
                terminalResult,
                failureCode)));
    }

    private void dispatchLogicalMulticast(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1
            || inbound.requestSequence() != null) {
            return;
        }
        try {
            ZLinkServiceM6BWireCodec.LogicalMulticast header =
                statefulWire.decodeLogicalMulticastHeader(frames.getFirst());
            boolean admitted = admittedPeerChannels
                .getOrDefault(inbound.source(), Map.of())
                .containsKey(header.channelName());
            streamTrace(STREAM_TRACE ? "logical-multicast-receive channel="
                + header.channelName()
                + " topic=" + header.topic()
                + " source=" + inbound.source()
                + " admitted=" + admitted : null);
            if (!admitted) {
                return;
            }
            byte[] metadata = payloadOffset == 2
                ? frames.get(1).clone()
                : new byte[0];
            ZLinkServiceM6AWireCodec.ApplicationPayload payload =
                wire.decodeApplicationPayload(frames.get(payloadOffset));
            List<Message> messages = decodeApplicationMessages(payload);
            try {
                ((ZLinkJavaRawSpotNode) spotNode()).enqueueLogicalMulticast(
                    header.channelName(),
                    header.topic(),
                    header.sourceSpotId(),
                    inbound.source(),
                    metadata,
                    applicationContentType(messages),
                    messages);
            } finally {
                messages.forEach(Message::close);
            }
        } catch (RuntimeException invalid) {
            return;
        }
    }

    private void dispatchInstanceSpot(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1) {
            return;
        }
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header;
        try {
            header =
                statefulWire.decodeInstanceSpotHeader(frames.getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
        } catch (RuntimeException invalidPayload) {
            replyInstanceFailure(inbound, header, 104, 12);
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        boolean validFence = header.request() == (inbound.requestSequence() != null)
            && header.sourceNodeRid().equals(inbound.source())
            && source.isPresent()
            && source.orElseThrow().descriptor().lifecycleGeneration()
                == header.sourceNodeGeneration()
            && header.route().targetNodeRid().equals(routingId)
            && localDescriptor != null
            && header.route().targetNodeGeneration()
                == localDescriptor.lifecycleGeneration();
        if (!validFence) {
            streamTrace(STREAM_TRACE ? "instance dispatch rejected source=" + inbound.source()
                + " sourceGeneration=" + header.sourceNodeGeneration()
                + " sourcePeerGeneration=" + (source.isPresent()
                    ? source.orElseThrow().descriptor().lifecycleGeneration() : -1)
                + " targetNode=" + header.route().targetNodeRid()
                + " localNode=" + routingId
                + " targetGeneration=" + header.route().targetNodeGeneration()
                + " localGeneration=" + (localDescriptor == null
                    ? -1 : localDescriptor.lifecycleGeneration())
                + " request=" + header.request()
                + " inboundRequest=" + (inbound.requestSequence() != null) : null);
            replyInstanceFailure(inbound, header, 102, 1);
            return;
        }
        byte[] metadata = payloadOffset == 2
            ? frames.get(1).clone()
            : new byte[0];
        List<Message> messages;
        try {
            messages = decodeApplicationMessages(payload);
        } catch (RuntimeException invalidPayload) {
            replyInstanceFailure(inbound, header, 104, 12);
            return;
        }
        String contentType;
        try {
            contentType = applicationContentType(messages);
        } catch (RuntimeException budgetFailure) {
            messages.forEach(Message::close);
            replyInstanceFailure(inbound, header, 104, 12);
            return;
        }
        ZLinkTerminalWinner terminal =
            new ZLinkTerminalWinner();
        boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
            .enqueueRemoteInstanceSpot(
                inbound.source(),
                header,
                metadata,
                messages,
                contentType,
                replyParts -> {
                    if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                            .completion.ZLinkTerminalWinner.Cause.RESPONSE)) {
                        replyParts.forEach(Message::close);
                        return;
                    }
                    try {
                        port.reply(
                            requireStarted(),
                            inbound.source(),
                            inbound.requestSequence(),
                            List.of(
                                wire.encodeReplyHeader(
                                    header.replyRouteId(), 0, 0),
                                wire.encodeApplicationPayload(
                                    applicationPayload(replyParts))));
                    } finally {
                        replyParts.forEach(Message::close);
                    }
                },
                failure -> {
                    if (terminal.tryWin(systems.zlink.framework.runtime.internal
                            .completion.ZLinkTerminalWinner.Cause.FAILURE)) {
                        replyInstanceFailure(inbound, header, 102, 1);
                    }
                });
        if (!accepted) {
            streamTrace(STREAM_TRACE ? "instance dispatch rejected by mailbox source="
                + inbound.source() + " target=" + header.route().targetSpotId()
                + " request=" + header.request() : null);
            messages.forEach(Message::close);
            if (terminal.tryWin(systems.zlink.framework.runtime.internal
                    .completion.ZLinkTerminalWinner.Cause.FAILURE)) {
                replyInstanceFailure(inbound, header, 102, 1);
            }
        }
    }

    private void dispatchUserSpotCreate(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.frames().size() != 1
            || inbound.requestSequence() == null) {
            return;
        }
        ZLinkServiceM6BWireCodec.UserSpotCreate command;
        try {
            command = statefulWire.decodeUserSpotCreateHeader(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        ZLinkInternalMeshNode.UserSpotOperationHandler handler =
            userSpotOperationHandler;
        if (source.isEmpty()
            || handler == null
            || !command.sourceNodeRid().equals(inbound.source())
            || command.sourceNodeGeneration()
                != source.orElseThrow().descriptor()
                    .lifecycleGeneration()
            || localDescriptor == null
            || !command.reservation().targetNodeRid()
                .equals(routingId)
            || command.reservation().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyUserSpotCreateFailure(inbound, command, 107, 33);
            return;
        }
        UserSpotOperationKey operationKey = new UserSpotOperationKey(
            command.sourceNodeRid(),
            command.sourceNodeGeneration(),
            command.operationHigh(),
            command.operationLow());
        UserSpotTerminalAdmission admission = admitUserSpotOperation(
            operationKey,
            fingerprint(statefulWire.encodeUserSpotCreateHeader(
                new ZLinkServiceM6BWireCodec.UserSpotCreate(
                    1,
                    command.operationHigh(),
                    command.operationLow(),
                    command.sourceNodeRid(),
                    command.sourceNodeGeneration(),
                    command.spotId(),
                    command.stableType(),
                    command.reservation(),
                    command.deadlineUnixMs()))),
            command.deadlineUnixMs());
        if (admission.fingerprintMismatch()) {
            replyUserSpotCreateFailure(inbound, command, 110, 0);
            return;
        }
        if (admission.expiredNew()) {
            replyUserSpotCreateFailure(inbound, command, 101, 0);
            return;
        }
        if (admission.slot() == null) {
            replyUserSpotCreateFailure(inbound, command, 108, 0);
            return;
        }
        if (!admission.owner()) {
            admission.slot().terminal.whenComplete(
                (reply, failure) -> sendUserSpotCreateTerminal(
                    inbound, command, reply));
            return;
        }
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        CompletionStage<ZLinkInternalMeshNode.UserSpotCreateResponse>
            completion;
        try {
            completion = handler.create(
                new ZLinkInternalMeshNode.UserSpotCreateRequest(
                    command.sourceNodeRid(),
                    command.sourceNodeGeneration(),
                    command.operationHigh(),
                    command.operationLow(),
                    new ZLinkInternalMeshNode.UserSpotCreateIntent(
                        command.spotId(),
                        command.stableType(),
                        command.reservation(),
                        command.deadlineUnixMs())));
        } catch (RuntimeException failure) {
            completeUserSpotCreateTerminal(
                admission.slot(), inbound, command, null, failure);
            return;
        }
        completion.whenComplete((result, failure) -> {
            if (!terminal.tryWin(completionTerminalCause(failure))) {
                closeUserSpotApplicationReply(result);
                return;
            }
            if (failure != null || result == null
                || !result.spotId().equals(command.spotId())
                || result.objectGeneration()
                    != command.reservation().objectGeneration()
                || (result.result()
                        == ZLinkServiceM6BWireCodec
                            .UserSpotCreateResult.EXISTING
                    && !result.applicationReply().isEmpty())) {
                completeUserSpotCreateTerminal(
                    admission.slot(), inbound, command, result, failure);
                return;
            }
            try {
                byte[] applicationFrame = null;
                if (!result.applicationReply().isEmpty()) {
                    applicationFrame = wire.encodeApplicationPayload(
                        applicationPayload(
                            result.applicationReply()));
                }
                UserSpotTerminalReply reply = new UserSpotTerminalReply(
                    0,
                    0,
                    new ZLinkServiceM6BWireCodec.UserSpotCreateTerminal(
                        result.result(),
                        result.spotId(),
                        result.objectGeneration()),
                    null,
                    applicationFrame);
                admission.slot().terminal.complete(reply);
                sendUserSpotCreateTerminal(inbound, command, reply);
            } finally {
                closeUserSpotApplicationReply(result);
            }
        });
    }

    private void dispatchActorCreate(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.frames().size() != 1
            || inbound.requestSequence() == null) {
            return;
        }
        ZLinkServiceM6BWireCodec.ActorCreate command;
        try {
            command = statefulWire.decodeActorCreateHeader(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        var handler = actorCreateOperationHandler;
        if (source.isEmpty()
            || handler == null
            || !command.sourceNodeRid().equals(inbound.source())
            || command.sourceNodeGeneration()
                != source.orElseThrow().descriptor()
                    .lifecycleGeneration()
            || localDescriptor == null
            || !command.reservation().targetNodeRid().equals(routingId)
            || command.reservation().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyActorCreateFailure(inbound, command, 107, 21);
            return;
        }
        CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
            completion;
        try {
            completion = handler.create(
                new ZLinkInternalMeshNode.ActorCreateRequest(
                    command.sourceNodeRid(),
                    command.sourceNodeGeneration(),
                    command.operationHigh(),
                    command.operationLow(),
                    new ZLinkInternalMeshNode.ActorCreateIntent(
                        command.actorId(),
                        command.stableType(),
                        command.reservation(),
                        command.operationHigh(),
                        command.operationLow(),
                        command.deadlineUnixMs())));
        } catch (RuntimeException failure) {
            replyActorCreateFailure(inbound, command, 105, 2);
            return;
        }
        completion.whenComplete((response, failure) -> {
            if (failure != null || response == null) {
                replyActorCreateFailure(inbound, command, 105, 2);
                return;
            }
            sendActorCreateTerminal(
                inbound,
                command,
                response.terminalEnvelope());
        });
    }

    private void dispatchUserSpotClose(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.frames().size() != 1
            || inbound.requestSequence() == null) {
            return;
        }
        ZLinkServiceM6BWireCodec.UserSpotClose command;
        try {
            command = statefulWire.decodeUserSpotCloseHeader(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        ZLinkInternalMeshNode.UserSpotOperationHandler handler =
            userSpotOperationHandler;
        if (source.isEmpty()
            || handler == null
            || !command.sourceNodeRid().equals(inbound.source())
            || command.sourceNodeGeneration()
                != source.orElseThrow().descriptor()
                    .lifecycleGeneration()
            || localDescriptor == null
            || !command.target().targetNodeRid().equals(routingId)
            || command.target().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyUserSpotCloseFailure(inbound, command, 107, 33);
            return;
        }
        UserSpotOperationKey operationKey = new UserSpotOperationKey(
            command.sourceNodeRid(),
            command.sourceNodeGeneration(),
            command.operationHigh(),
            command.operationLow());
        UserSpotTerminalAdmission admission = admitUserSpotOperation(
            operationKey,
            fingerprint(statefulWire.encodeUserSpotCloseHeader(
                new ZLinkServiceM6BWireCodec.UserSpotClose(
                    1,
                    command.operationHigh(),
                    command.operationLow(),
                    command.sourceNodeRid(),
                    command.sourceNodeGeneration(),
                    command.target(),
                    command.deadlineUnixMs()))),
            command.deadlineUnixMs());
        if (admission.fingerprintMismatch()) {
            replyUserSpotCloseFailure(inbound, command, 110, 0);
            return;
        }
        if (admission.expiredNew()) {
            replyUserSpotCloseFailure(inbound, command, 101, 0);
            return;
        }
        if (admission.slot() == null) {
            replyUserSpotCloseFailure(inbound, command, 108, 0);
            return;
        }
        if (!admission.owner()) {
            admission.slot().terminal.whenComplete(
                (reply, failure) -> sendUserSpotCloseTerminal(
                    inbound, command, reply));
            return;
        }
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse>
            completion;
        try {
            completion = handler.close(
                new ZLinkInternalMeshNode.UserSpotCloseRequest(
                    command.sourceNodeRid(),
                    command.sourceNodeGeneration(),
                    command.operationHigh(),
                    command.operationLow(),
                    new ZLinkInternalMeshNode.UserSpotCloseIntent(
                        command.target(),
                        command.deadlineUnixMs())));
        } catch (RuntimeException failure) {
            UserSpotTerminalReply reply = terminalFailure(failure);
            admission.slot().terminal.complete(reply);
            sendUserSpotCloseTerminal(inbound, command, reply);
            return;
        }
        completion.whenComplete((result, failure) -> {
            if (!terminal.tryWin(completionTerminalCause(failure))) {
                return;
            }
            if (failure != null || result == null) {
                UserSpotTerminalReply reply = terminalFailure(failure);
                admission.slot().terminal.complete(reply);
                sendUserSpotCloseTerminal(inbound, command, reply);
                return;
            }
            UserSpotTerminalReply reply = new UserSpotTerminalReply(
                0, 0, null, result.closed(), null);
            admission.slot().terminal.complete(reply);
            sendUserSpotCloseTerminal(inbound, command, reply);
        });
    }

    private void replyUserSpotCreateFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.UserSpotCreate command,
        int terminalResult,
        int failureCode) {
        if (inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(statefulWire.encodeUserSpotCreateReply(
                command.correlation(),
                terminalResult,
                failureCode,
                null)));
    }

    private void replyActorCreateFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.ActorCreate command,
        int terminalResult,
        int failureCode) {
        if (inbound.requestSequence() == null) {
            return;
        }
        var terminal =
            new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                terminalResult,
                failureCode,
                null,
                null);
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(statefulWire.encodeActorCreateReply(
                command.correlation(), terminal)));
    }

    private void sendActorCreateTerminal(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.ActorCreate command,
        byte[] terminalEnvelope) {
        if (inbound.requestSequence() == null) {
            return;
        }
        var terminal =
            statefulWire.decodeCreationOperationTerminal(
                terminalEnvelope);
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeActorCreateReply(
            command.correlation(), terminal));
        if (terminal.applicationPayloadFrame() != null) {
            frames.add(terminal.applicationPayloadFrame());
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            frames);
    }

    private void completeUserSpotCreateTerminal(
        UserSpotTerminalSlot slot,
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.UserSpotCreate command,
        ZLinkInternalMeshNode.UserSpotCreateResponse result,
        Throwable failure) {
        closeUserSpotApplicationReply(result);
        UserSpotTerminalReply reply = terminalFailure(failure);
        slot.terminal.complete(reply);
        sendUserSpotCreateTerminal(inbound, command, reply);
    }

    private static UserSpotTerminalReply terminalFailure(
        Throwable failure) {
        Throwable current = failure == null
            ? null : unwrap(failure);
        if (current instanceof ZLinkUserSpotOperationException typed) {
            return new UserSpotTerminalReply(
                typed.terminalResult(),
                typed.failureCode(),
                null,
                null,
                null);
        }
        return new UserSpotTerminalReply(
            105, 17, null, null, null);
    }

    private void sendUserSpotCreateTerminal(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.UserSpotCreate command,
        UserSpotTerminalReply reply) {
        if (reply == null || inbound.requestSequence() == null) {
            return;
        }
        List<byte[]> frames = new ArrayList<>();
        frames.add(statefulWire.encodeUserSpotCreateReply(
            command.correlation(),
            reply.terminalResult(),
            reply.failureCode(),
            reply.create()));
        byte[] applicationFrame = reply.applicationFrame();
        if (applicationFrame != null) {
            frames.add(applicationFrame);
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            frames);
    }

    private void sendUserSpotCloseTerminal(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.UserSpotClose command,
        UserSpotTerminalReply reply) {
        if (reply == null || inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(statefulWire.encodeUserSpotCloseReply(
                command.correlation(),
                reply.terminalResult(),
                reply.failureCode(),
                reply.closed())));
    }

    private void replyUserSpotCloseFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.UserSpotClose command,
        int terminalResult,
        int failureCode) {
        if (inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(statefulWire.encodeUserSpotCloseReply(
                command.correlation(),
                terminalResult,
                failureCode,
                null)));
    }

    private static void closeUserSpotApplicationReply(
        ZLinkInternalMeshNode.UserSpotCreateResponse response) {
        if (response != null) {
            response.applicationReply().forEach(Message::close);
        }
    }

    private void replyInstanceFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
        int terminalResult,
        int failureCode) {
        if (!header.request() || inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(
                header.replyRouteId(),
                terminalResult,
                failureCode)));
    }

    private void dispatchCanonicalActorJoin(ZLinkJavaRawServicePort.Inbound inbound) {
        List<byte[]> frames = inbound.frames();
        if (inbound.requestSequence() == null || frames.isEmpty()) {
            return;
        }
        if (frames.size() > 2) {
            replyCanonicalActorJoinProtocolFailure(inbound, frames);
            return;
        }
        final ServiceWirePilotCodec.ActorJoin28 join;
        try {
            join = ServiceWirePilotCodec.decodeActorJoin28(frames);
        } catch (Exception invalidCanonical) {
            replyCanonicalActorJoinProtocolFailure(inbound, frames);
            return;
        }
        RoutingId targetNode = RoutingId.from(
            join.targetSpot().targetNodeRid());
        if (localDescriptor == null || !targetNode.equals(routingId)
            || join.targetSpot().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyCanonicalActorJoinFailure(inbound, join.correlation(), 102, 14);
            return;
        }
        ZLinkInternalMeshNode.CanonicalActorJoinHandler handler =
            canonicalActorJoinHandler;
        if (handler == null) {
            replyCanonicalActorJoinFailure(inbound, join.correlation(), 105, 17);
            return;
        }
        try {
            handler.admit(inbound.source(), join).whenComplete((response, failure) -> {
                if (failure != null || response == null) {
                    int[] terminal = canonicalActorJoinFailurePair(unwrap(failure));
                    replyCanonicalActorJoinFailure(
                        inbound, join.correlation(), terminal[0], terminal[1]);
                    return;
                }
                List<Message> applicationReply = response.applicationReply();
                try {
                    ZLinkCanonicalActorJoinReplyCodec.Tail tail =
                        response.accepted()
                            ? new ZLinkCanonicalActorJoinReplyCodec.Tail(
                                ZLinkCanonicalActorJoinReplyCodec.JOIN_RESULT_ACCEPTED,
                                new ZLinkCanonicalActorJoinReplyCodec.SpotRef(
                                    join.targetSpot().id(),
                                    join.targetSpot().generation()),
                                response.membershipEpoch(),
                                32_768L)
                            : new ZLinkCanonicalActorJoinReplyCodec.Tail(
                                ZLinkCanonicalActorJoinReplyCodec.JOIN_RESULT_REJECTED,
                                null, null, null);
                    List<byte[]> reply = new ArrayList<>();
                    reply.add(new ZLinkCanonicalActorJoinReplyCodec().encode(
                        new ZLinkCanonicalActorJoinReplyCodec.ActorJoinReply(
                            join.correlation(), 0, 0, tail.joinResult(),
                            tail.spot(), tail.membershipEpoch(),
                            tail.receiveChunkLimitBytes())));
                    if (!applicationReply.isEmpty()) {
                        reply.add(wire.encodeApplicationPayload(
                            applicationPayload(applicationReply)));
                    }
                    port.reply(requireStarted(), inbound.source(),
                        inbound.requestSequence(), reply);
                } catch (RuntimeException invalidReply) {
                    replyCanonicalActorJoinFailure(
                        inbound, join.correlation(), 105, 2);
                } finally {
                    applicationReply.forEach(Message::close);
                }
            });
        } catch (RuntimeException failure) {
            int[] terminal = canonicalActorJoinFailurePair(failure);
            replyCanonicalActorJoinFailure(
                inbound, join.correlation(), terminal[0], terminal[1]);
        }
    }

    private void replyCanonicalActorJoinProtocolFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        List<byte[]> frames) {
        if (inbound.requestSequence() == null || frames.isEmpty()
            || frames.getFirst().length < PREFIX_BYTES + Long.BYTES) {
            return;
        }
        long correlation = ByteBuffer.wrap(frames.getFirst())
            .getLong(PREFIX_BYTES);
        if (correlation != 0) {
            replyCanonicalActorJoinFailure(inbound, correlation, 104, 16);
        }
    }

    /**
     * The Store admission owner reports framework kinds, which must retain
     * their command-20 terminal rather than being collapsed into a generic
     * transport failure.  These pairs are the schema's exact typed-terminal
     * mappings used by the other service-wire receivers.
     */
    static int[] canonicalActorJoinFailurePair(Throwable failure) {
        if (failure instanceof ZLinkFrameworkException framework) {
            if (isSupersededCanonicalActorJoin(framework)) {
                return new int[] {107, 21};
            }
            return switch (framework.kind()) {
                case NOT_FOUND -> new int[] {102, 14};
                case PROTOCOL_ERROR -> new int[] {104, 16};
                case TYPE_MISMATCH -> new int[] {107, 4};
                case REJECTED -> new int[] {106, 15};
                default -> new int[] {105, 17};
            };
        }
        return relayedFailurePair(failure);
    }

    private static boolean isSupersededCanonicalActorJoin(
        ZLinkFrameworkException failure) {
        return "true".equals(failure.metadata().get(
            "zlink.actorJoin.superseded"));
    }

    private void replyCanonicalActorJoinFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        long correlation,
        int terminalResult,
        int failureCode) {
        if (inbound.requestSequence() == null
            || !ServiceWireConstants.validTerminalFailure(
                terminalResult, failureCode)) {
            return;
        }
        port.reply(requireStarted(), inbound.source(), inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(
                correlation, terminalResult, failureCode)));
    }

    private boolean dispatchActor(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1) {
            return false;
        }
        ZLinkServiceM6BWireCodec.ActorMessage header;
        try {
            header = statefulWire.decodeActorHeader(frames.getFirst());
        } catch (RuntimeException invalid) {
            return false;
        }
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
        } catch (RuntimeException invalid) {
            replyActorFailure(inbound, header, 104, 12);
            return false;
        }
        if (header.request() != (inbound.requestSequence() != null)
            || !header.target().actor().nodeRid().equals(routingId)
            || localDescriptor == null
            || header.target().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyActorFailure(inbound, header, 102, 1);
            return false;
        }
        List<Message> messages = null;
        String contentType;
        ZLinkStreamCodec streamCodec;
        try {
            messages = decodeApplicationMessages(payload);
            contentType = applicationContentType(messages);
            streamCodec = Objects.requireNonNull(
                    applicationStreamCodecResolver.apply(contentType),
                    "application stream codec resolver returned null")
                .orElseThrow(() -> new IllegalArgumentException(
                    "no stream codec is registered for content type: "
                        + contentType));
        } catch (RuntimeException unknownContentType) {
            if (messages != null) {
                messages.forEach(Message::close);
            }
            replyActorFailure(inbound, header, 104, 12);
            return false;
        }
        final List<Message> receivedMessages = messages;
        resolveAcceptedAuthorities(inbound).whenComplete((authorities, failure) -> {
            if (failure != null || authorities.isEmpty()) {
                receivedMessages.forEach(Message::close);
                replyActorFailure(inbound, header, 107, 21);
                inbound.close();
                return;
            }
            AcceptedAuthorities acceptedAuthorities =
                authorities.orElseThrow();
            ZLinkTerminalWinner terminal =
                new ZLinkTerminalWinner();
            boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
                .enqueueRemoteActor(
                    acceptedAuthorities.source(),
                    header,
                    () -> ZLinkServiceFrozenRecordCodec.encodeActor(
                        acceptedAuthorities.source(),
                        acceptedAuthorities.targetOwner(),
                        header,
                        new byte[0],
                        wire.encodeApplicationPayload(payload)),
                    receivedMessages,
                    contentType,
                    replyParts -> {
                        if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                                .completion.ZLinkTerminalWinner.Cause.RESPONSE)) {
                            replyParts.forEach(Message::close);
                            return;
                        }
                        try {
                            port.reply(
                                requireStarted(),
                                inbound.source(),
                                inbound.requestSequence(),
                                List.of(
                                    wire.encodeReplyHeader(
                                        header.correlation(), 0, 0),
                                    wire.encodeApplicationPayload(
                                        applicationPayload(replyParts))));
                        } finally {
                            replyParts.forEach(Message::close);
                        }
                    },
                    relayFailure -> {
                        if (!terminal.tryWin(systems.zlink.framework.runtime.internal
                                .completion.ZLinkTerminalWinner.Cause.FAILURE)) {
                            return;
                        }
                        //  Relay the real received/classified pair instead of a
                        //  fixed 102+1 (spec 32-framework-error-model:83-92).
                        int[] pair = relayedFailurePair(relayFailure);
                        replyActorFailure(inbound, header, pair[0], pair[1]);
                    },
                    inbound::close);
            if (!accepted) {
                receivedMessages.forEach(Message::close);
                replyActorFailure(inbound, header, 102, 1);
                inbound.close();
            }
        });
        return true;
    }

    private CompletionStage<Optional<
        ZLinkInternalMeshNode.PeerAuthorityFence>> resolveSourceAuthority(
            ZLinkJavaRawServicePort.Inbound inbound) {
        ZLinkInternalMeshNode.PeerAuthorityResolver resolver =
            peerAuthorityResolver;
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        if (resolver == null || peer.isEmpty()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return resolver.resolve(
            meshName,
            inbound.source(),
            peer.orElseThrow().descriptor().lifecycleGeneration());
    }

    private CompletionStage<Optional<AcceptedAuthorities>>
        resolveAcceptedAuthorities(
            ZLinkJavaRawServicePort.Inbound inbound) {
        CompletionStage<Optional<
            ZLinkInternalMeshNode.PeerAuthorityFence>> source =
                resolveSourceAuthority(inbound);
        ZLinkInternalMeshNode.PeerAuthorityResolver resolver =
            peerAuthorityResolver;
        ZLinkServiceNodeDescriptor descriptor = localDescriptor;
        if (resolver == null || descriptor == null || routingId == null) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        return source.thenCombine(
            resolver.resolve(
                meshName,
                routingId,
                descriptor.lifecycleGeneration()),
            (resolvedSource, targetOwner) ->
                resolvedSource.isPresent() && targetOwner.isPresent()
                    ? Optional.of(new AcceptedAuthorities(
                        resolvedSource.orElseThrow(),
                        targetOwner.orElseThrow()))
                    : Optional.empty());
    }

    private record AcceptedAuthorities(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkInternalMeshNode.PeerAuthorityFence targetOwner) {
    }

    private void dispatchBoundSessionBind(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.frames().size() != 1
            || inbound.requestSequence() == null) {
            return;
        }
        ZLinkServiceM6BWireCodec.BoundSessionBind binding;
        try {
            binding = statefulWire.decodeBoundSessionBindHeader(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        if (source.isEmpty()) {
            port.reply(
                requireStarted(),
                inbound.source(),
                inbound.requestSequence(),
                List.of(wire.encodeReplyHeader(
                    binding.correlation(), 102, 1)));
            return;
        }
        long sourceGeneration = source.orElseThrow().descriptor()
            .lifecycleGeneration();
        resolveSourceAuthority(inbound).handle((authority, failure) -> {
            String ownerId = authority != null && authority.isPresent()
                ? authority.orElseThrow().ownerId()
                : inbound.source().toString();
            long ownerLease = authority != null && authority.isPresent()
                ? authority.orElseThrow().ownerLeaseGeneration()
                : 1L;
            boolean accepted = failure == null
                && ((ZLinkJavaRawSpotNode) spotNode())
                    .acceptRemoteStreamBinding(
                        inbound.source(),
                        sourceGeneration,
                        ownerId,
                        ownerLease,
                        binding);
            port.reply(
                requireStarted(),
                inbound.source(),
                inbound.requestSequence(),
                List.of(wire.encodeReplyHeader(
                    binding.correlation(),
                    accepted ? 0 : 102,
                    accepted ? 0 : 1)));
            return null;
        });
    }

    private void dispatchBoundSessionReplaced(
        ZLinkJavaRawServicePort.Inbound inbound) {
        if (inbound.frames().size() != 1
            || inbound.requestSequence() != null) {
            return;
        }
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement;
        try {
            replacement = statefulWire.decodeBoundSessionReplaced(
                inbound.frames().getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        if (!replacement.actorAuthority().actor().nodeRid()
                .equals(inbound.source())) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source = topology == null
            ? Optional.empty()
            : topology.peer(inbound.source());
        if (source.isEmpty()
            || replacement.actorAuthority().targetNodeGeneration()
                != source.orElseThrow().descriptor().lifecycleGeneration()) {
            return;
        }
        var handler = boundSessionReplacedHandler;
        if (handler == null) {
            return;
        }
        try {
            handler.handle(inbound.source(), replacement);
        } catch (RuntimeException ignored) {
            // One-way infrastructure records have no reply path. The sender
            // owns bounded admission retry; handler failure is terminal here.
        }
    }

    private void dispatchBoundSessionSend(
        ZLinkJavaRawServicePort.Inbound inbound) {
        List<byte[]> frames = inbound.frames();
        if (frames.size() != 2
            || inbound.requestSequence() != null) {
            return;
        }
        Optional<ZLinkServiceTopologyRegistry.Peer> source =
            topology == null
                ? Optional.empty()
                : topology.peer(inbound.source());
        if (source.isEmpty()) {
            return;
        }
        try {
            ZLinkServiceM6BWireCodec.BoundSessionSend send =
                statefulWire.decodeBoundSessionSendHeader(
                    frames.getFirst());
            ZLinkServiceM6AWireCodec.ApplicationPayload payload =
                wire.decodeApplicationPayload(frames.get(1));
            var handler = boundSessionSendHandler;
            if (handler == null) {
                return;
            }
            boolean accepted = handler.handle(
                inbound.source(),
                source.orElseThrow().descriptor()
                    .lifecycleGeneration(),
                send,
                payload);
            streamTrace(STREAM_TRACE ? "bound session receive "
                + (accepted ? "accepted" : "rejected")
                + " actor=" + actorSummary(send.actor().actor())
                + " source=" + inbound.source()
                + " binding=" + send.expectedBindingGeneration() : null);
        } catch (RuntimeException invalid) {
            // A malformed or stale one-way record has no terminal route.
        }
    }

    private void replyActorFailure(
        ZLinkJavaRawServicePort.Inbound inbound,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        int terminalResult,
        int failureCode) {
        if (!header.request() || inbound.requestSequence() == null) {
            return;
        }
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(
                header.correlation(),
                terminalResult,
                failureCode)));
    }

    private void dispatchAdmission(
        ZLinkJavaRawServicePort.Inbound inbound,
        int command) {
        if (inbound.frames().size() != 1) {
            return;
        }
        try {
            // A null inbound pair means this lane cannot distinguish
            // physical candidates; admission must still proceed with the
            // legacy RID-addressed surface (spec 07 duplicate confirmation
            // is a no-op when only one physical connection can exist).
            TransportPair inboundPair = transportPair(inbound);
            streamTrace(STREAM_TRACE ? "admission-stage stage=entered command=" + command
                + " source=" + inbound.source()
                + " inboundPair=" + transportPairSummary(inboundPair) : null);
            ZLinkServiceNodeDescriptor descriptor =
                wire.decodeAdmission(
                    inbound.frames().getFirst(),
                    command,
                    inbound.source());
            PeerIntent expected = findExpectedPeer(
                inbound.source(),
                descriptor.advertisedEndpoint());
            rememberPeerIntentRoutingId(expected, inbound.source());
            PeerAdmissionExpectation observed =
                peerAdmissionExpectations.get(inbound.source());
            String expectedEndpoint = observed == null
                ? expected == null ? null : expected.endpoint()
                : observed.endpoint();
            // No configured PeerIntent means this is an unconfigured inbound
            // admission (a listen-only server accepting any client) -- fall
            // back to "no constraint" (null), symmetric with expectedEndpoint
            // and expectedLifecycleGeneration just above. The previous
            // fallback compared the raw ZMTP transport identity
            // (inbound.source()) against the peer's advertised
            // securityIdentity, a field that is not the peer's routing id but
            // a plaintext-transport placeholder ("default" -- see the .NET
            // ZLinkServiceSecurityIdentity.Plaintext / ZLinkManagedMeshNode's
            // matching unconfigured-inbound fallback). Those two values only
            // ever coincided for Java-to-Java peers, where both ends happen
            // to set their ZMTP identity to their own routing id string; a
            // C++ (or any non-Java) peer connecting to an unconfigured Java
            // RouteMesh listener therefore always failed this check and was
            // immediately disconnected on every single admission attempt.
            String expectedSecurityIdentity =
                observed == null
                ? expected == null
                    ? null
                    : expected.expectedSecurityIdentity()
                : observed.securityIdentity();
            long expectedLifecycleGeneration =
                observed == null
                ? expected == null
                    ? 0
                    : expected.expectedLifecycleGeneration()
                : observed.lifecycleGeneration();
            if (!ZLinkServiceAdmissionGuard.matchesExpectedRoute(
                    expectedEndpoint,
                    expectedSecurityIdentity,
                    expectedLifecycleGeneration,
                    descriptor)) {
                streamTrace(STREAM_TRACE
                    ? "admission-rejected reason=expected-route-mismatch"
                        + " source=" + inbound.source()
                        + " fields=" + expectedRouteMismatchFields(
                            expectedEndpoint,
                            expectedSecurityIdentity,
                            expectedLifecycleGeneration,
                            descriptor)
                    : null);
                rejectedPeers.add(inbound.source());
                if (inboundPair == null) {
                    trySendAdmissionControl(
                        inbound.source(),
                        List.of(wire.encodeReject(3)),
                        "expected-route-mismatch");
                } else {
                    // A descriptor fence failure terminates this physical
                    // candidate. Queueing an exact-pair REJECT leaves a send
                    // racing the caller-visible ERROR cleanup; if teardown
                    // wins, that failed send can contaminate the following
                    // reconnect's ROUTER envelope. Closing the identified
                    // pair is the terminal admission result and cannot be
                    // laundered onto a replacement pair.
                    disconnectTransportPair(inboundPair);
                }
                return;
            }
            if (routeMeshConnectionNotRequired(
                localDescriptor,
                descriptor)) {
                disconnectAdmitted(inbound.source());
                admissionControlReadyConnections.remove(inbound.source());
                admittedPeerObjectRoles.put(
                    inbound.source(),
                    descriptor.objectRole());
                notRequiredPeers.add(inbound.source());
                trySendAdmissionControl(
                    inbound.source(),
                    inboundPair,
                    List.of(wire.encodeReject(4)),
                    "route-not-required");
                return;
            }
            notRequiredPeers.remove(inbound.source());
            ZLinkServiceAdmissionGuard.ConnectionDirection direction =
                command == ServiceWireConstants.COMMAND_HELLO
                    ? ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND
                    : command == ServiceWireConstants.COMMAND_ADMIT
                        ? ZLinkServiceAdmissionGuard
                            .ConnectionDirection.OUTBOUND
                        : topology.peer(inbound.source())
                            .map(peer -> peer.connection().direction())
                            .orElse(
                                ZLinkServiceAdmissionGuard
                                    .ConnectionDirection.OUTBOUND);
            String connectionId = connectionIdForAdmission(
                inbound.source(), direction, inboundPair);
            String previousConnectionId =
                connectionIds.get(inbound.source());
            ZLinkServiceTopologyRegistry.Connection candidate =
                new ZLinkServiceTopologyRegistry.Connection(
                    connectionId,
                    direction,
                    direction.name().toLowerCase(
                        Locale.ROOT)
                        + ":"
                        + descriptor.advertisedEndpoint()
                        + ":"
                        + connectionId);
            ZLinkServiceTopologyRegistry.Peer currentPeer =
                topology.peer(inbound.source()).orElse(null);
            boolean distinctInboundReplacement = currentPeer != null
                && direction
                    == ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND
                && currentPeer.connection().direction()
                    == ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND
                && inboundPair != null
                && selectedTransportPair(currentPeer)
                    .filter(currentPair -> !currentPair.equals(inboundPair))
                    .isPresent()
                && !liveness.isReady(
                    inbound.source(), currentPeer.connectionId())
                && peerIntents.values().stream().noneMatch(intent ->
                    inbound.source().equals(intent.expectedRoutingId())
                        || intent.expectedRoutingId() == null
                            && descriptor.advertisedEndpoint().equals(
                                intent.endpoint()));
            if (distinctInboundReplacement) {
                // A pure inbound peer has no competing local connection
                // intent. If its current pair never became live and a fresh
                // inbound pair arrives, the old entry is a disconnected
                // admission zombie whose monitor edge lost the race with the
                // new HELLO. Replace it immediately. Bilateral/manual peers
                // retain the RID-direction duplicate arbitration below.
                disconnectAdmitted(
                    inbound.source(), currentPeer.connectionId());
            }
            ZLinkServiceTopologyRegistry.AdmissionResult admitted =
                topology.admit(descriptor, candidate);
            if (admitted
                == ZLinkServiceTopologyRegistry.AdmissionResult
                    .DUPLICATE_REJECTED) {
                streamTrace(STREAM_TRACE ? "duplicate-admission-reject source="
                    + inbound.source()
                    + " connection=" + connectionId
                    + " pair=" + (inboundPair == null
                        ? "none" : inboundPair.id()
                            + "/" + inboundPair.generation()) : null);
                trySendAdmissionControl(
                    inbound.source(),
                    inboundPair,
                    List.of(wire.encodeReject(3)),
                    "duplicate-admission");
                return;
            }
            if (admitted
                != ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED) {
                if (topology.peer(inbound.source()).isEmpty()) {
                    rejectedPeers.add(inbound.source());
                }
                trySendAdmissionControl(
                    inbound.source(),
                    inboundPair,
                    List.of(wire.encodeReject(3)),
                    "admission-rejected");
                return;
            }
            rejectedPeers.remove(inbound.source());
            if (command != ServiceWireConstants.COMMAND_UPDATE
                || !connectionId.equals(previousConnectionId)) {
                admissionControlReadyConnections.remove(inbound.source());
            }
            connectionIds.put(inbound.source(), connectionId);
            selectApplicationTransportPair(
                inbound.source(), connectionId);
            streamTrace(STREAM_TRACE ? "admission-stage stage=application-pair-installed"
                + " command=" + command
                + " source=" + inbound.source()
                + " connection=" + connectionId
                + " inboundPair=" + transportPairSummary(inboundPair)
                + " selectedPair=" + transportPairSummary(
                    transportPairs.get(connectionId))
                + " applicationPair=" + transportPairSummary(
                    applicationTransportPairs.get(inbound.source())) : null);
            admitPeerChannels(
                inbound.source(),
                descriptor.channels().stream().collect(
                    Collectors.toMap(
                        ZLinkServiceNodeDescriptor.Channel::name,
                        ZLinkServiceNodeDescriptor.Channel::weight)));
            admittedPeerObjectRoles.put(
                inbound.source(),
                descriptor.objectRole());
            liveness.admit(inbound.source(), connectionId, System.nanoTime());
            liveness.requestProbe(
                inbound.source(), connectionId, System.nanoTime());
            if (command == ServiceWireConstants.COMMAND_ADMIT) {
                admissionControlReadyConnections.put(
                    inbound.source(), connectionId);
                // HELLO and the host's SERVING transition can cross. In that
                // race the peer admits the PREPARING descriptor carried by
                // HELLO after markServiceReady already broadcast its update
                // to the then-known peers. Re-publish the current descriptor
                // at the terminal admission boundary so the new peer cannot
                // retain that stale lifecycle state indefinitely.
                trySendAdmissionControl(
                    inbound.source(),
                    inboundPair,
                    List.of(wire.encodeAdmission(
                        ServiceWireConstants.COMMAND_UPDATE,
                        localDescriptor)),
                    "post-admit-descriptor-sync");
            }
            if (command == ServiceWireConstants.COMMAND_HELLO) {
                trySendAdmissionControl(
                    inbound.source(),
                    inboundPair,
                    List.of(wire.encodeAdmission(
                        ServiceWireConstants.COMMAND_ADMIT,
                        localDescriptor)),
                    "admit-response");
            }
        } catch (RuntimeException invalid) {
            streamTrace(STREAM_TRACE ? "admission-invalid source=" + inbound.source()
                + " command=" + command
                + " error=" + invalid : null);
            trySendAdmissionControl(
                inbound.source(),
                transportPair(inbound),
                List.of(wire.encodeReject(3)),
                "invalid-admission");
        }
    }

    private boolean trySendAdmissionControl(
        RoutingId target,
        TransportPair pair,
        List<byte[]> frames,
        String reason) {
        streamTrace(STREAM_TRACE ? "admission-control-send target=" + target
            + " reason=" + reason
            + " pair=" + (pair == null
                ? "none" : pair.id() + "/" + pair.generation()) : null);
        if (pair == null) {
            // The transport did not identify a physical pair for this
            // exchange (single-connection lanes report no pair identity).
            // Duplicate-pair confirmation (spec 07 §manual topology) only
            // applies when pairs are distinguishable; fall back to the
            // RID-addressed admission path instead of dropping the exchange.
            return trySendAdmissionControl(target, frames, reason);
        }
        int command = wire.decodeHeader(frames.getFirst()).command();
        String connectionId = connectionIds.getOrDefault(target, "");
        try {
            port.send(
                requireStarted(),
                target,
                pair.id(),
                pair.generation(),
                frames)
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        markAdmissionControlReady(
                            target, connectionId, command);
                        return;
                    }
                    streamTrace(STREAM_TRACE ? "admission-control-send-failed target="
                        + target
                        + " reason=" + reason
                        + " error=" + failure.getClass().getSimpleName()
                        + ":" + String.valueOf(failure.getMessage()) : null);
                    if (isAdmissionStateBroadcast(command)) {
                        // ADMIT/UPDATE publish current admission state, so a
                        // stale exact pair may retry on the current RID route.
                        // REJECT is candidate-specific and must never be
                        // laundered onto a replacement pair.
                        trySendAdmissionControl(target, frames, reason);
                    }
                });
            return true;
        } catch (RuntimeException failure) {
            streamTrace(STREAM_TRACE ? "admission-control-send-failed target="
                + target
                + " reason=" + reason
                + " result=PERMANENT_FAILURE"
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()) : null);
            return isAdmissionStateBroadcast(command)
                && trySendAdmissionControl(target, frames, reason);
        }
    }

    private static boolean isAdmissionStateBroadcast(int command) {
        return command == ServiceWireConstants.COMMAND_ADMIT
            || command == ServiceWireConstants.COMMAND_UPDATE;
    }

    private boolean trySendAdmissionControl(
        RoutingId target,
        List<byte[]> frames,
        String reason) {
        streamTrace(STREAM_TRACE ? "admission-control-send-rid target=" + target
            + " reason=" + reason : null);
        int command = wire.decodeHeader(frames.getFirst()).command();
        String connectionId = connectionIds.getOrDefault(target, "");
        try {
            port.send(
                requireStarted(),
                target,
                frames)
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        markAdmissionControlReady(
                            target, connectionId, command);
                        return;
                    }
                    streamTrace(STREAM_TRACE ? "admission-control-send-failed target="
                        + target
                        + " reason=" + reason
                        + " error=" + failure.getClass().getSimpleName()
                        + ":" + String.valueOf(failure.getMessage()) : null);
                });
            return true;
        } catch (RuntimeException failure) {
            streamTrace(STREAM_TRACE ? "admission-control-send-failed target="
                + target
                + " reason=" + reason
                + " result=PERMANENT_FAILURE"
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()) : null);
            return false;
        }
    }

    private void markAdmissionControlReady(
        RoutingId target,
        String connectionId,
        int command) {
        if (command != ServiceWireConstants.COMMAND_ADMIT
            || connectionId == null
            || connectionId.isBlank()
            || !connectionId.equals(connectionIds.get(target))) {
            return;
        }
        admissionControlReadyConnections.put(target, connectionId);
    }

    private void dispatchLiveness(
        ZLinkJavaRawServicePort.Inbound inbound,
        int command) {
        try {
            ZLinkServiceWireFrame record =
                new ZLinkServiceWireCodec().decode(inbound.frames());
            long probeId = ByteBuffer.wrap(record.frames().getFirst()).getLong();
            ZLinkServiceTopologyRegistry.Peer peer =
                topology.peer(inbound.source()).orElseThrow();
            TransportPair inboundPair = transportPair(inbound);
            // Anti-laundering: an ack must not refresh the admitted
            // connection when its frame demonstrably traveled on a pair
            // OWNED BY A DIFFERENT connection of the same peer. Pair ids
            // differ per transport lane, so comparing against the single
            // recorded "selected" pair rejects legitimate control-lane
            // liveness and starves readiness; only a positive ownership
            // mismatch is a laundering signal.
            if (inboundPair != null) {
                String owning = topology.peers().stream()
                    .filter(candidate -> selectedTransportPair(candidate)
                        .filter(inboundPair::equals)
                        .isPresent())
                    .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                    .filter(owner -> !owner.equals(peer.connectionId()))
                    .findFirst()
                    .orElse(null);
                if (owning != null && !owning.equals(peer.connectionId())) {
                    streamTrace(STREAM_TRACE ? "liveness-ignored-foreign-pair source="
                        + inbound.source()
                        + " connection=" + peer.connectionId()
                        + " owning=" + owning
                        + " pair=" + inboundPair.id()
                        + "/" + inboundPair.generation() : null);
                    return;
                }
            }
            if (command == ServiceWireConstants.COMMAND_LIVENESS_PROBE) {
                streamTrace(STREAM_TRACE ? "liveness-probe source=" + inbound.source()
                    + " connection=" + peer.connectionId() : null);
                if (liveness.acknowledgeProbe(
                    inbound.source(),
                    peer.connectionId(),
                    probeId).isPresent()) {
                    List<byte[]> acknowledgement = encodeLiveness(
                        ServiceWireConstants.COMMAND_LIVENESS_ACK,
                        probeId);
                    // Java RouterSocket.send is request-shaped. A .NET probe
                    // therefore must be completed on its original request
                    // route, rather than by a fresh routed send.
                    if (inbound.requestSequence() != null) {
                        port.reply(
                            requireStarted(),
                            inbound.source(),
                            inbound.requestSequence(),
                            acknowledgement);
                    } else {
                        sendLiveness(
                            inbound.source(), inboundPair, acknowledgement);
                    }
                }
            } else {
                boolean acknowledged = liveness.acknowledge(
                    inbound.source(),
                    peer.connectionId(),
                    probeId,
                    System.nanoTime());
                if (acknowledged) {
                    LOGGER.info("ZLINK_FRAMEWORK_PEER_READY mesh=" + meshName
                        + " peer=" + inbound.source());
                }
                streamTrace(STREAM_TRACE ? "liveness-ack source=" + inbound.source()
                    + " connection=" + peer.connectionId()
                    + " probe=" + probeId
                    + " accepted=" + acknowledged : null);
            }
        } catch (RuntimeException failure) {
            streamTrace(STREAM_TRACE ? "liveness-dispatch-failed source="
                + inbound.source()
                + " command=" + command
                + " failure=" + failure : null);
        }
    }

    static List<RoutingId> readyAdmissionPeerIds(
        List<ZLinkServiceTopologyRegistry.Peer> peers,
        Predicate<ZLinkServiceTopologyRegistry.Peer> isReady) {
        return peers.stream()
            .filter(isReady)
            .map(peer -> peer.descriptor().nodeRoutingId())
            .toList();
    }

    private void drainMonitorEvents() {
        MonitorEvent event;
        while ((event = monitorEvents.poll()) != null) {
            Optional<RoutingId> peer = event.routingId();
            if (peer.isEmpty()) {
                if (event.event() == MonitorEventType.DISCONNECTED
                    || event.event() == MonitorEventType.CLOSED) {
                    markPeerIntentsClosed(event, null);
                } else if (event.event() == MonitorEventType.CONNECTION_READY
                    && !isConnectionReadyEdge(event)
                    && event.transportPairId() != 0
                    && event.transportPairGeneration() != 0) {
                    markPeerIntentPairClosed(event, null);
                }
                continue;
            }
            if (event.event() == MonitorEventType.CONNECTION_READY) {
                RoutingId peerRid = peer.orElseThrow();
                if (!isConnectionReadyEdge(event)) {
                    if (event.transportPairId() != 0
                        && event.transportPairGeneration() != 0) {
                        markPeerIntentPairClosed(event, peerRid);
                    }
                    continue;
                }
                markPeerIntentsActive(event, peerRid);
                ZLinkServiceAdmissionGuard.ConnectionDirection direction =
                    monitorConnectionDirection(event, peerRid);
                String registeredId =
                    registerTransportConnection(event, peerRid, direction);
                ZLinkServiceTopologyRegistry.Peer admitted =
                    topology.peer(peerRid).orElse(null);
                if (admitted == null
                    || !admitted.connectionId().equals(registeredId)) {
                    pendingConnectionIds.computeIfAbsent(
                            new ConnectionCandidate(peerRid, direction),
                            ignored ->
                                new ConcurrentLinkedQueue<>())
                        .add(registeredId);
                }
                nextAnnouncementNanos.put(peer.orElseThrow(), 0L);
            } else if (event.event() == MonitorEventType.DISCONNECTED
                || event.event() == MonitorEventType.CLOSED) {
                RoutingId peerRid = peer.orElseThrow();
                markPeerIntentsClosed(event, peerRid);
                String disconnectedId = removeTransportConnection(event);
                if (disconnectedId == null) {
                    // The monitor edge could not be mapped back to its
                    // registered connection id (pair churn). Leaving the
                    // topology's current connection in place makes it a
                    // zombie that rejects every fresh candidate as a
                    // duplicate until the liveness timeout (~30s admission
                    // lockout). Attribute the edge to the current
                    // connection when its recorded pair matches — or when
                    // no pair was recorded to check against — and evict it
                    // now; re-admission is one HELLO round-trip.
                    final long edgePairId = event.transportPairId();
                    final long edgePairGeneration =
                        event.transportPairGeneration();
                    // Attribute only on a positive pair match: transports
                    // without pair identity (inproc lanes) see unrelated
                    // DISCONNECTED edges, and evicting the admitted peer on
                    // those churns admission forever.
                    disconnectedId = topology.peer(peerRid)
                        .filter(current -> selectedTransportPair(current)
                            .map(pair -> pair.id() == edgePairId
                                && pair.generation() == edgePairGeneration)
                            .orElse(false))
                        .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                        .orElse(null);
                    if (disconnectedId != null) {
                        streamTrace(STREAM_TRACE ? "monitor-disconnect-evicts-current peer="
                            + peerRid + " connection=" + disconnectedId : null);
                    }
                }
                discardPendingConnectionId(disconnectedId);
                disconnectAdmitted(peerRid, disconnectedId);
                removeApplicationTransportPair(peerRid, event);
                nextAnnouncementNanos.put(peerRid, 0L);
            }
        }
    }

    private static boolean isConnectionReadyEdge(MonitorEvent event) {
        return (event.flags() & CONNECTION_READY_EDGE_FLAG) != 0;
    }

    private void markPeerIntentsActive(
        MonitorEvent event,
        RoutingId peerRid) {
        peerIntents.forEach((intentId, intent) -> {
            if (event.value() > 0
                && !closeRequestedPeerIntents.contains(intentId)
                && matchesMonitorPeer(intentId, intent, event, peerRid)) {
                closedPeerIntents.remove(intentId);
                livePeerIntents.add(intentId);
                if (peerRid != null
                    && intent.expectedRoutingId() == null) {
                    // An endpoint-only intent learns its peer's routing id
                    // from this monitor edge. Recording it here is what lets
                    // announceExpectedPeers open admission on a blind
                    // connect(endpoint): a peer that only listens, or that
                    // only announces to peers it was configured to expect
                    // (Node, .NET, and this runtime), never speaks first, so
                    // an un-announced blind connect leaves the pipe
                    // ESTABLISHED with no HELLO ever exchanged.
                    peerIntentRoutingIds.put(intentId, peerRid);
                }
                peerIntentTransports.computeIfAbsent(
                    intentId,
                    ignored -> ConcurrentHashMap.newKeySet())
                    .add(TransportIdentity.from(event));
            }
        });
    }

    private void markPeerIntentsClosed(
        MonitorEvent event,
        RoutingId peerRid) {
        peerIntents.forEach((intentId, intent) -> {
            Set<TransportIdentity> transports =
                peerIntentTransports.get(intentId);
            boolean matchedTransport = transports != null
                && transports.removeIf(transport -> transport.matches(event));
            boolean closingUntrackedTransport =
                closeRequestedPeerIntents.contains(intentId)
                    && (transports == null || transports.isEmpty())
                    && matchesMonitorPeer(intentId, intent, event, peerRid);
            if (!matchedTransport && !closingUntrackedTransport) {
                return;
            }
            if (transports == null || transports.isEmpty()) {
                closedPeerIntents.add(intentId);
                closeRequestedPeerIntents.remove(intentId);
                livePeerIntents.remove(intentId);
                peerIntentTransports.remove(intentId);
                cleanupClosedPeerEndpoint(intent.endpoint());
            }
        });
    }

    private void markPeerIntentPairClosed(
        MonitorEvent event,
        RoutingId peerRid) {
        peerIntents.forEach((intentId, intent) -> {
            if (!closeRequestedPeerIntents.contains(intentId)
                || !matchesMonitorPeer(intentId, intent, event, peerRid)) {
                return;
            }
            Set<TransportIdentity> transports =
                peerIntentTransports.get(intentId);
            if (transports == null || transports.isEmpty()) {
                closedPeerIntents.add(intentId);
                closeRequestedPeerIntents.remove(intentId);
                livePeerIntents.remove(intentId);
                peerIntentTransports.remove(intentId);
                cleanupClosedPeerEndpoint(intent.endpoint());
                return;
            }
            transports.removeIf(transport ->
                transport.pairId() == event.transportPairId()
                    && transport.pairGeneration()
                        == event.transportPairGeneration());
            if (transports.isEmpty()) {
                closedPeerIntents.add(intentId);
                closeRequestedPeerIntents.remove(intentId);
                livePeerIntents.remove(intentId);
                cleanupClosedPeerEndpoint(intent.endpoint());
            }
        });
    }

    private void cleanupClosedPeerEndpoint(String endpoint) {
        RouterSocket current = router;
        if (current == null || endpoint.startsWith("inproc://")) {
            return;
        }
        try {
            // Exact-pair termination disables reconnect but intentionally
            // keeps the endpoint registration. Remove that registration only
            // after the monitor has confirmed physical pair closure so the
            // following replacement connect creates a new transport pair.
            current.disconnect(endpoint);
        } catch (RuntimeException ignored) {
            // A concurrent teardown may already have removed the endpoint.
        }
    }

    private void rememberPeerIntentRoutingId(
        PeerIntent expected,
        RoutingId peerRid) {
        if (expected == null) {
            return;
        }
        peerIntents.forEach((intentId, intent) -> {
            if (intent == expected
                && !closeRequestedPeerIntents.contains(intentId)) {
                peerIntentRoutingIds.put(intentId, peerRid);
                livePeerIntents.add(intentId);
            }
        });
    }

    private boolean matchesMonitorPeer(
        long intentId,
        PeerIntent intent,
        MonitorEvent event,
        RoutingId peerRid) {
        if (peerRid != null
            && (peerRid.equals(intent.expectedRoutingId())
                || peerRid.equals(peerIntentRoutingIds.get(intentId)))) {
            return true;
        }
        if (!event.remoteAddr().isBlank()
            && event.remoteAddr().equals(intent.endpoint())) {
            return true;
        }
        return event.localAddr().isBlank()
            && event.remoteAddr().isBlank()
            && intent.expectedRoutingId() == null
            && peerIntents.values().stream()
                .filter(candidate -> candidate.expectedRoutingId() == null)
                .count() == 1;
    }

    private void announceExpectedPeers(long nowNanos) {
        for (Map.Entry<Long, PeerIntent> entry : peerIntents.entrySet()) {
            PeerIntent intent = entry.getValue();
            // A locally initiated connection announces itself. An
            // endpoint-only intent has no configured routing id, so it uses
            // the one its CONNECTION_READY monitor edge reported: peers that
            // only listen (Node, .NET) never send the first HELLO, so an
            // un-announced blind connect deadlocks admission forever.
            RoutingId expected = intent.expectedRoutingId() != null
                ? intent.expectedRoutingId()
                : peerIntentRoutingIds.get(entry.getKey());
            if (expected == null || topology.peer(expected).isPresent()) {
                continue;
            }
            if (notRequiredPeers.contains(expected)) {
                continue;
            }
            long next = nextAnnouncementNanos.getOrDefault(expected, 0L);
            if (nowNanos < next) {
                continue;
            }
            try {
                port.send(
                    requireStarted(),
                    expected,
                    List.of(wire.encodeAdmission(
                        ServiceWireConstants.COMMAND_HELLO,
                        localDescriptor)));
            } catch (RuntimeException ignored) {
                // A manual peer can be configured before its listener is
                // ready. The next announcement interval retries admission.
            }
            nextAnnouncementNanos.put(
                expected,
                nowNanos + Duration.ofMillis(100).toNanos());
        }
    }

    private void tickLiveness(long nowNanos) {
        ZLinkServiceLivenessRegistry.Tick tick = liveness.tick(nowNanos);
        for (ZLinkServiceLivenessRegistry.Probe probe : tick.probes()) {
            try {
                // RID-addressed: the probe rides whatever lane/pair the
                // native route currently uses; pinning to the recorded
                // admission pair mis-lanes the control probe when pair ids
                // differ per transport lane.
                sendLiveness(
                        probe.nodeRoutingId(),
                        null,
                        encodeLiveness(
                            ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                            probe.probeId()))
                    .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                        "liveness-send target=" + probe.nodeRoutingId()
                            + " connection=" + probe.connectionId()
                            + " probe=" + probe.probeId()
                            + " result="
                            + (failure == null ? "accepted" : "failed") : null));
            } catch (RuntimeException ignored) {
                streamTrace(STREAM_TRACE ? "liveness-send-failed target="
                    + probe.nodeRoutingId()
                    + " connection=" + probe.connectionId()
                    + " error=" + ignored.getClass().getSimpleName()
                    + ":" + String.valueOf(ignored.getMessage()) : null);
                // The liveness timeout owns peer eviction. A transient send
                // failure must not stop the service receive pump.
            }
        }
        tick.timedOutNodes().forEach(this::disconnectAdmitted);
    }

    private CompletionStage<Void> sendLiveness(
        RoutingId target,
        TransportPair pair,
        List<byte[]> frames) {
        return pair == null
            ? port.send(requireStarted(), target, frames)
            : port.send(
                requireStarted(),
                target,
                pair.id(),
                pair.generation(),
                frames);
    }

    private static List<byte[]> encodeLiveness(int command, long probeId) {
        return new ZLinkServiceWireCodec().encode(
            new ZLinkServiceWireFrame(
                command,
                0,
                List.of(ByteBuffer.allocate(Long.BYTES)
                    .putLong(probeId)
                    .array())));
    }

    private static void validateCanonicalRelocationControl(byte[] record) {
        ZLinkServiceWireFrame decoded =
            new ZLinkServiceWireCodec().decode(List.of(record));
        if (!isCanonicalRelocationControl(decoded.command())) {
            throw new IllegalArgumentException(
                "record is not a canonical relocation control command");
        }
    }

    private static byte[] canonicalPrepareReply(byte[] record) {
        byte[] reply = Objects.requireNonNull(record, "record").clone();
        validateCanonicalRelocationControl(reply);
        int command = Byte.toUnsignedInt(reply[3]);
        if (command != ServiceWireConstants.COMMAND_RELOCATION_READY
            && command != ServiceWireConstants.COMMAND_RELOCATION_FAILED) {
            throw new IllegalArgumentException(
                "canonical relocation prepare reply must be READY or FAILED");
        }
        return reply;
    }

    private static boolean isCanonicalRelocationControl(int command) {
        return command == ServiceWireConstants.COMMAND_RELOCATION_READY
            || command == ServiceWireConstants.COMMAND_RELOCATION_FAILED
            || command == ServiceWireConstants.COMMAND_RELOCATION_DATA
            || command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE
            || command == ServiceWireConstants.COMMAND_RELOCATION_CUTOVER
            || command == ServiceWireConstants.COMMAND_RELOCATION_STATE;
    }

    private void disconnectAdmitted(RoutingId peer) {
        disconnectAdmitted(peer, connectionIds.get(peer));
    }

    private void disconnectAdmitted(
        RoutingId peer,
        String connectionId) {
        if (connectionId == null) {
            admissionControlReadyConnections.remove(peer);
            return;
        }
        if (!topology.disconnect(peer, connectionId)) {
            admissionControlReadyConnections.remove(peer, connectionId);
            return;
        }
        admissionControlReadyConnections.remove(peer, connectionId);
        connectionIds.remove(peer, connectionId);
        boolean wasAdmitted = admittedPeerChannels.remove(peer) != null;
        if (wasAdmitted && !notRequiredPeers.contains(peer)) {
            disconnectedPeers.add(peer);
        }
        if (!notRequiredPeers.contains(peer)) {
            admittedPeerObjectRoles.remove(peer);
        }
        liveness.disconnect(peer, connectionId);
    }

    private void forgetKnownPeerChannelsIfUntracked(RoutingId peer) {
        boolean hasIntent = peerIntents.values().stream()
            .anyMatch(intent -> peer.equals(intent.expectedRoutingId()));
        if (!hasIntent && !peerAdmissionExpectations.containsKey(peer)) {
            knownPeerChannels.remove(peer);
            disconnectedPeers.remove(peer);
        }
    }

    private String connectionIdForAdmission(
        RoutingId peer,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction,
        TransportPair pair) {
        if (pair != null) {
            // Pair identity is the authoritative physical connection
            // identity: HELLO and ADMIT describe opposite local directions
            // while completing the SAME pair's handshake (direction here is
            // inferred from the *command*, not the physical edge), so a
            // connectionId already assigned to this pair -- by an earlier
            // admission message on it, or by the CONNECTION_READY monitor
            // edge that first registered it -- must be reused before the
            // command-direction-bucketed pending queue below is even
            // consulted. Consulting that queue first can strand the
            // monitor's own pending candidate under the wrong direction
            // bucket; a later admission message on the exact same physical
            // pair then "discovers" that stranded candidate and mints a
            // SECOND connectionId for one physical connection, which
            // collides with the first as a false duplicate and gets
            // rejected -- corrupting the peer's admission for the whole
            // session (spec 07 admission is a per-peer, not per-message,
            // state machine).
            //
            // An already-ADMITTED peer's own connectionId is the single
            // authoritative source once one exists, so it is checked first:
            // transportPairs can (rarely) hold more than one id for the same
            // physical pair -- a second CONNECTION_READY edge on an
            // already-registered pair, or a stale id a DISCONNECTED edge has
            // not pruned yet -- and scanning it by value alone is unordered
            // over a ConcurrentHashMap. Only fall back to that scan for the
            // pre-admission case, where no single id is yet authoritative.
            String reuse = topology.peer(peer)
                .filter(current -> pair.equals(
                    transportPairs.get(current.connectionId())))
                .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                .orElseGet(() -> transportPairs.entrySet().stream()
                    .filter(entry -> pair.equals(entry.getValue()))
                    .map(Map.Entry::getKey)
                    .findFirst()
                    .orElse(null));
            if (reuse != null) {
                ConnectionCandidate owner =
                    new ConnectionCandidate(peer, direction);
                var strandedPending = pendingConnectionIds.get(owner);
                if (strandedPending != null) {
                    strandedPending.remove(reuse);
                    if (strandedPending.isEmpty()) {
                        pendingConnectionIds.remove(owner, strandedPending);
                    }
                }
                return reuse;
            }
        }
        ConnectionCandidate candidate =
            new ConnectionCandidate(peer, direction);
        var pending = pendingConnectionIds.get(candidate);
        String connectionId = null;
        if (pending != null && pair != null) {
            connectionId = pending.stream()
                .filter(id -> pair.equals(transportPairs.get(id)))
                .findFirst()
                .orElse(null);
            if (connectionId != null) {
                pending.remove(connectionId);
            }
        }
        if (connectionId == null && pending != null && pair == null) {
            connectionId = pending.poll();
        }
        if (pending != null && pending.isEmpty()) {
            pendingConnectionIds.remove(candidate, pending);
        }
        if (connectionId != null) {
            return connectionId;
        }
        if (pair != null) {
            // Admission announcements can be retransmitted on the same
            // physical pair after its pending monitor id was consumed. Keep
            // that pair attached to the current connection; manufacturing a
            // new id here turns a retransmission into a false duplicate and
            // makes both sides reject and disconnect the live pipe. HELLO
            // and ADMIT describe opposite local directions while completing
            // the same pair's handshake, so pair identity takes precedence
            // over the command-derived direction here.
            String currentConnectionId = topology.peer(peer)
                .filter(current -> pair.equals(
                    transportPairs.get(current.connectionId())))
                .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
                .orElse(null);
            if (currentConnectionId != null) {
                return currentConnectionId;
            }
            String generated = UUID.randomUUID().toString();
            transportPairs.put(generated, pair);
            return generated;
        }
        return topology.peer(peer)
            .filter(current ->
                current.connection().direction() == direction)
            .map(ZLinkServiceTopologyRegistry.Peer::connectionId)
            .orElseGet(() -> UUID.randomUUID().toString());
    }

    private PeerIntent findExpectedPeer(
        RoutingId peer,
        String advertisedEndpoint) {
        return peerIntents.values().stream()
            .filter(intent ->
                peer.equals(intent.expectedRoutingId())
                    || intent.expectedRoutingId() == null
                        && intent.endpoint().equals(advertisedEndpoint))
            .sorted(Comparator.comparing(PeerIntent::endpoint)
                .thenComparingLong(PeerIntent::createdAtMs))
            .findFirst()
            .orElse(null);
    }

    static String expectedRouteMismatchFields(
        String expectedEndpoint,
        String expectedSecurityIdentity,
        long expectedLifecycleGeneration,
        ZLinkServiceNodeDescriptor incoming) {
        List<String> fields = new ArrayList<>(3);
        if (expectedEndpoint != null
            && !expectedEndpoint.equals(incoming.advertisedEndpoint())) {
            fields.add("endpoint");
        }
        if (expectedSecurityIdentity != null
            && !expectedSecurityIdentity.equals(incoming.securityIdentity())) {
            fields.add("security-identity");
        }
        if (expectedLifecycleGeneration != 0
            && expectedLifecycleGeneration
                != incoming.lifecycleGeneration()) {
            fields.add("lifecycle-generation");
        }
        return String.join(",", fields);
    }

    private String registerTransportConnection(
        MonitorEvent event,
        RoutingId peer,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction) {
        String currentId = connectionIds.get(peer);
        boolean currentAdmissionNeedsMonitorIdentity =
            currentId != null
                && topology.peer(peer)
                    .map(current ->
                        current.connectionId().equals(currentId)
                        && current.connection().direction() == direction)
                    .orElse(false)
                && monitorConnectionIds.values().stream()
                    .noneMatch(ids -> ids.contains(currentId));
        String id = currentAdmissionNeedsMonitorIdentity
            ? currentId
            : UUID.randomUUID().toString();
        monitorConnectionIds.computeIfAbsent(
            transportEventKey(event),
            ignored -> new ConcurrentLinkedQueue<>())
            .add(id);
        if (event.transportPairId() != 0
            && event.transportPairGeneration() != 0) {
            TransportPair pair = new TransportPair(
                event.transportPairId(), event.transportPairGeneration());
            if (event.transportLane() == 0
                && (event.connectionId() != 0
                    || !event.localAddr().isBlank())) {
                transportPairs.put(id, pair);
                topology.peer(peer)
                    .filter(admitted -> admitted.connectionId().equals(id))
                    .ifPresent(ignored ->
                        applicationTransportPairs.put(peer, pair));
            }
        }
        return id;
    }

    private TransportPair transportPairFor(RoutingId peer) {
        return applicationTransportPairs.get(peer);
    }

    private void selectApplicationTransportPair(
        RoutingId peer,
        String connectionId) {
        TransportPair selected = transportPairs.get(connectionId);
        if (selected == null) {
            applicationTransportPairs.remove(peer);
        } else {
            applicationTransportPairs.put(peer, selected);
        }
    }

    private static String transportPairSummary(TransportPair pair) {
        return pair == null ? "none" : pair.id() + "/" + pair.generation();
    }

    private static String submitFailureSummary(Throwable failure) {
        if (failure == null) {
            return "accepted";
        }
        Throwable current = failure;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZlinkSubmitException submit) {
            return "rejected submitResult=" + submit.getResult()
                + " nativeErrno=" + submit.getNativeErrno();
        }
        return "failed error=" + current.getClass().getSimpleName()
            + ":" + String.valueOf(current.getMessage());
    }

    private static String requestFailureSummary(Throwable failure) {
        if (failure == null) {
            return "accepted";
        }
        Throwable current = failure;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZlinkRequestException request) {
            return "rejected requestResult=" + request.getResult()
                + " nativeErrno=" + request.getNativeErrno();
        }
        return submitFailureSummary(current);
    }

    private CompletionStage<Void> sendApplication(
        RoutingId target,
        List<byte[]> frames) {
        TransportPair pair = transportPairFor(target);
        streamTrace(STREAM_TRACE ? "application-send target=" + target
            + " pair=" + transportPairSummary(pair) : null);
        return pair == null
            ? port.send(requireStarted(), target, frames)
            : port.send(
                requireStarted(), target, pair.id(), pair.generation(), frames);
    }

    private CompletionStage<List<byte[]>> requestApplication(
        RoutingId target,
        List<byte[]> frames,
        Duration timeout) {
        TransportPair pair = transportPairFor(target);
        streamTrace(STREAM_TRACE ? "application-request target=" + target
            + " pair=" + transportPairSummary(pair) : null);
        CompletionStage<List<byte[]>> submission = pair == null
            ? port.request(requireStarted(), target, frames, timeout)
            : port.request(
                requireStarted(), target, pair.id(), pair.generation(),
                frames, timeout);
        return submission.whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
            "application-request-submit target=" + target
                + " pair=" + transportPairSummary(pair)
                + " result=" + requestFailureSummary(failure) : null));
    }

    private Optional<TransportPair> selectedTransportPair(RoutingId peer) {
        return topology.peer(peer).flatMap(this::selectedTransportPair);
    }

    private Optional<TransportPair> selectedTransportPair(
        ZLinkServiceTopologyRegistry.Peer peer) {
        return Optional.ofNullable(transportPairs.get(peer.connectionId()));
    }

    private static TransportPair transportPair(
        ZLinkJavaRawServicePort.Inbound inbound) {
        return inbound.transportPairId() == 0
                || inbound.transportPairGeneration() == 0
            ? null
            : new TransportPair(
                inbound.transportPairId(),
                inbound.transportPairGeneration());
    }

    private void disconnectTransportPair(TransportPair pair) {
        RouterSocket current = router;
        if (current == null || pair == null) {
            return;
        }
        streamTrace(STREAM_TRACE ? "disconnect-transport-pair id=" + pair.id()
            + " generation=" + pair.generation() : null);
        try {
            current.disconnectTransportPair(pair.id(), pair.generation());
        } catch (RuntimeException ignored) {
            // The pair may already have reached its terminal monitor edge.
        }
    }

    private void removeApplicationTransportPair(
        RoutingId peer,
        MonitorEvent event) {
        if (event.transportLane() != 0
            || event.transportPairId() == 0
            || event.transportPairGeneration() == 0) {
            return;
        }
        applicationTransportPairs.computeIfPresent(peer, (ignored, current) ->
            current.id() == event.transportPairId()
                && current.generation() == event.transportPairGeneration()
                ? null
                : current);
    }

    private ZLinkServiceAdmissionGuard.ConnectionDirection
        monitorConnectionDirection(
            MonitorEvent event,
            RoutingId peer) {
        if (Objects.equals(event.localAddr(), bindEndpoint)) {
            return ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND;
        }
        boolean matchesOutboundIntent = peerIntents.values().stream()
            .anyMatch(intent ->
                (intent.expectedRoutingId() == null
                    || peer.equals(intent.expectedRoutingId()))
                && intent.endpoint().equals(event.remoteAddr()));
        if (matchesOutboundIntent) {
            return ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND;
        }
        return ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND;
    }

    private String removeTransportConnection(MonitorEvent event) {
        String key = transportEventKey(event);
        var ids = monitorConnectionIds.get(key);
        if (ids == null) {
            return null;
        }
        TransportPair eventPair = event.transportPairId() == 0
                || event.transportPairGeneration() == 0
            ? null
            : new TransportPair(
                event.transportPairId(), event.transportPairGeneration());
        String id = eventPair == null
            ? ids.poll()
            : ids.stream()
                .filter(candidate -> eventPair.equals(
                    transportPairs.get(candidate)))
                .findFirst()
                .orElse(null);
        if (id != null && eventPair != null) {
            ids.remove(id);
        }
        if (ids.isEmpty()) {
            monitorConnectionIds.remove(key, ids);
        }
        if (id != null) {
            transportPairs.remove(id);
        }
        return id;
    }

    private void discardPendingConnectionId(String connectionId) {
        if (connectionId == null) {
            return;
        }
        for (var entry : pendingConnectionIds.entrySet()) {
            var ids = entry.getValue();
            ids.remove(connectionId);
            if (ids.isEmpty()) {
                pendingConnectionIds.remove(entry.getKey(), ids);
            }
        }
    }

    private void clearConnectionCandidates(RoutingId peer) {
        pendingConnectionIds.keySet().removeIf(
            candidate -> candidate.peer().equals(peer));
    }

    static String transportEventKey(MonitorEvent event) {
        // MonitorEvent.value is event-specific (for example a socket
        // descriptor or an error code), so it cannot identify one physical
        // connection across READY and DISCONNECTED notifications. The local
        // and remote endpoints are the stable pair exposed by the binding.
        return String.valueOf(event.localAddr())
            + "|"
            + String.valueOf(event.remoteAddr());
    }

    record TransportPair(long id, long generation) {
    }

    private record PeerCloseRequest(
        long connectionIntentId,
        String endpoint,
        Set<TransportPair> transportPairs) {
    }

    private record TransportIdentity(
        String eventKey,
        long connectionId,
        long pairId,
        long pairGeneration,
        int lane) {

        static TransportIdentity from(MonitorEvent event) {
            return new TransportIdentity(
                transportEventKey(event),
                event.connectionId(),
                event.transportPairId(),
                event.transportPairGeneration(),
                event.transportLane());
        }

        boolean matches(MonitorEvent event) {
            if (pairId != 0
                && pairGeneration != 0
                && pairId == event.transportPairId()
                && pairGeneration == event.transportPairGeneration()) {
                return lane == event.transportLane();
            }
            return connectionId != 0
                && event.connectionId() != 0
                && connectionId == event.connectionId();
        }
    }

    private void disconnectNotRequiredTransport(RoutingId peer) {
        RouterSocket current = router;
        if (current == null) {
            return;
        }
        peerIntents.values().stream()
            .filter(intent -> peer.equals(intent.expectedRoutingId()))
            .map(PeerIntent::endpoint)
            .distinct()
            .forEach(endpoint -> {
                try {
                    current.disconnect(endpoint);
                } catch (RuntimeException ignored) {
                    // The terminal NotRequired state is authoritative even
                    // if the transport already removed the candidate.
                }
            });
    }

    private void drainApplicationMailbox() {
        ZLinkServiceMailbox currentMailbox = mailbox;
        var queue = applicationJobQueue;
        if (currentMailbox == null
            || queue == null
            || !applicationDrainActive.compareAndSet(false, true)) {
            return;
        }
        queue.acquire().whenComplete((permit, admissionFailure) -> {
            if (admissionFailure != null || permit == null || closed.get()) {
                if (permit != null) {
                    permit.close();
                }
                applicationDrainActive.set(false);
                return;
            }
            Optional<ZLinkServiceMailbox.Claim> claimed = currentMailbox.tryClaim(
                ZLinkServiceMailbox.Domain.APPLICATION,
                1,
                1024L * 1024);
            if (claimed.isEmpty()) {
                permit.close();
                applicationDrainActive.set(false);
                if (currentMailbox.pendingMessages(
                    ZLinkServiceMailbox.Domain.APPLICATION) > 0) {
                    applicationDispatch.execute(this::drainApplicationMailbox);
                }
                return;
            }
            ZLinkServiceMailbox.Claim claim = claimed.orElseThrow();
            try (var ignored = systems.zlink.framework.runtime.internal.dispatch
                     .ZLinkApplicationJobContext.enter(permit)) {
                applicationDispatch.execute(() -> {
                    try {
                        ZLinkServiceMailbox.Record record =
                            claim.records().getFirst();
                        Long envelopeId = record.correlation();
                        ZLinkMeshDispatchRecord dispatch =
                            dispatchEnvelopes.remove(envelopeId);
                        if (dispatch != null) {
                            boolean accepted = false;
                            try {
                                receiver.accept(dispatch);
                                accepted = true;
                            } finally {
                                if (!accepted) {
                                    dispatch.close();
                                }
                            }
                        }
                    } finally {
                        currentMailbox.release(claim);
                        drainApplicationMailbox();
                    }
                });
            } catch (RejectedExecutionException rejected) {
                for (ZLinkServiceMailbox.Record record : claim.records()) {
                    ZLinkMeshDispatchRecord dispatch =
                        dispatchEnvelopes.remove(record.correlation());
                    if (dispatch != null) {
                        dispatch.close();
                    }
                }
                currentMailbox.release(claim);
            } finally {
                permit.abandonReservation();
                applicationDrainActive.set(false);
                drainApplicationMailbox();
            }
        });
    }

    private RouterSocket requireStarted() {
        RouterSocket current = router;
        if (current == null || closed.get()) {
            throw new IllegalStateException("raw MeshNode is not started");
        }
        return current;
    }

    private void requireCreated() {
        if (state != MeshNodeState.CREATED) {
            throw new IllegalStateException("MeshNode configuration is closed");
        }
    }

    private static String requireChannel(String value) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("channelName is required");
        }
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > 255 || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("channelName exceeds text8");
        }
        return value;
    }

    private ZLinkServiceNodeDescriptor descriptor(
        long lifecycle,
        long revision,
        List<ZLinkServiceNodeDescriptor.Channel> channels,
        ZLinkServiceNodeDescriptor.State descriptorState) {
        return new ZLinkServiceNodeDescriptor(
            meshName,
            routingId,
            lifecycle,
            revision,
            advertisedEndpoint(bindEndpoint),
            channels,
            descriptorState,
            //  securityIdentity is the plaintext-transport placeholder every
            //  language encodes, not this node's routing id. Encoding the
            //  routing id here made every non-Java peer that fences an
            //  expected security identity ("default") reject this
            //  descriptor.
            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
            0,
            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
            objectRole,
            placementWeight,
            10_000,
            128,
            0,
            0);
    }

    private String advertisedEndpoint(String actualEndpoint) {
        if (advertiseHost == null || actualEndpoint == null) {
            return actualEndpoint;
        }
        //  Write-time normalization (endpoint notation policy §2.3/§2.4):
        //  IPv6-safe host substitution. The former lastIndexOf(':') split
        //  broke on IPv6 literals, which carry more than one colon.
        return ZLinkEndpointNotation.normalize(
            ZLinkEndpointNotation.withHost(actualEndpoint, advertiseHost));
    }

    private static boolean routeMeshConnectionNotRequired(
        ZLinkServiceNodeDescriptor local,
        ZLinkServiceNodeDescriptor remote) {
        return local != null
            && local.objectRole()
                == ZLinkServiceNodeDescriptor.ObjectRole.CLIENT
            && local.channels().isEmpty()
            && remote.objectRole()
                == ZLinkServiceNodeDescriptor.ObjectRole.CLIENT
            && remote.channels().isEmpty();
    }

    boolean isObjectClientNodeDirectTarget(RoutingId target) {
        if (routingId.equals(target)) {
            return objectRole == ZLinkServiceNodeDescriptor.ObjectRole.CLIENT;
        }
        return admittedPeerObjectRoles.get(target)
            == ZLinkServiceNodeDescriptor.ObjectRole.CLIENT;
    }

    private static long positiveRandomLong() {
        return ThreadLocalRandom.current().nextLong(1, Long.MAX_VALUE);
    }

    private long allocateCorrelation() {
        long value = nextCorrelation.getAndIncrement();
        if (value <= 0) {
            throw new IllegalStateException("service correlation is exhausted");
        }
        return value;
    }

    static ZLinkServiceM6AWireCodec.ApplicationPayload applicationPayload(
        List<Message> parts) {
        return ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(parts);
    }

    static List<Message> applicationMessages(
        ZLinkServiceM6AWireCodec.ApplicationPayload payload,
        boolean request,
        Long requestSequence) {
        if (request && requestSequence == null) {
            throw new IllegalArgumentException(
                "request application payload requires a request sequence");
        }
        return ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
    }

    private List<Message> decodeApplicationMessages(
        ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
        return ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
    }

    private List<Message> decodeApplicationMessages(byte[] frame) {
        return decodeApplicationMessages(wire.decodeApplicationPayload(frame));
    }

    private static String applicationContentType(List<Message> parts) {
        String contentType = ZLinkChannelContentTypeFrame.decode(parts);
        return contentType == null
            ? ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE
            : contentType;
    }

    private static long applicationPayloadBytes(List<Message> parts) {
        return parts.size() > 1
            ? parts.get(1).size()
            : parts.getFirst().size();
    }

    private static Optional<ZLinkStreamCodec> defaultApplicationStreamCodec(
        String contentType) {
        if (contentType == null) {
            return Optional.empty();
        }
        return "application/json".equals(contentType)
                || "application/zlink-framework-json-v1"
                    .equals(contentType)
            ? Optional.of(ZLinkStreamCodec.JSON)
            : Optional.empty();
    }

    private static ZLinkBackendRequestResult backendResult(
        RequestResult result) {
        //  BACKPRESSURED is preserved now that ZLinkBackendRequestResult has a
        //  matching member (it classifies to CapacityExceeded), instead of being
        //  collapsed to BUSY (which would be Unavailable).
        return ZLinkBackendRequestResult.valueOf(result.name());
    }

    private static ZLinkBackendRequestResult backendResult(int wireValue) {
        return ZLinkBackendRequestResult.fromWireTerminal(wireValue);
    }

    private UserSpotTerminalAdmission admitUserSpotOperation(
        UserSpotOperationKey key,
        byte[] fingerprint,
        long deadlineUnixMs) {
        long now = currentTimeMillis.getAsLong();
        synchronized (userSpotTerminals) {
            userSpotTerminals.entrySet().removeIf(
                entry -> entry.getValue().terminal.isDone()
                    && entry.getValue().retentionDeadlineUnixMs < now);
            UserSpotTerminalSlot existing = userSpotTerminals.get(key);
            if (existing != null) {
                return MessageDigest.isEqual(
                        existing.fingerprint, fingerprint)
                    ? new UserSpotTerminalAdmission(
                        existing, false, false, false)
                    : new UserSpotTerminalAdmission(
                        null, false, true, false);
            }
            if (deadlineUnixMs < now) {
                return new UserSpotTerminalAdmission(
                    null, false, false, true);
            }
            if (userSpotTerminals.size() >= USER_SPOT_TERMINAL_CAPACITY) {
                return new UserSpotTerminalAdmission(
                    null, false, false, false);
            }
            UserSpotTerminalSlot created = new UserSpotTerminalSlot(
                fingerprint.clone(), deadlineUnixMs);
            userSpotTerminals.put(key, created);
            return new UserSpotTerminalAdmission(
                created, true, false, false);
        }
    }

    private static byte[] fingerprint(byte[] canonicalCommand) {
        try {
            return MessageDigest.getInstance("SHA-256")
                .digest(canonicalCommand);
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static List<byte[]> cloneFrames(List<byte[]> frames, int start) {
        ArrayList<byte[]> copies = new ArrayList<>(frames.size() - start);
        for (int index = start; index < frames.size(); index++) {
            copies.add(frames.get(index).clone());
        }
        return Collections.unmodifiableList(copies);
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof CompletionException
            || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private record UserSpotOperationKey(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long operationHigh,
        long operationLow) {
    }

    private record ConnectionCandidate(
        RoutingId peer,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction) {
        private ConnectionCandidate {
            Objects.requireNonNull(peer, "peer");
            Objects.requireNonNull(direction, "direction");
        }
    }

    private record ReplyRelayPendingKey(
        RoutingId sourceNodeRid,
        ZLinkServiceRelocationWireCodec.RelocationId relocation,
        ZLinkServiceRelocationWireCodec.CoordinatorFence coordinator,
        ZLinkServiceRelocationWireCodec.Operation operation,
        long replyRouteId) {
        private static ReplyRelayPendingKey from(
            RoutingId sourceNodeRid,
            ZLinkServiceRelocationWireCodec.ReplyRelay relay) {
            return new ReplyRelayPendingKey(
                sourceNodeRid,
                relay.relocation(),
                relay.coordinator(),
                relay.operation(),
                relay.replyRouteId());
        }

        private static ReplyRelayPendingKey from(
            RoutingId sourceNodeRid,
            ZLinkServiceRelocationWireCodec.ReplyRelayAck ack) {
            return new ReplyRelayPendingKey(
                sourceNodeRid,
                ack.relocation(),
                ack.coordinator(),
                ack.operation(),
                ack.replyRouteId());
        }
    }

    private static final class ReplyRelayPending {
        private final ZLinkServiceRelocationWireCodec.ReplyRelay relay;
        private final ZLinkServiceRelocationWireCodec.RequestSourceFence
            expectedSource;
        private final CompletableFuture<byte[]> completion =
            new CompletableFuture<>();
        private volatile ScheduledFuture<?> timeoutTask;

        private ReplyRelayPending(
            ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            ZLinkServiceRelocationWireCodec.RequestSourceFence
                expectedSource) {
            this.relay = relay;
            this.expectedSource = expectedSource;
        }

        private void armTimeout(ScheduledFuture<?> task) {
            timeoutTask = task;
        }

        private void cancelTimeout() {
            ScheduledFuture<?> current = timeoutTask;
            if (current != null) {
                current.cancel(false);
            }
        }
    }

    private static final class UserSpotTerminalSlot {
        private final byte[] fingerprint;
        private final long retentionDeadlineUnixMs;
        private final CompletableFuture<UserSpotTerminalReply> terminal =
            new CompletableFuture<>();

        private UserSpotTerminalSlot(
            byte[] fingerprint,
            long deadlineUnixMs) {
            this.fingerprint = fingerprint;
            this.retentionDeadlineUnixMs =
                deadlineUnixMs > Long.MAX_VALUE
                    - USER_SPOT_TERMINAL_RETENTION_MS
                ? Long.MAX_VALUE
                : deadlineUnixMs + USER_SPOT_TERMINAL_RETENTION_MS;
        }
    }

    private record UserSpotTerminalAdmission(
        UserSpotTerminalSlot slot,
        boolean owner,
        boolean fingerprintMismatch,
        boolean expiredNew) {
    }

    private record UserSpotTerminalReply(
        int terminalResult,
        int failureCode,
        ZLinkServiceM6BWireCodec.UserSpotCreateTerminal create,
        Boolean closed,
        byte[] applicationFrame) {
        private UserSpotTerminalReply {
            applicationFrame = applicationFrame == null
                ? null : applicationFrame.clone();
        }

        @Override
        public byte[] applicationFrame() {
            return applicationFrame == null
                ? null : applicationFrame.clone();
        }
    }

    private record PeerIntent(
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity,
        long createdAtMs) {
    }

    private record PeerAdmissionExpectation(
        String endpoint,
        long lifecycleGeneration,
        String securityIdentity) {
        private PeerAdmissionExpectation {
            //  lifecycleGeneration is an opaque equality token: only zero
            //  (absent) is invalid, not a negative long (spec 13 §7.1).
            if (endpoint == null || endpoint.isBlank()
                || lifecycleGeneration == 0
                || securityIdentity == null
                || securityIdentity.isBlank()) {
                throw new IllegalArgumentException(
                    "peer admission expectation must be complete");
            }
        }
    }

    private record AutomaticNotRequiredPeer(
        String endpoint,
        long lifecycleGeneration,
        long observedAtMs) {
    }
}
