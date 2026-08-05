package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Comparator;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
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
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
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
final class ZLinkJavaRawMeshNode implements ZLinkInternalMeshNode {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
    private static final int PREFIX_BYTES = 5;
    private static final int MAX_COMPLETION_CONTROL_PARTS = 64;
    private static final long MAX_COMPLETION_CONTROL_BYTES = 256L * 1024;
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
    private final Map<RoutingId, ZLinkServiceNodeDescriptor.ObjectRole>
        admittedPeerObjectRoles = new ConcurrentHashMap<>();
    private final java.util.Set<RoutingId> notRequiredPeers =
        ConcurrentHashMap.newKeySet();
    private final java.util.Set<RoutingId> disconnectedPeers =
        ConcurrentHashMap.newKeySet();
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
    private final ZLinkServiceM6AWireCodec wire =
        new ZLinkServiceM6AWireCodec();
    private final ZLinkServiceM6BWireCodec statefulWire =
        new ZLinkServiceM6BWireCodec();
    private final ZLinkServiceLivenessRegistry liveness;
    private final ScheduledExecutorService deadlines =
        Executors.newSingleThreadScheduledExecutor(Thread.ofVirtual()
            .name("zlink-jvm-service-deadline-" + System.identityHashCode(this))
            .factory());
    private final ExecutorService applicationDispatch =
        Executors.newSingleThreadExecutor(Thread.ofVirtual()
            .name("zlink-jvm-application-dispatch-"
                + System.identityHashCode(this))
            .factory());
    private final ZLinkServiceOperationRegistry operations =
        new ZLinkServiceOperationRegistry(deadlines);
    private final java.util.concurrent.ConcurrentLinkedQueue<MonitorEvent>
        monitorEvents = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private final Map<RoutingId, String> connectionIds =
        new ConcurrentHashMap<>();
    private final Map<RoutingId, String> admissionControlReadyConnections =
        new ConcurrentHashMap<>();
    private final Map<ConnectionCandidate,
        java.util.concurrent.ConcurrentLinkedQueue<String>>
        pendingConnectionIds = new ConcurrentHashMap<>();
    private final Map<String, java.util.concurrent.ConcurrentLinkedQueue<String>>
        monitorConnectionIds = new ConcurrentHashMap<>();
    private final Map<RoutingId, Long> nextAnnouncementNanos =
        new ConcurrentHashMap<>();
    private volatile RoutingId routingId;
    private volatile String bindEndpoint;
    private volatile RouterSocket router;
    private volatile SocketMonitor rawMonitor;
    private volatile MeshNodeState state = MeshNodeState.CREATED;
    private volatile Consumer<ZLinkMeshDispatchRecord> receiver =
        ZLinkMeshDispatchRecord::close;
    private volatile ZLinkMeshApplicationReceiver applicationReceiver;
    private volatile ZLinkInboundDispatchBudget applicationDispatchBudget;
    private volatile ZLinkJavaRawSpotNode spotNode;
    private volatile ExecutorService pump;
    private volatile long mailboxMessageBudget = 4096;
    private volatile int placementWeight = 100;
    private volatile ZLinkServiceNodeDescriptor.ObjectRole objectRole =
        ZLinkServiceNodeDescriptor.ObjectRole.NONE;
    private volatile ZLinkServiceMailbox mailbox;
    private volatile ZLinkServiceTopologyRegistry topology;
    private volatile ZLinkServiceNodeDescriptor localDescriptor;
    private volatile ZLinkInternalMeshNode.UserSpotOperationHandler
        userSpotOperationHandler;
    private volatile ZLinkInternalMeshNode.ActorCreateOperationHandler
        actorCreateOperationHandler;
    private volatile ZLinkInternalMeshNode.PeerAuthorityResolver
        peerAuthorityResolver;
    private volatile ZLinkInternalMeshNode.PeerAuthorityFence
        localAuthorityFence;
    private volatile ZLinkInternalMeshNode.RelocationControlHandler
        relocationControlHandler;
    private volatile ZLinkInternalMeshNode.CanonicalRelocationControlHandler
        canonicalRelocationControlHandler;
    private volatile ZLinkInternalMeshNode.MessageFollowHandler
        messageFollowHandler;
    private volatile Function<String, Optional<ZLinkStreamCodec>>
        applicationStreamCodecResolver =
            ZLinkJavaRawMeshNode::defaultApplicationStreamCodec;
    private volatile ZLinkInternalMeshNode.RelocationReplyRelayHandler
        relocationReplyRelayHandler;
    private final Map<ReplyRelayPendingKey, ReplyRelayPending>
        pendingReplyRelays = new ConcurrentHashMap<>();
    private final java.util.concurrent.ConcurrentLinkedQueue<
        InfrastructureSend> pendingInfrastructureSends =
            new java.util.concurrent.ConcurrentLinkedQueue<>();
    private final ZLinkJavaAdmissionControlRetryQueue
        pendingAdmissionControls =
            new ZLinkJavaAdmissionControlRetryQueue();
    private final AtomicBoolean admissionControlRetryReady =
        new AtomicBoolean();
    private volatile ZLinkInternalMeshNode.SessionRelocationRouteHandler
        sessionRelocationRouteHandler;
    private final Map<UserSpotOperationKey, UserSpotTerminalSlot>
        userSpotTerminals = new ConcurrentHashMap<>();

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
        this.port = new ZLinkJavaRawServicePort(
            context,
            ZLinkJavaRawMeshNode::shouldUseCompletionControl);
        this.currentTimeMillis = java.util.Objects.requireNonNull(
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
        peerAuthorityResolver = java.util.Objects.requireNonNull(
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
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
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
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long requestId,
        String packetName,
        java.util.Map<String, String> metadata,
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
        java.util.Map<String, String> metadata) {
        if (metadata == null || metadata.isEmpty()) {
            return new byte[0];
        }
        if (metadata.size() > 255) {
            throw new IllegalArgumentException(
                "accepted metadata entry count exceeds u8");
        }
        try {
            java.io.ByteArrayOutputStream bytes =
                new java.io.ByteArrayOutputStream();
            java.io.DataOutputStream output =
                new java.io.DataOutputStream(bytes);
            output.writeByte(1);
            output.writeByte(metadata.size());
            for (var entry : metadata.entrySet().stream()
                .sorted(java.util.Map.Entry.comparingByKey())
                .toList()) {
                byte[] key = entry.getKey().getBytes(
                    java.nio.charset.StandardCharsets.UTF_8);
                byte[] value = entry.getValue().getBytes(
                    java.nio.charset.StandardCharsets.UTF_8);
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
        } catch (java.io.IOException impossible) {
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
        systems.zlink.framework.locations.ZLinkMeshNodeObjectRole role) {
        requireCreated();
        objectRole = switch (java.util.Objects.requireNonNull(role, "role")) {
            case NONE -> ZLinkServiceNodeDescriptor.ObjectRole.NONE;
            case CLIENT -> ZLinkServiceNodeDescriptor.ObjectRole.CLIENT;
            case SERVER -> ZLinkServiceNodeDescriptor.ObjectRole.SERVER;
        };
    }

    @Override
    public void setRoutingId(RoutingId value) {
        requireCreated();
        routingId = java.util.Objects.requireNonNull(value, "routingId");
    }

    @Override
    public void setMailboxMessageBudget(long value) {
        if (value < 0) {
            throw new IllegalArgumentException("mailbox budget must not be negative");
        }
        mailboxMessageBudget = value == 0 ? 4096 : value;
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
                mailboxMessageBudget,
                64L * 1024 * 1024,
                1024,
                8L * 1024 * 1024);
            state = MeshNodeState.STARTED;
            localDescriptor = descriptor(
                lifecycle,
                2,
                descriptorChannels,
                ZLinkServiceNodeDescriptor.State.SERVING);
            topology.publishLocal(localDescriptor);
            state = MeshNodeState.READY;
            opened.setSendReadyHandler(this::notifyAdmissionReadyPeers);
            opened.setCompletionControlHandler(
                this::dispatchCompletionControl);
            rawMonitor = port.openMonitor(
                opened,
                MonitorEventType.CONNECTION_READY,
                MonitorEventType.DISCONNECTED,
                MonitorEventType.CLOSED);
            rawMonitor.onEvent(monitorEvents::add);
            startPump();
        } catch (RuntimeException failure) {
            state = MeshNodeState.ERROR;
            throw failure;
        }
    }

    @Override
    public long connectPeer(String endpoint) {
        return connectPeer(endpoint, null);
    }

    @Override
    public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
        return connectPeer(
            endpoint,
            expectedRoutingId,
            0,
            expectedRoutingId == null
                ? null
                : expectedRoutingId.toString());
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
        if (expectedLifecycleGeneration < 0) {
            throw new IllegalArgumentException(
                "expected lifecycle generation must not be negative");
        }
        current.connect(endpoint);
        long intent = nextIntent.getAndIncrement();
        peerIntents.put(
            intent,
            new PeerIntent(
                endpoint,
                expectedRoutingId,
                expectedLifecycleGeneration,
                expectedSecurityIdentity,
                System.currentTimeMillis()));
        if (expectedRoutingId != null) {
            nextAnnouncementNanos.put(expectedRoutingId, 0L);
        }
        return intent;
    }

    @Override
    public void removePeerConnection(long connectionIntentId) {
        PeerIntent removed = peerIntents.remove(connectionIntentId);
        if (removed != null && router != null) {
            if (removed.expectedRoutingId() != null) {
                admittedPeerChannels.remove(removed.expectedRoutingId());
                admittedPeerObjectRoles.remove(removed.expectedRoutingId());
                notRequiredPeers.remove(removed.expectedRoutingId());
                disconnectedPeers.remove(removed.expectedRoutingId());
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
                pendingAdmissionControls.removeTarget(
                    removed.expectedRoutingId());
            }
            router.disconnect(removed.endpoint());
        }
    }

    @Override
    public void observePeerAdmissionExpectation(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration,
        String securityIdentity) {
        peerAdmissionExpectations.put(
            java.util.Objects.requireNonNull(peerRid, "peerRid"),
            new PeerAdmissionExpectation(
                endpoint,
                lifecycleGeneration,
                securityIdentity));
    }

    @Override
    public void forgetPeerAdmissionExpectation(RoutingId peerRid) {
        peerAdmissionExpectations.remove(peerRid);
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
        return java.util.stream.Stream.concat(
            java.util.stream.Stream.concat(
                manualPeers.stream(),
                automaticPeers.stream()),
            admittedInboundPeers.stream()).toList();
    }

    @Override
    public void markPeerConnectionNotRequired(
        RoutingId peerRid,
        String endpoint,
        long lifecycleGeneration) {
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
        receiver = java.util.Objects.requireNonNull(value, "receiver");
    }

    @Override
    public void setApplicationReceiver(ZLinkMeshApplicationReceiver value) {
        applicationReceiver =
            java.util.Objects.requireNonNull(value, "applicationReceiver");
        ZLinkJavaRawSpotNode current = spotNode;
        if (current != null) {
            current.setApplicationReceiver(value);
        }
    }

    @Override
    public void setApplicationDispatchBudget(
        ZLinkInboundDispatchBudget value) {
        applicationDispatchBudget =
            java.util.Objects.requireNonNull(value, "applicationDispatchBudget");
    }

    ZLinkInboundDispatchBudget applicationDispatchBudget() {
        return applicationDispatchBudget;
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

    RoutingId routingId() {
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
            .map(peer -> liveness.isReady(
                peerRoutingId, peer.connectionId()))
            .orElse(false);
    }

    private boolean isReadyPeer(
        ZLinkServiceTopologyRegistry.Peer peer) {
        RoutingId peerRoutingId = peer.descriptor().nodeRoutingId();
        return peer.connectionId().equals(
                admissionControlReadyConnections.get(peerRoutingId))
            && liveness.isReady(peerRoutingId, peer.connectionId());
    }

    long lifecycleGeneration() {
        ZLinkServiceNodeDescriptor current = localDescriptor;
        if (current == null) {
            throw new IllegalStateException("raw MeshNode is not started");
        }
        return current.lifecycleGeneration();
    }

    long bindingGenerationSeed() {
        ZLinkServiceNodeDescriptor current = localDescriptor;
        return current == null
            ? preStartBindingGenerationSeed
            : current.lifecycleGeneration();
    }

    boolean sendNode(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        boolean request,
        Long correlation) {
        if (topology == null || topology.peer(target).isEmpty()) {
            return false;
        }
        List<byte[]> frames = new ArrayList<>();
        int flags = metadata == null || metadata.length == 0
            ? 0
            : ServiceWireConstants.FLAG_METADATA;
        frames.add(request
            ? wire.encodeNodeRequestHeader(
                java.util.Objects.requireNonNull(correlation, "correlation"),
                flags)
            : wire.encodeNodeSendHeader(flags));
        if (flags != 0) {
            frames.add(metadata.clone());
        }
        frames.add(wire.encodeApplicationPayload(applicationPayload(parts)));
        return port.send(requireStarted(), target, frames);
    }

    boolean sendChannel(
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
            return false;
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

    void publishLogicalMulticast(
        ZLinkJavaRawSpot source,
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts) {
        String selectedChannel = requireChannel(channelName);
        ZLinkJavaRawSpotNode currentSpots =
            (ZLinkJavaRawSpotNode) spotNode();
        streamTrace("logical-multicast-publish channel=" + selectedChannel
            + " topic=" + topic
            + " sourceSpot=" + (source == null ? "none" : source.spotId())
            + " peerCount=" + (topology == null ? 0 : topology.peers().size()));
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
        streamTrace("logical-multicast-targets channel=" + selectedChannel
            + " targets=" + targets.stream()
                .map(peer -> peer.descriptor().nodeRoutingId().toString())
                .sorted()
                .toList());
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
        for (ZLinkServiceTopologyRegistry.Peer target : targets) {
            boolean sent = port.send(
                requireStarted(),
                target.descriptor().nodeRoutingId(),
                frames);
            streamTrace("logical-multicast-send target="
                + target.descriptor().nodeRoutingId()
                + " sent=" + sent
                + " channel=" + selectedChannel
                + " topic=" + topic);
        }
    }

    boolean requestNode(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        Duration timeout) {
        return request(
            target,
            metadata,
            parts,
            callback,
            timeout,
            null);
    }

    boolean requestChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        Duration timeout) {
        String selectedChannel = requireChannel(channelName);
        Optional<RoutingId> target = topology.selectChannel(
                selectedChannel, this::isReadyPeer)
            .map(peer -> peer.descriptor().nodeRoutingId());
        streamTrace("request-channel-select channel=" + selectedChannel
            + " target=" + target.map(RoutingId::toString).orElse("none")
            + " peerCount=" + (topology == null ? 0 : topology.peers().size()));
        boolean submitted;
        try {
            submitted = target.isPresent() && request(
                target.orElseThrow(),
                metadata,
                parts,
                callback,
                timeout,
                selectedChannel);
        } catch (RuntimeException failure) {
            streamTrace("request-channel-submit-failed channel="
                + selectedChannel
                + " target=" + target.map(RoutingId::toString).orElse("none")
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()));
            throw failure;
        }
        streamTrace("request-channel-submit channel=" + selectedChannel
            + " target=" + target.map(RoutingId::toString).orElse("none")
            + " submitted=" + submitted);
        return submitted;
    }

    Optional<Integer> classifyChannelTarget(String channelName) {
        String selectedChannel = requireChannel(channelName);
        ZLinkServiceTopologyRegistry currentTopology = topology;
        return currentTopology != null
                && currentTopology.hasSelectableChannel(
                    selectedChannel, ignored -> true)
            ? Optional.empty()
            : Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
    }

    boolean sendSpot(
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
            streamTrace("send spot reject source=" + sourceSpotId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " reason=" + (peer.isEmpty()
                    ? "missing-peer"
                    : "peer-not-ready-or-missing-authority"));
            return false;
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
        return port.send(requireStarted(), targetNodeRid, frames);
    }

    boolean requestSpot(
        String sourceSpotId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
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
            streamTrace("request spot reject source=" + sourceSpotId
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " reason=" + (peer.isEmpty()
                    ? "missing-peer"
                    : "peer-not-ready-or-missing-authority"));
            return false;
        }
        java.util.Objects.requireNonNull(callback, "callback");
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
        operation.completion().whenComplete((reply, failure) -> {
            if (failure == null) {
                callback.handle(reply);
                return;
            }
            callback.handle(new ZLinkBackendReceived(
                failure instanceof java.util.concurrent.TimeoutException
                    ? ZLinkBackendRequestResult.TIMED_OUT
                    : ZLinkBackendRequestResult.INTERNAL_ERROR,
                Optional.of(targetNodeRid),
                Optional.of(targetSpotId),
                Optional.of(correlation),
                List.of()));
        });
        boolean submitted = port.request(
            requireStarted(),
            targetNodeRid,
            frames,
            timeout,
            (result, replyFrames) -> completeSpotRequest(
                operation.id(),
                targetNodeRid,
                targetSpotId,
                correlation,
                result,
                replyFrames));
        if (!submitted) {
            operations.discard(operation.id());
        }
        return submitted;
    }

    private void completeSpotRequest(
        UUID operationId,
        RoutingId targetNodeRid,
        String targetSpotId,
        long correlation,
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
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

    boolean sendActor(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
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
            streamTrace("send actor reject actor=" + actorSummary(actor)
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration);
            return false;
        }
        if (!isReadyPeer(peer.orElseThrow())) {
            streamTrace("send actor reject actor=" + actorSummary(actor)
                + " reason=peer-not-ready");
            return false;
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
        boolean accepted;
        try {
            accepted = port.send(requireStarted(), actor.nodeRid(), frames);
        } catch (ZlinkSubmitException failure) {
            if (failure.getResult() == SubmitResult.NOT_CONNECTED
                && isReadyPeer(actor.nodeRid())) {
                accepted = false;
            } else {
                throw failure;
            }
        }
        streamTrace("send actor " + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(actor));
        return accepted;
    }

    boolean sendBoundActor(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
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
            streamTrace("send bound actor reject actor="
                + actorSummary(actor) + " sourceSession=" + sourceSessionRid
                + " binding=" + sourceBindingGeneration
                + " sequence=" + sourceSessionSequence
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration);
            return false;
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
        boolean accepted = port.send(
            requireStarted(), actor.nodeRid(), frames);
        streamTrace("send bound actor "
            + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(actor)
            + " sourceSession=" + sourceSessionRid
            + " binding=" + sourceBindingGeneration
            + " sequence=" + sourceSessionSequence);
        return accepted;
    }

    boolean requestBoundActor(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long streamRequestSequence,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return requestBoundActor(
            actor,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            streamRequestSequence,
            parts,
            Duration.ofSeconds(30),
            reply,
            ignored -> { });
    }

    boolean requestBoundActor(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long streamRequestSequence,
        List<Message> parts,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        return requestBoundActor(
            actor,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            streamRequestSequence,
            parts,
            Duration.ofSeconds(30),
            reply,
            failure);
    }

    private boolean requestBoundActor(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long streamRequestSequence,
        List<Message> parts,
        Duration timeout,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
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
            streamTrace("request bound actor reject actor="
                + actorSummary(actor) + " sourceSession=" + sourceSessionRid
                + " binding=" + sourceBindingGeneration
                + " sequence=" + sourceSessionSequence
                + " requestSequence=" + streamRequestSequence
                + " reason=missing-peer-or-authority peer=" + peer.isPresent()
                + " authority=" + authorityOwnerGeneration);
            return false;
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
        boolean accepted = port.request(
            requireStarted(),
            actor.nodeRid(),
            frames,
            timeout,
            (result, replyFrames) -> {
                if (result
                    != systems.zlink.contracts.sockets.RequestResult.OK) {
                    failure.accept(new ZlinkRequestException(result));
                    return;
                }
                try {
                    if (replyFrames.isEmpty() || replyFrames.size() > 2) {
                        failure.accept(new IllegalStateException(
                            "invalid bound Actor request reply frame count"));
                        return;
                    }
                    ZLinkServiceM6AWireCodec.Reply response =
                        wire.decodeReplyHeader(replyFrames.getFirst());
                    if (response.correlation() != correlation
                        || (response.terminalResult() == 0)
                            != (replyFrames.size() == 2)) {
                        failure.accept(new IllegalStateException(
                            "bound Actor request reply terminal mismatch"));
                        return;
                    }
                    if (response.terminalResult() != 0) {
                        failure.accept(new ZlinkRequestException(
                            systems.zlink.contracts.sockets.RequestResult
                                .fromValue(response.terminalResult())));
                        return;
                    }
                    List<Message> replyParts =
                        decodeApplicationMessages(replyFrames.get(1));
                    try {
                        reply.accept(replyParts);
                    } catch (RuntimeException callbackFailure) {
                        replyParts.forEach(Message::close);
                        throw callbackFailure;
                    }
                } catch (RuntimeException invalid) {
                    failure.accept(invalid);
                }
            });
        streamTrace("request bound actor "
            + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(actor)
            + " sourceSession=" + sourceSessionRid
            + " binding=" + sourceBindingGeneration
            + " sequence=" + sourceSessionSequence
            + " requestSequence=" + streamRequestSequence);
        return accepted;
    }

    CompletionStage<List<Message>> requestBoundActorAsync(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
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
        CompletableFuture<List<Message>> completion = new CompletableFuture<>();
        boolean accepted = requestBoundActor(
            actor,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            streamRequestSequence,
            parts,
            timeout,
            reply -> {
                if (!completion.complete(reply)) {
                    reply.forEach(Message::close);
                }
            },
            failure -> completion.completeExceptionally(failure));
        if (!accepted) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED));
        }
        long timeoutNanos;
        try {
            timeoutNanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            timeoutNanos = Long.MAX_VALUE;
        }
        return completion.orTimeout(timeoutNanos, TimeUnit.NANOSECONDS);
    }

    private void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] mesh=" + meshName
                + " rid=" + routingId + " " + message);
        }
    }

    private static String actorSummary(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor) {
        return actor == null
            ? "null"
            : actor.actorId() + "@" + actor.nodeRid()
                + "/g=" + actor.generation();
    }

    boolean sendInstanceSpot(
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
            return false;
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
        return port.send(requireStarted(), route.targetNodeRid(), frames);
    }

    @Override
    public CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts) {
        return sendInstanceSpot(
            route, stableType, sourceSpotId, metadata, parts)
            ? CompletableFuture.completedFuture(null)
            : CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot send was not submitted"));
    }

    @Override
    public CompletionStage<Void> submitInstanceSpotSend(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        java.util.Objects.requireNonNull(route, "route");
        java.util.Objects.requireNonNull(parts, "parts");
        java.util.Objects.requireNonNull(timeout, "timeout");
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
        if (!closed.get() && (peer.isEmpty() || localDescriptor == null)) {
            long remainingNanos = deadlineNanos - System.nanoTime();
            if (remainingNanos > 0) {
                long delayNanos = Math.min(
                    TimeUnit.MILLISECONDS.toNanos(10), remainingNanos);
                return CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            delayNanos, TimeUnit.NANOSECONDS, deadlines))
                    .thenCompose(ignored -> submitInstanceSpotSendWhenConnected(
                        route,
                        stableType,
                        sourceSpotId,
                        metadata,
                        parts,
                        deadlineNanos));
            }
        }
        if (closed.get() || deadlineNanos - System.nanoTime() <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot send was not submitted"));
        }
        try {
            if (sendInstanceSpot(route, stableType, sourceSpotId, metadata, parts)) {
                return CompletableFuture.completedFuture(null);
            }
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Instance Spot send was not submitted"));
        }
        long delayNanos = Math.min(
            TimeUnit.MILLISECONDS.toNanos(10), remainingNanos);
        return CompletableFuture.supplyAsync(
                () -> null,
                CompletableFuture.delayedExecutor(
                    delayNanos, TimeUnit.NANOSECONDS, deadlines))
            .thenCompose(ignored -> submitInstanceSpotSendWhenConnected(
                route,
                stableType,
                sourceSpotId,
                metadata,
                parts,
                deadlineNanos));
    }

    @Override
    public CompletionStage<List<Message>> requestInstanceSpot(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route,
        String stableType,
        String sourceSpotId,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        java.util.Objects.requireNonNull(route, "route");
        java.util.Objects.requireNonNull(parts, "parts");
        java.util.Objects.requireNonNull(timeout, "timeout");
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
            long remainingNanos = deadlineNanos - System.nanoTime();
            if (!closed.get() && remainingNanos > 0) {
                long delayNanos = Math.min(
                    TimeUnit.MILLISECONDS.toNanos(10), remainingNanos);
                return CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            delayNanos, TimeUnit.NANOSECONDS, deadlines))
                    .thenCompose(ignored -> requestInstanceSpotWhenConnected(
                        route,
                        stableType,
                        sourceSpotId,
                        metadata,
                        parts,
                        deadlineNanos));
            }
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
        AtomicBoolean terminal = new AtomicBoolean();
        boolean submitted = port.request(
            requireStarted(),
            route.targetNodeRid(),
            frames,
            remainingTimeout,
            (result, replyFrames) -> {
                if (!terminal.compareAndSet(false, true)) {
                    return;
                }
                if (result != systems.zlink.contracts.sockets.RequestResult.OK
                    || replyFrames.isEmpty()) {
                    streamTrace("instance request reply failed target="
                        + route.targetSpotId()
                        + " targetNode=" + route.targetNodeRid()
                        + " objectGeneration=" + route.objectGeneration()
                        + " ownerGeneration="
                        + route.authorityOwnerGeneration()
                        + " result=" + result
                        + " frames=" + replyFrames.size());
                    operations.completeExceptionally(
                        operation.id(),
                        new IllegalStateException(
                            "remote Instance Spot request failed: " + result));
                    return;
                }
                try {
                    var header = wire.decodeReplyHeader(replyFrames.getFirst());
                    streamTrace("instance request reply target="
                        + route.targetSpotId()
                        + " targetNode=" + route.targetNodeRid()
                        + " objectGeneration=" + route.objectGeneration()
                        + " ownerGeneration="
                        + route.authorityOwnerGeneration()
                        + " correlation=" + correlation
                        + " replyCorrelation=" + header.correlation()
                        + " terminal=" + header.terminalResult()
                        + " frames=" + replyFrames.size());
                    if (header.correlation() != correlation
                        || header.terminalResult() != 0
                        || replyFrames.size() != 2) {
                        throw new IllegalStateException(
                            "remote Instance Spot request was rejected");
                    }
                    var payload = wire.decodeApplicationPayload(
                        replyFrames.get(1));
                    List<Message> replyParts =
                        decodeApplicationMessages(payload);
                    if (!operations.complete(operation.id(), replyParts)) {
                        replyParts.forEach(Message::close);
                    }
                } catch (RuntimeException failure) {
                    operations.completeExceptionally(
                        operation.id(), failure);
                }
            });
        if (!submitted && terminal.compareAndSet(false, true)) {
            operations.completeExceptionally(
                operation.id(),
                new IllegalStateException(
                    "remote Instance Spot request was not submitted"));
        }
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
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
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
                new IllegalStateException("remote Actor route is not connected"));
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
        boolean submitted = port.request(
            requireStarted(),
            actor.nodeRid(),
            frames,
            timeout,
            (result, replyFrames) -> completeActorRequest(
                operation.id(),
                actor,
                correlation,
                result,
                replyFrames));
        if (!submitted) {
            operations.complete(
                operation.id(),
                new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.NOT_CONNECTED,
                    Optional.of(actor.nodeRid()),
                    Optional.empty(),
                    Optional.of(correlation),
                    List.of()));
        }
        return operation.completion().thenCompose(received -> {
            if (received.result() != ZLinkBackendRequestResult.OK) {
                received.close();
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor request failed: " + received.result()));
            }
            return CompletableFuture.completedFuture(received.parts());
        });
    }

    CompletionStage<Void> bindRemoteStreamSession(
        RoutingId sessionRid,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        long authorityOwnerGeneration,
        long bindingGeneration,
        boolean active,
        Duration timeout) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null ? Optional.empty() : topology.peer(actor.nodeRid());
        long ownerLeaseGeneration =
            ((ZLinkJavaRawSpotNode) spotNode())
                .actorAuthorityOwnerLeaseGeneration(actor);
        if (peer.isEmpty()
            || actor.generation() <= 0
            || bindingGeneration <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Actor binding route is not connected"));
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
        AtomicBoolean terminal = new AtomicBoolean();
        boolean submitted = port.request(
            requireStarted(),
            actor.nodeRid(),
            List.of(frame),
            timeout,
            (result, replyFrames) -> {
                if (!terminal.compareAndSet(false, true)) {
                    return;
                }
                if (result
                    != systems.zlink.contracts.sockets.RequestResult.OK) {
                    completion.completeExceptionally(
                        new IllegalStateException(
                            "bound session binding transport failed: "
                                + result));
                    return;
                }
                try {
                    if (replyFrames.size() != 1) {
                        throw new IllegalArgumentException(
                            "bound session binding reply frame count");
                    }
                    ZLinkServiceM6AWireCodec.Reply reply =
                        wire.decodeReplyHeader(replyFrames.getFirst());
                    if (reply.correlation() != correlation
                        || reply.terminalResult() != 0
                        || reply.failureCode() != 0) {
                        throw new IllegalArgumentException(
                            "bound session binding was rejected");
                    }
                    completion.complete(null);
                } catch (RuntimeException failure) {
                    completion.completeExceptionally(failure);
                }
            });
        if (!submitted && terminal.compareAndSet(false, true)) {
            completion.completeExceptionally(
                new IllegalStateException(
                    "bound session binding was not submitted"));
        }
        return completion;
    }

    @Override
    public void setUserSpotOperationHandler(
        ZLinkInternalMeshNode.UserSpotOperationHandler handler) {
        userSpotOperationHandler =
            java.util.Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setActorCreateOperationHandler(
        ZLinkInternalMeshNode.ActorCreateOperationHandler handler) {
        actorCreateOperationHandler =
            java.util.Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setApplicationStreamCodecResolver(
        Function<String, Optional<ZLinkStreamCodec>> resolver) {
        applicationStreamCodecResolver =
            java.util.Objects.requireNonNull(resolver, "resolver");
    }

    @Override
    public void setRelocationControlHandler(
        ZLinkInternalMeshNode.RelocationControlHandler handler) {
        relocationControlHandler =
            java.util.Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestRelocationControl(
        RoutingId targetNodeRid,
        byte[] command,
        Duration timeout) {
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] payload = java.util.Objects.requireNonNull(
            command, "command").clone();
        java.util.Objects.requireNonNull(timeout, "timeout");
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
        CompletableFuture<byte[]> completion = new CompletableFuture<>();
        List<Message> parts = List.of(
            Message.from(RELOCATION_CONTROL_PACKET.getBytes(
                StandardCharsets.UTF_8)),
            Message.from(payload));
        boolean submitted;
        try {
            submitted = requestNode(
                targetNodeRid,
                new byte[0],
                parts,
                received -> {
                    try {
                        if (received.result()
                                != ZLinkBackendRequestResult.OK
                            || received.parts().size() != 1) {
                            completion.completeExceptionally(
                                new IllegalStateException(
                                    "remote relocation command failed: "
                                        + received.result()));
                            return;
                        }
                        completion.complete(
                            received.parts().getFirst().toByteArray());
                    } finally {
                        received.close();
                    }
                },
                timeout);
        } finally {
            parts.forEach(Message::close);
        }
        if (!submitted) {
            completion.completeExceptionally(new IllegalStateException(
                "remote relocation command was not submitted"));
        }
        return completion;
    }

    @Override
    public void setCanonicalRelocationControlHandler(
        ZLinkInternalMeshNode.CanonicalRelocationControlHandler handler) {
        canonicalRelocationControlHandler =
            java.util.Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendCanonicalRelocationControl(
        RoutingId targetNodeRid,
        byte[] command) {
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] record = java.util.Objects.requireNonNull(
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
                return java.util.Objects.requireNonNull(
                    handler.handle(routingId, record),
                    "canonical relocation handler returned null");
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
        requireStarted();
        pendingInfrastructureSends.add(new InfrastructureSend(
            targetNodeRid,
            List.of(record),
            () -> !closed.get(),
            ignored -> { }));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public void setMessageFollowHandler(
        ZLinkInternalMeshNode.MessageFollowHandler handler) {
        messageFollowHandler = java.util.Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<Void> sendMessageFollow(
        RoutingId targetNodeRid,
        ZLinkServiceMessageFollowWireCodec.Notice notice) {
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        byte[] record = new ZLinkServiceMessageFollowWireCodec().encode(
            java.util.Objects.requireNonNull(notice, "notice"));
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
        pendingInfrastructureSends.add(new InfrastructureSend(
            targetNodeRid,
            List.of(record),
            () -> !closed.get(),
            ignored -> { }));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public void setRelocationReplyRelayHandler(
        ZLinkInternalMeshNode.RelocationReplyRelayHandler handler) {
        relocationReplyRelayHandler = java.util.Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestRelocationReplyRelay(
        RoutingId sourceNodeRid,
        ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
        byte[] command33,
        List<byte[]> payload,
        Duration timeout) {
        java.util.Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        java.util.Objects.requireNonNull(expectedSource, "expectedSource");
        if (!expectedSource.nodeRid().equals(sourceNodeRid)) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "expected source fence differs from target RID"));
        }
        byte[] command = java.util.Objects.requireNonNull(
            command33, "command33").clone();
        List<byte[]> application = java.util.Objects.requireNonNull(
            payload, "payload").stream().map(byte[]::clone).toList();
        java.util.Objects.requireNonNull(timeout, "timeout");
        var relocationWire = new ZLinkServiceRelocationWireCodec();
        var relay = relocationWire.decodeReplyRelay(command);
        if (sourceNodeRid.equals(routingId)) {
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
            sourceNodeRid, relay);
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
                            new java.util.concurrent.TimeoutException(
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
        pendingInfrastructureSends.add(new InfrastructureSend(
            sourceNodeRid,
            frames,
            () -> pendingReplyRelays.get(key) == pending,
            failure -> {
                if (pendingReplyRelays.remove(key, pending)) {
                    pending.cancelTimeout();
                    pending.completion.completeExceptionally(failure);
                }
            }));
        return pending.completion;
    }

    @Override
    public void setSessionRelocationRouteHandler(
        ZLinkInternalMeshNode.SessionRelocationRouteHandler handler) {
        sessionRelocationRouteHandler = java.util.Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public CompletionStage<byte[]> requestSessionRelocationRoute(
        RoutingId sessionOwnerNodeRid,
        byte[] command44,
        Duration timeout) {
        java.util.Objects.requireNonNull(
            sessionOwnerNodeRid, "sessionOwnerNodeRid");
        byte[] record = java.util.Objects.requireNonNull(
            command44, "command44").clone();
        java.util.Objects.requireNonNull(timeout, "timeout");
        statefulWire.decodeSessionRelocationRoute(record);
        streamTrace("request session route target=" + sessionOwnerNodeRid
            + " local=" + sessionOwnerNodeRid.equals(routingId)
            + " timeout=" + timeout);
        if (sessionOwnerNodeRid.equals(routingId)) {
            var handler = sessionRelocationRouteHandler;
            if (handler == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "local Session relocation route handler is unavailable"));
            }
            return handler.handle(routingId, record).thenApply(reply -> {
                statefulWire.decodeSessionRelocationRouted(reply);
                streamTrace("request session route local ACK target="
                    + sessionOwnerNodeRid);
                return reply.clone();
            });
        }
        CompletableFuture<byte[]> completion = new CompletableFuture<>();
        boolean submitted = port.request(
            requireStarted(),
            sessionOwnerNodeRid,
            List.of(record),
            timeout,
            (result, reply) -> {
                streamTrace("request session route result target="
                    + sessionOwnerNodeRid + " result=" + result
                    + " replyFrames=" + reply.size());
                if (result != RequestResult.OK || reply.size() != 1) {
                    completion.completeExceptionally(
                        new ZlinkRequestException(result));
                    return;
                }
                try {
                    statefulWire.decodeSessionRelocationRouted(
                        reply.getFirst());
                    completion.complete(reply.getFirst().clone());
                } catch (RuntimeException invalid) {
                    completion.completeExceptionally(invalid);
                }
            });
        if (!submitted) {
            completion.completeExceptionally(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
        }
        streamTrace("request session route "
            + (submitted ? "accepted" : "rejected") + " target="
            + sessionOwnerNodeRid);
        return completion;
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
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        java.util.Objects.requireNonNull(intent, "intent");
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
            if (currentTimeMillis.getAsLong() < intent.deadlineUnixMs()) {
                return CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            10, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored ->
                        requestActorCreate(
                            targetNodeRid,
                            intent,
                            timeout,
                            operationHigh,
                            operationLow));
            }
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
        AtomicBoolean terminal = new AtomicBoolean();
        operation.completion().whenComplete((ignored, failure) ->
            terminal.compareAndSet(false, true));
        class SubmitAttempt implements Runnable {
            @Override
            public void run() {
                if (terminal.get() || closed.get()) {
                    return;
                }
                Optional<ZLinkServiceTopologyRegistry.Peer> currentPeer =
                    topology == null
                        ? Optional.empty()
                        : topology.peer(targetNodeRid);
                if (currentPeer.isEmpty() || localDescriptor == null) {
                    retry(new ZlinkRequestException(
                        RequestResult.NOT_CONNECTED));
                    return;
                }
                if (currentPeer.orElseThrow().descriptor()
                        .lifecycleGeneration()
                    != intent.reservation().targetNodeGeneration()) {
                    fail(new IllegalStateException(
                        "remote Actor create target lifecycle generation changed"));
                    return;
                }
                if (!isReadyPeer(currentPeer.orElseThrow())) {
                    retry(new ZlinkRequestException(
                        RequestResult.NOT_CONNECTED));
                    return;
                }
                if (currentTimeMillis.getAsLong() >= intent.deadlineUnixMs()) {
                    fail(new IllegalStateException(
                        "remote Actor create was not submitted before deadline"));
                    return;
                }
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
                boolean submitted;
                try {
                    submitted = port.request(
                        requireStarted(),
                        targetNodeRid,
                        List.of(statefulWire.encodeActorCreateHeader(command)),
                        timeout,
                        (result, replyFrames) -> {
                            if (result == RequestResult.NOT_CONNECTED
                                && currentTimeMillis.getAsLong()
                                    < intent.deadlineUnixMs()) {
                                retry(new ZlinkRequestException(result));
                                return;
                            }
                            if (!terminal.compareAndSet(false, true)) {
                                return;
                            }
                            completeActorCreate(
                                operation.id(),
                                attemptCorrelation,
                                result,
                                replyFrames);
                        });
                } catch (ZlinkSubmitException failure) {
                    if (isTransientSubmitFailure(failure)) {
                        retry(failure);
                        return;
                    }
                    fail(failure);
                    return;
                } catch (RuntimeException failure) {
                    fail(failure);
                    return;
                }
                if (!submitted) {
                    retry(new IllegalStateException(
                        "remote Actor create submit was backpressured"));
                }
            }

            private void retry(Throwable failure) {
                if (terminal.get() || currentTimeMillis.getAsLong()
                    >= intent.deadlineUnixMs()) {
                    fail(failure);
                    return;
                }
                try {
                    deadlines.schedule(this, 10, TimeUnit.MILLISECONDS);
                } catch (RuntimeException schedulingFailure) {
                    fail(schedulingFailure);
                }
            }

            private void fail(Throwable failure) {
                if (terminal.compareAndSet(false, true)) {
                    operations.completeExceptionally(operation.id(), failure);
                }
            }
        }
        new SubmitAttempt().run();
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
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        java.util.Objects.requireNonNull(intent, "intent");
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
            if (currentTimeMillis.getAsLong() < intent.deadlineUnixMs()) {
                return CompletableFuture.supplyAsync(
                        () -> null,
                        CompletableFuture.delayedExecutor(
                            10, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored ->
                        requestUserSpotCreate(
                            targetNodeRid,
                            intent,
                            timeout,
                            operationHigh,
                            operationLow));
            }
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
        AtomicBoolean terminal = new AtomicBoolean();
        operation.completion().whenComplete((ignored, failure) ->
            terminal.compareAndSet(false, true));
        submitRequestUntilAccepted(
            intent.deadlineUnixMs(),
            terminal,
            retryAttempt -> {
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
                return port.request(
                    requireStarted(),
                    targetNodeRid,
                    List.of(statefulWire.encodeUserSpotCreateHeader(command)),
                    timeout,
                    (result, replyFrames) -> {
                        if (result == RequestResult.NOT_CONNECTED
                            && currentTimeMillis.getAsLong()
                                < intent.deadlineUnixMs()
                            && !terminal.get()) {
                            try {
                                deadlines.schedule(
                                    retryAttempt,
                                    10,
                                    TimeUnit.MILLISECONDS);
                            } catch (RuntimeException schedulingFailure) {
                                if (terminal.compareAndSet(false, true)) {
                                    operations.completeExceptionally(
                                        operation.id(),
                                        schedulingFailure);
                                }
                            }
                            return;
                        }
                        if (!terminal.compareAndSet(false, true)) {
                            return;
                        }
                        completeUserSpotCreate(
                            operation.id(),
                            attemptCorrelation,
                            result,
                            replyFrames);
                    });
            },
            failure -> operations.completeExceptionally(operation.id(), failure));
        return operation.completion();
    }

    private void submitRequestUntilAccepted(
        long deadlineUnixMs,
        AtomicBoolean terminal,
        Function<Runnable, Boolean> submit,
        Consumer<Throwable> failureHandler) {
        class Attempt implements Runnable {
            private Throwable lastFailure;

            @Override
            public void run() {
                if (terminal.get() || closed.get()) {
                    return;
                }
                if (currentTimeMillis.getAsLong() >= deadlineUnixMs) {
                    fail(lastFailure == null
                        ? new IllegalStateException(
                            "service request was not submitted before deadline")
                        : lastFailure);
                    return;
                }
                try {
                    if (submit.apply(this)) {
                        return;
                    }
                    lastFailure = new IllegalStateException(
                        "service request submit was backpressured");
                } catch (ZlinkSubmitException submitFailure) {
                    if (!isTransientSubmitFailure(submitFailure)) {
                        fail(submitFailure);
                        return;
                    }
                    lastFailure = submitFailure;
                } catch (RuntimeException submitFailure) {
                    fail(submitFailure);
                    return;
                }
                if (currentTimeMillis.getAsLong() >= deadlineUnixMs) {
                    fail(lastFailure);
                    return;
                }
                try {
                    deadlines.schedule(this, 10, TimeUnit.MILLISECONDS);
                } catch (RuntimeException schedulingFailure) {
                    fail(schedulingFailure);
                }
            }

            private void fail(Throwable error) {
                if (terminal.compareAndSet(false, true)) {
                    failureHandler.accept(error);
                }
            }
        }
        new Attempt().run();
    }

    private static boolean isTransientSubmitFailure(
        ZlinkSubmitException failure) {
        return failure.getResult() == SubmitResult.BACKPRESSURED
            || failure.getResult() == SubmitResult.NOT_CONNECTED
            || failure.getResult() == SubmitResult.NOT_ADMITTED;
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.UserSpotCloseResponse>
        requestUserSpotClose(
            RoutingId targetNodeRid,
            ZLinkInternalMeshNode.UserSpotCloseIntent intent,
            Duration timeout) {
        java.util.Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        java.util.Objects.requireNonNull(intent, "intent");
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
        AtomicBoolean terminal = new AtomicBoolean();
        boolean submitted = port.request(
            requireStarted(),
            targetNodeRid,
            List.of(statefulWire.encodeUserSpotCloseHeader(command)),
            timeout,
            (result, replyFrames) -> {
                if (!terminal.compareAndSet(false, true)) {
                    return;
                }
                completeUserSpotClose(
                    operation.id(),
                    correlation,
                    result,
                    replyFrames);
            });
        if (!submitted && terminal.compareAndSet(false, true)) {
            operations.completeExceptionally(
                operation.id(),
                new IllegalStateException(
                    "remote User Spot close was not submitted"));
        }
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
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote User Spot create transport failed: "
                        + result));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new IllegalArgumentException(
                    "invalid User Spot create reply frame count");
            }
            ZLinkServiceM6BWireCodec.UserSpotCreateReply reply =
                statefulWire.decodeUserSpotCreateReply(
                    frames.getFirst());
            if (reply.correlation() != correlation
                || reply.terminalResult() != 0
                || reply.failureCode() != 0
                || reply.success() == null
                || (reply.success().result()
                        == ZLinkServiceM6BWireCodec
                            .UserSpotCreateResult.EXISTING
                    && frames.size() != 1)) {
                throw new IllegalStateException(
                    "remote User Spot create was rejected");
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
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    private void completeActorCreate(
        UUID operationId,
        long correlation,
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote Actor create transport failed: " + result));
            return;
        }
        try {
            if (frames.isEmpty() || frames.size() > 2) {
                throw new IllegalArgumentException(
                    "invalid Actor create reply frame count");
            }
            var reply = statefulWire.decodeActorCreateReply(
                frames.getFirst(), meshName);
            if (reply.correlation() != correlation) {
                throw new IllegalArgumentException(
                    "Actor create correlation mismatch");
            }
            var terminal = reply.terminal();
            if (frames.size() == 2) {
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
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    private void completeUserSpotClose(
        UUID operationId,
        long correlation,
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
            operations.completeExceptionally(
                operationId,
                new IllegalStateException(
                    "remote User Spot close transport failed: "
                        + result));
            return;
        }
        try {
            if (frames.size() != 1) {
                throw new IllegalArgumentException(
                    "invalid User Spot close reply frame count");
            }
            ZLinkServiceM6BWireCodec.UserSpotCloseReply reply =
                statefulWire.decodeUserSpotCloseReply(
                    frames.getFirst());
            if (reply.correlation() != correlation
                || reply.terminalResult() != 0
                || reply.failureCode() != 0
                || reply.closed() == null) {
                throw new IllegalStateException(
                    "remote User Spot close was rejected");
            }
            operations.complete(
                operationId,
                new ZLinkInternalMeshNode.UserSpotCloseResponse(
                    reply.closed()));
        } catch (RuntimeException failure) {
            operations.completeExceptionally(operationId, failure);
        }
    }

    boolean sendBoundSession(
        ZLinkJavaRawSpotNode.RemoteStreamBinding binding,
        List<Message> parts) {
        Optional<ZLinkServiceTopologyRegistry.Peer> peer =
            topology == null
                ? Optional.empty()
                : topology.peer(binding.sessionOwnerNodeRid());
        long ownerLeaseGeneration = ((ZLinkJavaRawSpotNode) spotNode())
            .actorAuthorityOwnerLeaseGeneration(binding.actor());
        if (peer.isEmpty()
            || peer.orElseThrow().descriptor().lifecycleGeneration()
                != binding.sessionOwnerNodeGeneration()
            || !isReadyPeer(binding.sessionOwnerNodeRid())
            || localDescriptor == null
            || !((ZLinkJavaRawSpotNode) spotNode())
                .isCurrentBoundActor(binding.actor())
            || ((ZLinkJavaRawSpotNode) spotNode())
                .actorAuthorityOwnerGeneration(binding.actor())
                != binding.authorityOwnerGeneration()
            || ownerLeaseGeneration <= 0) {
            streamTrace("send bound session rejected actor="
                + actorSummary(binding.actor())
                + " session=" + binding.sessionRid()
                + " binding=" + binding.bindingGeneration()
                + " reason=route-or-binding-fence");
            return false;
        }
        List<byte[]> frames = List.of(
            statefulWire.encodeBoundSessionSendHeader(
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    binding.actor(),
                    localDescriptor.lifecycleGeneration(),
                    binding.authorityOwnerGeneration(),
                    ownerLeaseGeneration),
                binding.bindingGeneration()),
            wire.encodeApplicationPayload(applicationPayload(parts)));
        boolean accepted = port.send(
            requireStarted(),
            binding.sessionOwnerNodeRid(),
            frames);
        streamTrace("send bound session " + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(binding.actor())
            + " session=" + binding.sessionRid()
            + " binding=" + binding.bindingGeneration());
        return accepted;
    }

    private void completeActorRequest(
        UUID operationId,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor,
        long correlation,
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
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

    private boolean request(
        RoutingId target,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        Duration timeout,
        String channelName) {
        java.util.Objects.requireNonNull(callback, "callback");
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
            if (failure == null) {
                if (channelName != null) {
                    streamTrace("request-channel-complete channel="
                        + channelName + " target=" + target
                        + " result=" + reply.result());
                }
                callback.handle(reply);
                return;
            }
            if (channelName != null) {
                streamTrace("request-channel-complete channel="
                    + channelName + " target=" + target
                    + " result=" + (failure instanceof java.util.concurrent.TimeoutException
                        ? ZLinkBackendRequestResult.TIMED_OUT
                        : ZLinkBackendRequestResult.INTERNAL_ERROR));
            }
            callback.handle(new ZLinkBackendReceived(
                failure instanceof java.util.concurrent.TimeoutException
                    ? ZLinkBackendRequestResult.TIMED_OUT
                    : ZLinkBackendRequestResult.INTERNAL_ERROR,
                Optional.of(target),
                Optional.empty(),
                Optional.empty(),
                List.of()));
        });
        boolean submitted = port.request(
            requireStarted(),
            target,
            frames,
            timeout,
            (result, replyFrames) -> completeRequest(
                operation.id(),
                target,
                correlation,
                result,
                replyFrames));
        if (!submitted) {
            operations.discard(operation.id());
        }
        return submitted;
    }

    private void completeRequest(
        UUID operationId,
        RoutingId target,
        long correlation,
        systems.zlink.contracts.sockets.RequestResult result,
        List<byte[]> frames) {
        if (result != systems.zlink.contracts.sockets.RequestResult.OK) {
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
        java.util.Objects.requireNonNull(peerRoutingId, "peerRoutingId");
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
        admittedPeerChannels.put(peerRoutingId, Map.copyOf(validated));
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
        applicationDispatch.shutdown();
        awaitExecutorTermination(applicationDispatch);
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
        pendingInfrastructureSends.clear();
        pendingAdmissionControls.clear();
        admissionControlRetryReady.set(false);
        admittedPeerChannels.clear();
        connectionIds.clear();
        admissionControlReadyConnections.clear();
        pendingConnectionIds.clear();
        monitorConnectionIds.clear();
        operations.close();
        deadlines.shutdownNow();
        awaitExecutorTermination(deadlines);
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
                drainAdmissionControlRetries();
                announceExpectedPeers(now);
                tickLiveness(now);
                drainInfrastructureSends();
                ZLinkInboundDispatchBudget budget = applicationDispatchBudget;
                if (budget != null && !budget.canStartApplicationReceive()) {
                    java.util.concurrent.locks.LockSupport.parkNanos(
                        Duration.ofMillis(1).toNanos());
                    continue;
                }
                Optional<ZLinkJavaRawServicePort.Inbound> inbound;
                try {
                    inbound = port.receive(requireStarted());
                } catch (RuntimeException ignored) {
                    if (closed.get()) {
                        return;
                    }
                    // A transient transport receive failure must not stop the
                    // only service pump. Retry on the next pump iteration.
                    java.util.concurrent.locks.LockSupport.parkNanos(
                        Duration.ofMillis(1).toNanos());
                    continue;
                }
                if (inbound.isEmpty()) {
                    java.util.concurrent.locks.LockSupport.parkNanos(
                        Duration.ofMillis(1).toNanos());
                    continue;
                }
                dispatch(inbound.orElseThrow());
            }
        });
    }

    private void dispatchCompletionControl(
        RoutingId source,
        List<Message> parts) {
        try {
            int command = allowedCompletionControlCommand(parts);
            if (command < 0
                || !hasCurrentCompletionControlSource(source, command)) {
                return;
            }
            List<byte[]> frames =
                parts.stream().map(Message::toByteArray).toList();
            dispatch(new ZLinkJavaRawServicePort.Inbound(
                source,
                null,
                frames));
        } finally {
            parts.forEach(Message::close);
        }
    }

    private boolean hasCurrentCompletionControlSource(
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
                peer.descriptor().lifecycleGeneration() > 0
                    && peer.connectionId().equals(connectionIds.get(source)))
            .isPresent();
    }

    static int allowedCompletionControlCommand(List<Message> parts) {
        if (parts == null
            || parts.isEmpty()
            || parts.size() > MAX_COMPLETION_CONTROL_PARTS) {
            return -1;
        }
        long totalBytes = 0;
        for (Message part : parts) {
            if (part == null) {
                return -1;
            }
            totalBytes += part.size();
            if (totalBytes > MAX_COMPLETION_CONTROL_BYTES) {
                return -1;
            }
        }
        int command =
            allowedCompletionControlCommand(parts.getFirst().toByteArray());
        return command == ServiceWireConstants.COMMAND_REPLY_RELAY
                && parts.size() != 1
            ? -1
            : command;
    }

    private static boolean shouldUseCompletionControl(List<byte[]> frames) {
        if (frames == null
            || frames.isEmpty()
            || frames.size() > MAX_COMPLETION_CONTROL_PARTS) {
            return false;
        }
        long totalBytes = 0;
        for (byte[] frame : frames) {
            if (frame == null) {
                return false;
            }
            totalBytes += frame.length;
            if (totalBytes > MAX_COMPLETION_CONTROL_BYTES) {
                return false;
            }
        }
        int command = allowedCompletionControlCommand(frames.getFirst());
        return command >= 0
            && (command != ServiceWireConstants.COMMAND_REPLY_RELAY
                || frames.size() == 1);
    }

    private static int allowedCompletionControlCommand(byte[] head) {
        if (head == null
            || head.length < PREFIX_BYTES
            || head[0] != (byte) ServiceWireConstants.MAGIC_0
            || head[1] != (byte) ServiceWireConstants.MAGIC_1
            || head[2] != (byte) ServiceWireConstants.WIRE_MAJOR) {
            return -1;
        }
        int command = Byte.toUnsignedInt(head[3]);
        return isAllowedCompletionControlCommand(command) ? command : -1;
    }

    private static boolean isAllowedCompletionControlCommand(int command) {
        return command == ServiceWireConstants.COMMAND_HELLO
            || command == ServiceWireConstants.COMMAND_ADMIT
            || command == ServiceWireConstants.COMMAND_REJECT
            || command == ServiceWireConstants.COMMAND_UPDATE
            || command == ServiceWireConstants.COMMAND_LIVENESS_PROBE
            || command == ServiceWireConstants.COMMAND_LIVENESS_ACK
            || command == ServiceWireConstants.COMMAND_RELOCATION_READY
            || command == ServiceWireConstants.COMMAND_RELOCATION_ACK
            || command == ServiceWireConstants.COMMAND_REPLY_RELAY
            || command == ServiceWireConstants.COMMAND_RELOCATION_SEAL
            || command == ServiceWireConstants.COMMAND_RELOCATION_COMPLETE
            || command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE
            || command == ServiceWireConstants.COMMAND_RELOCATION_RESERVED
            || command == ServiceWireConstants.COMMAND_REPLY_RELAY_ACK;
    }

    private void dispatch(ZLinkJavaRawServicePort.Inbound inbound) {
        List<byte[]> frames = inbound.frames();
        if (frames.isEmpty() || frames.getFirst().length < PREFIX_BYTES) {
            return;
        }
        byte[] head = frames.getFirst();
        ZLinkServiceM6AWireCodec.Header header;
        try {
            header = wire.decodeHeader(head);
        } catch (RuntimeException invalid) {
            return;
        }
        int command = header.command();
        int flags = header.flags();
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
                    if (reason == 4) {
                        disconnectAdmitted(inbound.source());
                        admittedPeerObjectRoles.put(
                            inbound.source(),
                            ZLinkServiceNodeDescriptor.ObjectRole.CLIENT);
                        notRequiredPeers.add(inbound.source());
                        disconnectNotRequiredTransport(inbound.source());
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
        if (command == ServiceWireConstants.COMMAND_SPOT_SEND
            || command == ServiceWireConstants.COMMAND_SPOT_REQUEST) {
            dispatchSpot(inbound, flags);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_LOGICAL_MULTICAST) {
            dispatchLogicalMulticast(inbound, flags);
            return;
        }
        if (command == ServiceWireConstants.COMMAND_ACTOR_SEND
            || command == ServiceWireConstants.COMMAND_ACTOR_REQUEST) {
            dispatchActor(inbound, flags);
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
            streamTrace("channel-application-received kind=" + kind
                + " channel=" + channelName
                + " source=" + inbound.source()
                + " requestSequence=" + inbound.requestSequence()
                + " correlation=" + correlation);
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
        ZLinkInboundDispatchBudget.Lease dispatchLease = null;
        String contentType;
        try {
            ZLinkInboundDispatchBudget budget = applicationDispatchBudget;
            contentType = applicationContentType(messages);
            if (budget != null) {
                dispatchLease = budget.track(applicationPayloadBytes(messages));
            }
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
            dispatchLease);
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
            streamTrace("channel-application-mailbox-rejected channel="
                + channelName + " source=" + inbound.source()
                + " correlation=" + correlation
                + " mailbox=" + (currentMailbox != null));
            dispatch.close();
            return;
        }
        dispatchEnvelopes.put(envelopeId, dispatch);
        streamTrace("channel-application-mailbox-enqueued channel="
            + channelName + " source=" + inbound.source()
            + " correlation=" + correlation + " envelope=" + envelopeId);
        drainApplicationMailbox();
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
            completion = java.util.Objects.requireNonNull(
                handler.handle(inbound.source(), command.clone()),
                "relocation handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        AtomicBoolean terminal = new AtomicBoolean();
        completion.whenComplete((reply, failure) -> {
            if (!terminal.compareAndSet(false, true)) {
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
            CompletionStage<Void> completion =
                java.util.Objects.requireNonNull(
                    handler.handle(inbound.source(), command),
                    "canonical relocation handler returned null");
            completion.exceptionally(failure -> {
                return null;
            });
        } catch (RuntimeException ignored) {
            // Invalid or rejected maintenance records never enter an
            // application mailbox and have no request/reply terminal.
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
        List<byte[]> payload = inbound.frames().stream()
            .skip(1)
            .map(byte[]::clone)
            .toList();
        CompletionStage<byte[]> completion;
        try {
            completion = java.util.Objects.requireNonNull(
                handler.handle(inbound.source(), command33.clone(), payload),
                "relocation reply handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        completion.whenComplete((ack, failure) -> {
            if (failure != null || ack == null) {
                return;
            }
            try {
                relocationWire.decodeReplyRelayAck(ack);
                pendingInfrastructureSends.add(new InfrastructureSend(
                    inbound.source(),
                    List.of(ack.clone()),
                    () -> !closed.get(),
                    ignored -> { }));
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
            var peer = topology.peer(inbound.source()).orElseThrow();
            if (!ack.requestSource().nodeRid().equals(inbound.source())
                || ack.requestSource().nodeGeneration()
                    != peer.descriptor().lifecycleGeneration()) {
                return;
            }
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
            || inbound.requestSequence() == null
            || inbound.frames().size() != 1) {
            return;
        }
        byte[] command44 = inbound.frames().getFirst();
        try {
            statefulWire.decodeSessionRelocationRoute(command44);
        } catch (RuntimeException invalid) {
            return;
        }
        CompletionStage<byte[]> completion;
        try {
            completion = java.util.Objects.requireNonNull(
                handler.handle(inbound.source(), command44.clone()),
                "Session relocation route handler returned null");
        } catch (RuntimeException failure) {
            completion = CompletableFuture.failedFuture(failure);
        }
        completion.whenComplete((ack, failure) -> {
            if (failure != null || ack == null) {
                return;
            }
            try {
                statefulWire.decodeSessionRelocationRouted(ack);
                port.reply(
                    requireStarted(),
                    inbound.source(),
                    inbound.requestSequence(),
                    List.of(ack));
            } catch (RuntimeException invalid) {
                // Invalid ACKs never cross the infrastructure boundary.
            }
        });
    }

    private void dispatchSpot(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1) {
            return;
        }
        ZLinkServiceM6BWireCodec.SpotMessage header;
        try {
            header = statefulWire.decodeSpotHeader(frames.getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
        } catch (RuntimeException invalidPayload) {
            replySpotFailure(inbound, header, 104, 12);
            return;
        }
        if (header.request() != (inbound.requestSequence() != null)
            || !header.target().targetNodeRid().equals(routingId)
            || localDescriptor == null
            || header.target().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replySpotFailure(inbound, header, 102, 1);
            return;
        }
        byte[] metadata = payloadOffset == 2
            ? frames.get(1).clone()
            : new byte[0];
        resolveAcceptedAuthorities(inbound).whenComplete((authorities, failure) -> {
            if (failure != null || authorities.isEmpty()) {
                replySpotFailure(inbound, header, 107, 33);
                return;
            }
            AcceptedAuthorities acceptedAuthorities =
                authorities.orElseThrow();
            List<Message> messages = null;
            ZLinkInboundDispatchBudget.Lease dispatchLease = null;
            String contentType;
            byte[] acceptedRecord;
            try {
                messages = decodeApplicationMessages(payload);
                contentType = applicationContentType(messages);
                if (applicationDispatchBudget != null) {
                    dispatchLease = applicationDispatchBudget.track(
                        applicationPayloadBytes(messages));
                }
                acceptedRecord = ZLinkServiceFrozenRecordCodec.encodeSpot(
                    acceptedAuthorities.source(),
                    acceptedAuthorities.targetOwner(),
                    header,
                    metadata,
                    wire.encodeApplicationPayload(payload));
            } catch (RuntimeException invalidPayload) {
                if (messages != null) {
                    messages.forEach(Message::close);
                }
                if (dispatchLease != null) {
                    dispatchLease.close();
                }
                replySpotFailure(inbound, header, 104, 12);
                return;
            }
            AtomicBoolean terminal = new AtomicBoolean();
            boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
                .enqueueRemoteSpot(
                    acceptedAuthorities.source(),
                    header,
                    metadata,
                    acceptedRecord,
                    messages,
                    contentType,
                    dispatchLease,
                    replyParts -> {
                        if (!terminal.compareAndSet(false, true)) {
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
                    });
            if (!accepted) {
                messages.forEach(Message::close);
                replySpotFailure(inbound, header, 102, 1);
            }
        });
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
            streamTrace("logical-multicast-receive channel="
                + header.channelName()
                + " topic=" + header.topic()
                + " source=" + inbound.source()
                + " admitted=" + admitted);
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
            streamTrace("instance dispatch rejected source=" + inbound.source()
                + " sourceGeneration=" + header.sourceNodeGeneration()
                + " sourcePeerGeneration=" + (source.isPresent()
                    ? source.orElseThrow().descriptor().lifecycleGeneration() : -1)
                + " targetNode=" + header.route().targetNodeRid()
                + " localNode=" + routingId
                + " targetGeneration=" + header.route().targetNodeGeneration()
                + " localGeneration=" + (localDescriptor == null
                    ? -1 : localDescriptor.lifecycleGeneration())
                + " request=" + header.request()
                + " inboundRequest=" + (inbound.requestSequence() != null));
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
        ZLinkInboundDispatchBudget.Lease dispatchLease = null;
        String contentType;
        try {
            contentType = applicationContentType(messages);
            if (applicationDispatchBudget != null) {
                dispatchLease = applicationDispatchBudget.track(
                    applicationPayloadBytes(messages));
            }
        } catch (RuntimeException budgetFailure) {
            messages.forEach(Message::close);
            replyInstanceFailure(inbound, header, 104, 12);
            return;
        }
        AtomicBoolean terminal = new AtomicBoolean();
        boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
            .enqueueRemoteInstanceSpot(
                inbound.source(),
                header,
                metadata,
                messages,
                contentType,
                dispatchLease,
                replyParts -> {
                    if (!terminal.compareAndSet(false, true)) {
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
                });
        if (!accepted) {
            streamTrace("instance dispatch rejected by mailbox source="
                + inbound.source() + " target=" + header.route().targetSpotId()
                + " request=" + header.request());
            messages.forEach(Message::close);
            replyInstanceFailure(inbound, header, 102, 1);
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
        AtomicBoolean terminal = new AtomicBoolean();
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
            if (!terminal.compareAndSet(false, true)) {
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
        AtomicBoolean terminal = new AtomicBoolean();
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
            if (!terminal.compareAndSet(false, true)) {
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

    private void dispatchActor(
        ZLinkJavaRawServicePort.Inbound inbound,
        int flags) {
        List<byte[]> frames = inbound.frames();
        int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0
            ? 1
            : 2;
        if (frames.size() != payloadOffset + 1) {
            return;
        }
        ZLinkServiceM6BWireCodec.ActorMessage header;
        try {
            header = statefulWire.decodeActorHeader(frames.getFirst());
        } catch (RuntimeException invalid) {
            return;
        }
        ZLinkServiceM6AWireCodec.ApplicationPayload payload;
        try {
            payload = wire.decodeApplicationPayload(frames.get(payloadOffset));
        } catch (RuntimeException invalid) {
            replyActorFailure(inbound, header, 104, 12);
            return;
        }
        if (header.request() != (inbound.requestSequence() != null)
            || !header.target().actor().nodeRid().equals(routingId)
            || localDescriptor == null
            || header.target().targetNodeGeneration()
                != localDescriptor.lifecycleGeneration()) {
            replyActorFailure(inbound, header, 102, 1);
            return;
        }
        List<Message> messages = null;
        String contentType;
        ZLinkStreamCodec streamCodec;
        try {
            messages = decodeApplicationMessages(payload);
            contentType = applicationContentType(messages);
            streamCodec = java.util.Objects.requireNonNull(
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
            return;
        }
        final List<Message> receivedMessages = messages;
        ZLinkInboundDispatchBudget.Lease acquiredDispatchLease;
        try {
            if (applicationDispatchBudget != null) {
                acquiredDispatchLease = applicationDispatchBudget.track(
                    applicationPayloadBytes(receivedMessages));
            } else {
                acquiredDispatchLease = null;
            }
        } catch (RuntimeException budgetFailure) {
            receivedMessages.forEach(Message::close);
            replyActorFailure(inbound, header, 104, 12);
            return;
        }
        final ZLinkInboundDispatchBudget.Lease dispatchLease =
            acquiredDispatchLease;
        resolveAcceptedAuthorities(inbound).whenComplete((authorities, failure) -> {
            if (failure != null || authorities.isEmpty()) {
                receivedMessages.forEach(Message::close);
                if (dispatchLease != null) {
                    dispatchLease.close();
                }
                replyActorFailure(inbound, header, 107, 21);
                return;
            }
            AcceptedAuthorities acceptedAuthorities =
                authorities.orElseThrow();
            byte[] acceptedRecord;
            try {
                acceptedRecord = ZLinkServiceFrozenRecordCodec.encodeActor(
                    acceptedAuthorities.source(),
                    acceptedAuthorities.targetOwner(),
                    header,
                    new byte[0],
                    wire.encodeApplicationPayload(payload));
            } catch (RuntimeException invalidRecord) {
                receivedMessages.forEach(Message::close);
                if (dispatchLease != null) {
                    dispatchLease.close();
                }
                replyActorFailure(inbound, header, 104, 12);
                return;
            }
            AtomicBoolean terminal = new AtomicBoolean();
            boolean accepted = ((ZLinkJavaRawSpotNode) spotNode())
                .enqueueRemoteActor(
                    acceptedAuthorities.source().sourceNodeRid(),
                    acceptedAuthorities.source().sourceNodeGeneration(),
                    header,
                    acceptedRecord,
                    receivedMessages,
                    contentType,
                    dispatchLease,
                    replyParts -> {
                        if (!terminal.compareAndSet(false, true)) {
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
                        if (!terminal.compareAndSet(false, true)) {
                            return;
                        }
                        replyActorFailure(inbound, header, 102, 1);
                    });
            if (!accepted) {
                receivedMessages.forEach(Message::close);
                replyActorFailure(inbound, header, 102, 1);
            }
        });
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
        boolean accepted = source.isPresent()
            && ((ZLinkJavaRawSpotNode) spotNode())
                .acceptRemoteStreamBinding(
                    inbound.source(),
                    source.orElseThrow().descriptor()
                        .lifecycleGeneration(),
                    binding);
        port.reply(
            requireStarted(),
            inbound.source(),
            inbound.requestSequence(),
            List.of(wire.encodeReplyHeader(
                binding.correlation(),
                accepted ? 0 : 102,
                accepted ? 0 : 1)));
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
        List<Message> parts = List.of();
        try {
            ZLinkServiceM6BWireCodec.BoundSessionSend send =
                statefulWire.decodeBoundSessionSendHeader(
                    frames.getFirst());
            ZLinkServiceM6AWireCodec.ApplicationPayload payload =
                wire.decodeApplicationPayload(frames.get(1));
            parts = decodeApplicationMessages(payload);
            boolean accepted = ((ZLinkJavaRawSpotNode) spotNode()).acceptBoundSessionPush(
                inbound.source(),
                source.orElseThrow().descriptor()
                    .lifecycleGeneration(),
                send,
                parts);
            streamTrace("bound session receive "
                + (accepted ? "accepted" : "rejected")
                + " actor=" + actorSummary(send.actor().actor())
                + " source=" + inbound.source()
                + " binding=" + send.expectedBindingGeneration());
        } catch (RuntimeException invalid) {
            // A malformed or stale one-way record has no terminal route.
        } finally {
            parts.forEach(Message::close);
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
            ZLinkServiceNodeDescriptor descriptor =
                wire.decodeAdmission(
                    inbound.frames().getFirst(),
                    command,
                    inbound.source());
            PeerIntent expected = findExpectedPeer(
                inbound.source(),
                descriptor.advertisedEndpoint());
            PeerAdmissionExpectation observed =
                peerAdmissionExpectations.get(inbound.source());
            boolean manualExpectation = expected != null
                && expected.expectedLifecycleGeneration() == 0;
            String expectedEndpoint = observed == null || manualExpectation
                ? expected == null ? null : expected.endpoint()
                : observed.endpoint();
            String expectedSecurityIdentity =
                observed == null || manualExpectation
                ? expected == null
                    ? inbound.source().toString()
                    : expected.expectedSecurityIdentity()
                : observed.securityIdentity();
            long expectedLifecycleGeneration =
                observed == null || manualExpectation
                ? expected == null
                    ? 0
                    : expected.expectedLifecycleGeneration()
                : observed.lifecycleGeneration();
            if (!ZLinkServiceAdmissionGuard.matchesExpectedRoute(
                    expectedEndpoint,
                    expectedSecurityIdentity,
                    expectedLifecycleGeneration,
                    descriptor)) {
                trySendAdmissionControl(
                    inbound.source(),
                    List.of(wire.encodeReject(3)),
                    "expected-route-mismatch");
                return;
            }
            if (routeMeshConnectionNotRequired(
                localDescriptor,
                descriptor)) {
                disconnectAdmitted(inbound.source());
                pendingAdmissionControls.removeTarget(inbound.source());
                admissionControlReadyConnections.remove(inbound.source());
                admittedPeerObjectRoles.put(
                    inbound.source(),
                    descriptor.objectRole());
                notRequiredPeers.add(inbound.source());
                trySendAdmissionControl(
                    inbound.source(),
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
                inbound.source(), direction);
            String previousConnectionId =
                connectionIds.get(inbound.source());
            ZLinkServiceTopologyRegistry.AdmissionResult admitted =
                topology.admit(
                    descriptor,
                    new ZLinkServiceTopologyRegistry.Connection(
                        connectionId,
                        direction,
                        direction.name().toLowerCase(
                            java.util.Locale.ROOT)
                            + ":"
                            + descriptor.advertisedEndpoint()
                            + ":"
                            + connectionId));
            if (admitted
                == ZLinkServiceTopologyRegistry.AdmissionResult
                    .DUPLICATE_REJECTED) {
                trySendAdmissionControl(
                    inbound.source(),
                    List.of(wire.encodeReject(3)),
                    "duplicate-admission");
                return;
            }
            if (admitted
                != ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED) {
                trySendAdmissionControl(
                    inbound.source(),
                    List.of(wire.encodeReject(3)),
                    "admission-rejected");
                return;
            }
            pendingAdmissionControls.removeTarget(inbound.source());
            if (command != ServiceWireConstants.COMMAND_UPDATE
                || !connectionId.equals(previousConnectionId)) {
                admissionControlReadyConnections.remove(inbound.source());
            }
            connectionIds.put(inbound.source(), connectionId);
            admitPeerChannels(
                inbound.source(),
                descriptor.channels().stream().collect(
                    java.util.stream.Collectors.toMap(
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
            }
            if (command == ServiceWireConstants.COMMAND_HELLO) {
                trySendAdmissionControl(
                    inbound.source(),
                    List.of(wire.encodeAdmission(
                        ServiceWireConstants.COMMAND_ADMIT,
                        localDescriptor)),
                    "admit-response");
            }
        } catch (RuntimeException invalid) {
            trySendAdmissionControl(
                inbound.source(),
                List.of(wire.encodeReject(3)),
                "invalid-admission");
        }
    }

    private boolean trySendAdmissionControl(
        RoutingId target,
        List<byte[]> frames,
        String reason) {
        int command = wire.decodeHeader(frames.getFirst()).command();
        String connectionId = connectionIds.getOrDefault(target, "");
        long intentVersion = pendingAdmissionControls.nextIntentVersion();
        try {
            boolean submitted = port.send(
                requireStarted(),
                target,
                frames);
            if (submitted) {
                pendingAdmissionControls.removeUpTo(
                    target,
                    connectionId,
                    command,
                    intentVersion);
                markAdmissionControlReady(target, connectionId, command);
                return true;
            }
            streamTrace("admission-control-not-submitted target="
                + target
                + " reason=" + reason
                + " result=" + SubmitResult.BACKPRESSURED);
            rememberAdmissionControlRetry(
                target,
                connectionId,
                command,
                frames,
                intentVersion,
                reason);
            return false;
        } catch (ZlinkSubmitException failure) {
            streamTrace("admission-control-send-failed target="
                + target
                + " reason=" + reason
                + " result=" + failure.getResult()
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()));
            if (failure.getResult() == SubmitResult.BACKPRESSURED) {
                rememberAdmissionControlRetry(
                    target,
                    connectionId,
                    command,
                    frames,
                    intentVersion,
                    reason);
            }
            return false;
        } catch (RuntimeException failure) {
            streamTrace("admission-control-send-failed target="
                + target
                + " reason=" + reason
                + " result=PERMANENT_FAILURE"
                + " error=" + failure.getClass().getSimpleName()
                + ":" + String.valueOf(failure.getMessage()));
            return false;
        }
    }

    private void rememberAdmissionControlRetry(
        RoutingId target,
        String connectionId,
        int command,
        List<byte[]> frames,
        long intentVersion,
        String reason) {
        if (pendingAdmissionControls.remember(
                target,
                connectionId,
                command,
                frames,
                intentVersion)) {
            streamTrace("admission-control-deferred target="
                + target
                + " reason=" + reason
                + " connection=" + connectionId
                + " command=" + command);
            return;
        }
        streamTrace("admission-control-retry-capacity target="
            + target
            + " reason=" + reason
            + " connection=" + connectionId
            + " command=" + command);
    }

    private void drainAdmissionControlRetries() {
        if (!admissionControlRetryReady.compareAndSet(true, false)) {
            return;
        }
        pendingAdmissionControls.flush(pending -> {
            if (closed.get()) {
                return ZLinkJavaAdmissionControlRetryQueue.RetryResult.STALE;
            }
            if (pending.connectionId().isBlank()
                || !pending.connectionId().equals(
                    connectionIds.get(pending.target()))) {
                return ZLinkJavaAdmissionControlRetryQueue.RetryResult.STALE;
            }
            try {
                boolean submitted = port.send(
                    requireStarted(),
                    pending.target(),
                    pending.frames());
                streamTrace("admission-control-retry target="
                    + pending.target()
                    + " connection=" + pending.connectionId()
                    + " command=" + pending.command()
                    + " result=" + (submitted
                        ? "ACCEPTED"
                        : "BACKPRESSURED"));
                if (submitted) {
                    markAdmissionControlReady(
                        pending.target(),
                        pending.connectionId(),
                        pending.command());
                }
                return submitted
                    ? ZLinkJavaAdmissionControlRetryQueue.RetryResult.ACCEPTED
                    : ZLinkJavaAdmissionControlRetryQueue.RetryResult.BACKPRESSURED;
            } catch (ZlinkSubmitException failure) {
                streamTrace("admission-control-retry-failed target="
                    + pending.target()
                    + " connection=" + pending.connectionId()
                    + " command=" + pending.command()
                    + " result=" + failure.getResult());
                return failure.getResult() == SubmitResult.BACKPRESSURED
                    ? ZLinkJavaAdmissionControlRetryQueue.RetryResult.BACKPRESSURED
                    : ZLinkJavaAdmissionControlRetryQueue.RetryResult.PERMANENT_FAILURE;
            } catch (RuntimeException failure) {
                streamTrace("admission-control-retry-failed target="
                    + pending.target()
                    + " connection=" + pending.connectionId()
                    + " command=" + pending.command()
                    + " result=PERMANENT_FAILURE"
                    + " error=" + failure.getClass().getSimpleName()
                    + ":" + String.valueOf(failure.getMessage()));
                return ZLinkJavaAdmissionControlRetryQueue.RetryResult.PERMANENT_FAILURE;
            }
        });
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
            if (command == ServiceWireConstants.COMMAND_LIVENESS_PROBE) {
                streamTrace("liveness-probe source=" + inbound.source()
                    + " connection=" + peer.connectionId());
                if (liveness.acknowledgeProbe(
                    inbound.source(),
                    peer.connectionId(),
                    probeId).isPresent()) {
                    port.send(
                        requireStarted(),
                        inbound.source(),
                        encodeLiveness(
                            ServiceWireConstants.COMMAND_LIVENESS_ACK,
                            probeId));
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
                    ZLinkJavaRawSpotNode spots =
                        (ZLinkJavaRawSpotNode) spotNode();
                    spots.admissionReadyForPeer(inbound.source());
                    channelWeights.keySet().forEach(channel ->
                        spots.admissionReadyForChannel(channel));
                }
                streamTrace("liveness-ack source=" + inbound.source()
                    + " connection=" + peer.connectionId()
                    + " probe=" + probeId
                    + " accepted=" + acknowledged);
            }
        } catch (RuntimeException ignored) {
        }
    }

    /**
     * A Router send-ready callback is a source-wide transport signal. The
     * admission runtime retries only the node routes that are currently
     * liveness-ready; it must still receive this signal when a non-blocking
     * application submit was rejected after the liveness ACK.
     */
    private void notifyAdmissionReadyPeers() {
        admissionControlRetryReady.set(true);
        ZLinkJavaRawSpotNode spots = spotNode;
        ZLinkServiceTopologyRegistry current = topology;
        if (spots == null || current == null) {
            return;
        }
        for (RoutingId peerRid : readyAdmissionPeerIds(
            current.peers(), this::isReadyPeer)) {
            spots.admissionReadyForPeer(peerRid);
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
                continue;
            }
            streamTrace("transport-event event=" + event.event()
                + " peer=" + peer.orElseThrow()
                + " value=" + event.value()
                + " local=" + event.localAddr()
                + " remote=" + event.remoteAddr());
            if (event.event() == MonitorEventType.CONNECTION_READY) {
                RoutingId peerRid = peer.orElseThrow();
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
                                new java.util.concurrent.ConcurrentLinkedQueue<>())
                        .add(registeredId);
                }
                nextAnnouncementNanos.put(peer.orElseThrow(), 0L);
            } else if (event.event() == MonitorEventType.DISCONNECTED
                || event.event() == MonitorEventType.CLOSED) {
                String disconnectedId = removeTransportConnection(event);
                discardPendingConnectionId(disconnectedId);
                if (disconnectedId == null) {
                    pendingAdmissionControls.removeTarget(peer.orElseThrow());
                } else {
                    pendingAdmissionControls.removeTargetConnection(
                        peer.orElseThrow(),
                        disconnectedId);
                }
                disconnectAdmitted(peer.orElseThrow(), disconnectedId);
                nextAnnouncementNanos.put(peer.orElseThrow(), 0L);
            }
        }
    }

    private void announceExpectedPeers(long nowNanos) {
        for (PeerIntent intent : peerIntents.values()) {
            RoutingId expected = intent.expectedRoutingId();
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
                boolean submitted = port.send(
                    requireStarted(),
                    probe.nodeRoutingId(),
                    encodeLiveness(
                        ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                        probe.probeId()));
                streamTrace("liveness-send target=" + probe.nodeRoutingId()
                    + " connection=" + probe.connectionId()
                    + " probe=" + probe.probeId()
                    + " submitted=" + submitted);
            } catch (RuntimeException ignored) {
                streamTrace("liveness-send-failed target="
                    + probe.nodeRoutingId()
                    + " connection=" + probe.connectionId()
                    + " error=" + ignored.getClass().getSimpleName()
                    + ":" + String.valueOf(ignored.getMessage()));
                // The liveness timeout owns peer eviction. A transient send
                // failure must not stop the service receive pump.
            }
        }
        tick.timedOutNodes().forEach(this::disconnectAdmitted);
    }

    private void drainInfrastructureSends() {
        InfrastructureSend pending;
        while ((pending = pendingInfrastructureSends.peek()) != null) {
            if (!pending.active().getAsBoolean()) {
                pendingInfrastructureSends.poll();
                continue;
            }
            try {
                boolean submitted = port.send(
                        requireStarted(),
                        pending.target(),
                        pending.frames());
                if (!submitted) {
                    return;
                }
                pendingInfrastructureSends.poll();
            } catch (RuntimeException failure) {
                pendingInfrastructureSends.poll();
                pending.onFailure().accept(failure);
            }
        }
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

    private static boolean isCanonicalRelocationControl(int command) {
        return command == ServiceWireConstants.COMMAND_RELOCATION_READY
            || command == ServiceWireConstants.COMMAND_RELOCATION_DATA
            || command == ServiceWireConstants.COMMAND_RELOCATION_ACK
            || command == ServiceWireConstants.COMMAND_RELOCATION_SEAL
            || command == ServiceWireConstants.COMMAND_RELOCATION_COMPLETE
            || command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE
            || command == ServiceWireConstants.COMMAND_RELOCATION_RESERVED;
    }

    private void disconnectAdmitted(RoutingId peer) {
        disconnectAdmitted(peer, connectionIds.get(peer));
    }

    private void disconnectAdmitted(
        RoutingId peer,
        String connectionId) {
        if (connectionId == null) {
            pendingAdmissionControls.removeTarget(peer);
            admissionControlReadyConnections.remove(peer);
            return;
        }
        if (!topology.disconnect(peer, connectionId)) {
            pendingAdmissionControls.removeTargetConnection(peer, connectionId);
            admissionControlReadyConnections.remove(peer, connectionId);
            return;
        }
        pendingAdmissionControls.removeTarget(peer);
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

    private String connectionIdForAdmission(
        RoutingId peer,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction) {
        ConnectionCandidate candidate =
            new ConnectionCandidate(peer, direction);
        var pending = pendingConnectionIds.get(candidate);
        String connectionId = pending == null ? null : pending.poll();
        if (pending != null && pending.isEmpty()) {
            pendingConnectionIds.remove(candidate, pending);
        }
        if (connectionId != null) {
            return connectionId;
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
            ignored -> new java.util.concurrent.ConcurrentLinkedQueue<>())
            .add(id);
        return id;
    }

    private ZLinkServiceAdmissionGuard.ConnectionDirection
        monitorConnectionDirection(
            MonitorEvent event,
            RoutingId peer) {
        if (java.util.Objects.equals(event.localAddr(), bindEndpoint)) {
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
        String id = ids.poll();
        if (ids.isEmpty()) {
            monitorConnectionIds.remove(key, ids);
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
        if (currentMailbox == null) {
            return;
        }
        Optional<ZLinkServiceMailbox.Claim> claimed;
        while ((claimed = currentMailbox.tryClaim(
            ZLinkServiceMailbox.Domain.APPLICATION,
            64,
            1024L * 1024)).isPresent()) {
            ZLinkServiceMailbox.Claim claim = claimed.orElseThrow();
            try {
                applicationDispatch.execute(() -> {
                    try {
                        for (ZLinkServiceMailbox.Record record
                            : claim.records()) {
                            Long envelopeId = record.correlation();
                            ZLinkMeshDispatchRecord dispatch =
                                dispatchEnvelopes.remove(envelopeId);
                            if (dispatch != null) {
                                boolean accepted = false;
                                try {
                                    streamTrace("channel-application-dispatch-start channel="
                                        + dispatch.receive().channelName()
                                        + " source=" + dispatch.receive().sourceNodeRid()
                                        + " envelope=" + envelopeId);
                                    receiver.accept(dispatch);
                                    accepted = true;
                                    streamTrace("channel-application-dispatch-accepted channel="
                                        + dispatch.receive().channelName()
                                        + " source=" + dispatch.receive().sourceNodeRid()
                                        + " envelope=" + envelopeId);
                                } catch (RuntimeException failure) {
                                    streamTrace("channel-application-dispatch-rejected channel="
                                        + dispatch.receive().channelName()
                                        + " source=" + dispatch.receive().sourceNodeRid()
                                        + " envelope=" + envelopeId
                                        + " error=" + failure.getClass().getSimpleName());
                                    throw failure;
                                } finally {
                                    if (!accepted) {
                                        dispatch.close();
                                    }
                                }
                            }
                        }
                    } finally {
                        currentMailbox.release(claim);
                    }
                });
            } catch (java.util.concurrent.RejectedExecutionException rejected) {
                for (ZLinkServiceMailbox.Record record : claim.records()) {
                    ZLinkMeshDispatchRecord dispatch =
                        dispatchEnvelopes.remove(record.correlation());
                    if (dispatch != null) {
                        dispatch.close();
                    }
                }
                currentMailbox.release(claim);
            }
        }
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
            bindEndpoint,
            channels,
            descriptorState,
            routingId.toString(),
            4 * 1024 * 1024,
            0,
            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
            objectRole,
            placementWeight,
            10_000,
            128,
            0,
            0);
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
        String normalized = contentType.trim();
        return "application/json".equalsIgnoreCase(normalized)
                || "application/zlink-framework-json-v1"
                    .equalsIgnoreCase(normalized)
            ? Optional.of(ZLinkStreamCodec.JSON)
            : Optional.empty();
    }

    private static ZLinkBackendRequestResult backendResult(
        systems.zlink.contracts.sockets.RequestResult result) {
        return result == systems.zlink.contracts.sockets.RequestResult.BACKPRESSURED
            ? ZLinkBackendRequestResult.BUSY
            : ZLinkBackendRequestResult.valueOf(result.name());
    }

    private static ZLinkBackendRequestResult backendResult(int wireValue) {
        for (ZLinkBackendRequestResult value : ZLinkBackendRequestResult.values()) {
            if (value.ordinal() == 0 && wireValue == 0) {
                return value;
            }
            if (wireValue == 101 + value.ordinal() - 1) {
                return value;
            }
        }
        return ZLinkBackendRequestResult.PROTOCOL_ERROR;
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
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
            || current instanceof java.util.concurrent.ExecutionException)
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
            java.util.Objects.requireNonNull(peer, "peer");
            java.util.Objects.requireNonNull(direction, "direction");
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

    private record InfrastructureSend(
        RoutingId target,
        List<byte[]> frames,
        java.util.function.BooleanSupplier active,
        java.util.function.Consumer<RuntimeException> onFailure) {
        private InfrastructureSend {
            java.util.Objects.requireNonNull(target, "target");
            frames = frames.stream().map(byte[]::clone).toList();
            java.util.Objects.requireNonNull(active, "active");
            java.util.Objects.requireNonNull(onFailure, "onFailure");
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
            if (endpoint == null || endpoint.isBlank()
                || lifecycleGeneration <= 0
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
