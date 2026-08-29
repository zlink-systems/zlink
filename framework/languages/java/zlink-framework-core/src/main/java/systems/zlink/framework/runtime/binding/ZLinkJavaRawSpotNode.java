package systems.zlink.framework.runtime.binding;
import java.util.ArrayList;
import java.util.Objects;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.completion.ZLinkTerminalWinner;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Framework-owned service runtime projected over the raw MeshNode transport.
 * Stateful Spot and Actor identity remains inside the Framework runtime.
 */
final class ZLinkJavaRawSpotNode
    implements ZLinkInternalSpotNode, ZLinkJavaAdmissionBacked {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkJavaRawSpotNode.class.getName());
    private final ZLinkJavaRawMeshNode owner;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ZLinkJavaRawSpot> spots =
        new ConcurrentHashMap<>();
    private final Map<String, ZLinkBackendActorRef> actors =
        new ConcurrentHashMap<>();
    private final Map<String, String> actorSpots =
        new ConcurrentHashMap<>();
    private final Map<String, Long> actorMembershipEpochs =
        new ConcurrentHashMap<>();
    private final Map<SpotAuthorityKey, AuthorityFence> spotAuthorities =
        new ConcurrentHashMap<>();
    private final Map<ActorAuthorityKey, AuthorityFence> actorAuthorities =
        new ConcurrentHashMap<>();
    private final AtomicLong nextGeneration = new AtomicLong(1);
    private final AtomicLong nextRequestSequence = new AtomicLong(1);
    private final AtomicLong nextActorRequestSequence = new AtomicLong(1);
    private final AtomicLong nextStreamBindingGeneration =
        new AtomicLong();
    private final Map<Long, CompletableFuture<List<Message>>> actorRequests =
        new ConcurrentHashMap<>();
    private final Map<Long, Consumer<List<Message>>> actorRemoteReplies =
        new ConcurrentHashMap<>();
    private final Map<String, StreamBinding> streamBindings =
        new ConcurrentHashMap<>();
    private final Map<String, Long> streamBindingSequences =
        new ConcurrentHashMap<>();
    private final Map<String, RemoteStreamBinding> remoteStreamBindings =
        new ConcurrentHashMap<>();
    private final Map<String, Long> remoteStreamSequences =
        new ConcurrentHashMap<>();
    private final ZLinkJavaInstanceSpotRegistry instanceSpots =
        new ZLinkJavaInstanceSpotRegistry();
    private final Map<String, InstanceAuthority> instanceAuthorities =
        new ConcurrentHashMap<>();
    private volatile ZLinkJavaRawSpot entrySpot;
    private volatile ZLinkMeshApplicationReceiver applicationReceiver;
    private volatile ZLinkInternalSpotNode.MessageFollowRelayHandler
        messageFollowRelayHandler;
    private volatile ZLinkInternalSpotNode.RelocationStagingIngressHandler
        relocationStagingIngressHandler;
    private final Map<ZLinkServiceM6BWireCodec.SpotRouteFence,
        ZLinkServiceM6BWireCodec.SpotRouteFence> relocationSpotForwards =
            new ConcurrentHashMap<>();
    private final Map<ZLinkServiceM6BWireCodec.ActorRouteFence,
        ZLinkServiceM6BWireCodec.ActorRouteFence> relocationActorForwards =
            new ConcurrentHashMap<>();

    ZLinkJavaRawSpotNode(ZLinkJavaRawMeshNode owner) {
        this.owner = owner;
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    @Override
    public String name() {
        return owner.name();
    }

    @Override
    public RoutingId routingId() {
        return owner.routingId();
    }

    @Override
    public long localAuthorityLeaseGeneration() {
        return owner.localAuthorityLeaseGeneration();
    }

    @Override
    public long localNodeGeneration() {
        return ownerLifecycleGeneration();
    }

    @Override
    public String localAuthorityOwnerId() {
        return owner.localAuthorityOwnerId();
    }

    @Override
    public long actorNodeGeneration(ZLinkBackendActorRef actor) {
        return owner.nodeLifecycleGeneration(actor.nodeRid());
    }

    @Override
    public void setRoutingId(RoutingId routingId) {
        owner.setRoutingId(routingId);
    }

    @Override
    public void setPublisherRoutingId(RoutingId routingId) {
    }

    @Override
    public void setSubscriberRoutingId(RoutingId routingId) {
    }

    @Override
    public void setRouterBind(String endpoint) {
        owner.setBind(endpoint);
    }

    @Override
    public void setPubBind(String endpoint) {
    }

    @Override
    public void connectPeer(String endpoint) {
        owner.connectPeer(endpoint);
    }

    @Override
    public void connectPeer(RoutingId peerRid, String endpoint) {
        owner.connectPeer(endpoint, peerRid);
    }

    @Override
    public void disconnectPeer(String endpoint) {
        owner.connectionIntentIds().stream()
            .filter(intent -> owner.peers().stream().anyMatch(
                peer -> peer.connectionIntentId() == intent
                    && peer.endpoint().equals(endpoint)))
            .findFirst()
            .ifPresent(owner::removePeerConnection);
    }

    @Override
    public void disconnectPeer(RoutingId peerRid) {
        owner.peers().stream()
            .filter(peer -> peer.routingId().equals(peerRid))
            .findFirst()
            .ifPresent(peer -> owner.removePeerConnection(
                peer.connectionIntentId()));
    }

    @Override
    public void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        applicationReceiver = Objects.requireNonNull(receiver, "receiver");
        receiver.setLocalNodeReadyHandler(() -> { });
    }

    @Override
    public void setMessageFollowRelayHandler(
        ZLinkInternalSpotNode.MessageFollowRelayHandler handler) {
        messageFollowRelayHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public void setRelocationStagingIngressHandler(
        ZLinkInternalSpotNode.RelocationStagingIngressHandler handler) {
        relocationStagingIngressHandler = Objects.requireNonNull(
            handler, "handler");
    }

    @Override
    public void installRelocationSpotForward(
        ZLinkServiceM6BWireCodec.SpotRouteFence source,
        ZLinkServiceM6BWireCodec.SpotRouteFence target,
        Duration retention) {
        installRelocationForward(
            relocationSpotForwards, source, target, retention);
    }

    @Override
    public void installRelocationActorForward(
        ZLinkServiceM6BWireCodec.ActorRouteFence source,
        ZLinkServiceM6BWireCodec.ActorRouteFence target,
        Duration retention) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(retention, "retention");
        if (retention.isNegative() || retention.isZero()) {
            throw new IllegalArgumentException(
                "relocation forward retention must be positive");
        }
        relocationActorForwards.compute(source, (ignored, previous) -> {
            if (previous == null || previous.equals(target)) {
                return target;
            }
            boolean sameCommittedTarget =
                previous.actor().equals(target.actor())
                    && previous.targetNodeGeneration()
                        == target.targetNodeGeneration()
                    && previous.ownerLeaseGeneration()
                        == target.ownerLeaseGeneration();
            if (!sameCommittedTarget
                || target.authorityOwnerGeneration()
                    <= previous.authorityOwnerGeneration()) {
                throw new IllegalStateException(
                    "relocation forward source already has another target");
            }
            // The provider allocates AuthorityOwnerGeneration from a global
            // monotonic sequence, so the source's provisional +1 fence must
            // be refreshed with the generation returned by the target CAS.
            return target;
        });
        CompletableFuture.delayedExecutor(
                retention.toMillis(), TimeUnit.MILLISECONDS)
            .execute(() -> relocationActorForwards.remove(source, target));
    }

    private static <T> void installRelocationForward(
        Map<T, T> forwards,
        T source,
        T target,
        Duration retention) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(retention, "retention");
        if (retention.isNegative() || retention.isZero()) {
            throw new IllegalArgumentException(
                "relocation forward retention must be positive");
        }
        T previous = forwards.putIfAbsent(source, target);
        if (previous != null && !previous.equals(target)) {
            throw new IllegalStateException(
                "relocation forward source already has another target");
        }
        CompletableFuture.delayedExecutor(
                retention.toMillis(), TimeUnit.MILLISECONDS)
            .execute(() -> forwards.remove(source, target));
    }

    @Override
    public Optional<CompletionStage<Integer>> submitLocalNodeSend(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts) {
        if (!routingId().equals(targetNodeRid)) {
            return Optional.empty();
        }
        ZLinkMeshApplicationReceiver current = applicationReceiver;
        if (current == null) {
            return Optional.of(CompletableFuture.completedFuture(
                ZLinkOneWayCalls.TARGET_NOT_FOUND));
        }
        return Optional.of(current.submitLocalNodeSend(
            routingId(), metadata, parts));
    }

    @Override
    public Optional<Integer> classifyNodeSendTarget(
        RoutingId targetNodeRid) {
        if (owner.isObjectClientNodeDirectTarget(targetNodeRid)) {
            return Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        if (routingId().equals(targetNodeRid)) {
            return Optional.empty();
        }
        Optional<MeshPeerState> peerState = owner.peers().stream()
            .filter(peer -> peer.routingId().equals(targetNodeRid))
            .map(peer -> peer.state())
            .findFirst();
        if (peerState.isPresent()) {
            return switch (peerState.orElseThrow()) {
                case ADMITTED, CONFIGURED, CONNECTING -> Optional.empty();
                case NOT_REQUIRED -> Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
                case CLOSED, DRAINING, ERROR ->
                    Optional.of(ZLinkOneWayCalls.ROUTE_NOT_CONNECTED);
            };
        }
        return Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
    }

    @Override
    public Optional<Integer> classifyChannelTarget(String channelName) {
        return owner.classifyChannelTarget(channelName);
    }

    @Override
    public CompletionStage<Void> sendToNode(
        RoutingId targetNodeRid,
        List<Message> parts) {
        return sendToNode(targetNodeRid, new byte[0], parts);
    }

    @Override
    public CompletionStage<Void> sendToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts) {
        return owner.sendNode(
            targetNodeRid, metadata, parts, false, null);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToNode(
        RoutingId targetNodeRid,
        List<Message> parts,
        Duration timeout) {
        return requestToNode(
            targetNodeRid, new byte[0], parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        return owner.requestNode(
            targetNodeRid, metadata, parts, timeout);
    }

    @Override
    public CompletionStage<Void> sendToChannel(
        String channelName,
        List<Message> parts) {
        return sendToChannel(channelName, new byte[0], parts);
    }

    @Override
    public CompletionStage<Void> sendToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts) {
        return owner.sendChannel(channelName, metadata, parts);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToChannel(
        String channelName,
        List<Message> parts,
        Duration timeout) {
        return requestToChannel(
            channelName, new byte[0], parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        return owner.requestChannel(
            channelName, metadata, parts, timeout);
    }

    @Override
    public void publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags) {
        owner.publishLogicalMulticast(
            null, channelName, topic, new byte[0], parts);
    }

    @Override
    public void publish(
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        owner.publishLogicalMulticast(
            null, channelName, topic, metadata, parts);
    }

    @Override
    public ZLinkBackendSpotRouteBridge createRouteBridge() {
        throw new UnsupportedOperationException(
            "raw MeshNode routes Spot records without a route bridge");
    }

    @Override
    public ZLinkBackendSpot createSpot() {
        return createSpot(UUID.randomUUID().toString());
    }

    @Override
    public ZLinkBackendSpot createSpot(String spotId) {
        Objects.requireNonNull(spotId, "spotId");
        return createSpot(spotId, nextGeneration.getAndIncrement());
    }

    @Override
    public ZLinkBackendSpot createSpot(
        String spotId,
        long lifecycleGeneration) {
        Objects.requireNonNull(spotId, "spotId");
        if (lifecycleGeneration == 0) {
            throw new IllegalArgumentException(
                "Spot lifecycle generation must be non-zero");
        }
        ZLinkJavaRawSpot created = new ZLinkJavaRawSpot(
            this, spotId, lifecycleGeneration);
        ZLinkJavaRawSpot existing = spots.putIfAbsent(spotId, created);
        if (existing != null) {
            return existing;
        }
        rememberSpotAuthority(
            routingId(),
            spotId,
            lifecycleGeneration,
            lifecycleGeneration,
            owner.localAuthorityLeaseGeneration());
        return created;
    }

    @Override
    public ZLinkBackendSpot entrySpot() {
        ZLinkJavaRawSpot current = entrySpot;
        if (current != null) {
            return current;
        }
        return inStateLane(() -> {
            if (entrySpot == null) {
                entrySpot = (ZLinkJavaRawSpot) createSpot(
                    routingId() + "-entry-" + UUID.randomUUID());
            }
            return entrySpot;
        });
    }

    @Override
    public ZLinkBackendActorRef createActor(
        String actorId,
        Message createRequest) {
        return createActor(actorId, 1, createRequest);
    }

    @Override
    public ZLinkBackendActorRef createActor(
        String actorId,
        long objectGeneration,
        Message createRequest) {
        if (actorId == null || actorId.isBlank()) {
            throw new IllegalArgumentException("actorId is required");
        }
        if (objectGeneration <= 0) {
            throw new IllegalArgumentException(
                "Actor object generation must be positive");
        }
        ZLinkBackendActorRef created = new ZLinkBackendActorRef(
            routingId(), actorId, objectGeneration);
        if (actors.putIfAbsent(actorId, created) != null) {
            throw new IllegalStateException("actor already exists: " + actorId);
        }
        actorSpots.put(actorId, entrySpot().spotId());
        actorMembershipEpochs.put(actorId, 1L);
        rememberActorAuthority(
            created,
            created.generation(),
            owner.localAuthorityLeaseGeneration());
        return created;
    }

    @Override
    public ZLinkBackendActorRef actorLookup(String actorId) {
        return actors.get(actorId);
    }

    @Override
    public boolean hasPendingActorRequests() {
        return !actorRequests.isEmpty();
    }

    @Override
    public void rememberActorAuthority(
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration) {
        rememberActorAuthority(
            actor,
            authorityOwnerGeneration,
            owner.localAuthorityLeaseGeneration());
    }

    @Override
    public void rememberActorAuthority(
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        if (actor == null
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            throw new IllegalArgumentException(
                "Actor authority generations must be positive");
        }
        actorAuthorities.put(
            new ActorAuthorityKey(
                actor.nodeRid(), actor.actorId(), actor.generation()),
            new AuthorityFence(
                authorityOwnerGeneration, ownerLeaseGeneration));
    }

    @Override
    public void registerTransferredActor(
        ZLinkBackendActorRef actor,
        String spotId,
        long membershipEpoch) {
        Objects.requireNonNull(actor, "actor");
        if (!routingId().equals(actor.nodeRid())
            || actor.generation() <= 0
            || spotId == null
            || spotId.isBlank()
            || membershipEpoch <= 0) {
            throw new IllegalArgumentException(
                "transferred Actor route requires the local node, exact "
                    + "generation, SpotId, and positive membership epoch");
        }
        ZLinkBackendActorRef current = actors.putIfAbsent(
            actor.actorId(),
            actor);
        if (current != null && !current.equals(actor)) {
            throw new IllegalStateException(
                "actor generation already exists on target: "
                    + actor.actorId());
        }
        actorSpots.put(actor.actorId(), spotId);
        actorMembershipEpochs.put(actor.actorId(), membershipEpoch);
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> parts,
        Duration timeout) {
        return joinActor(
            actor, targetNodeRid, targetSpotId, 0, parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> parts,
        Duration timeout) {
        ZLinkJavaRawSpot target = localSpot(
            targetNodeRid, targetSpotId, targetSpotGeneration);
        if (target == null || !actors.containsKey(actor.actorId())) {
            return CompletableFuture.completedFuture(
                new ZLinkBackendActorJoinResult(
                    systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendRequestResult.NOT_FOUND,
                    1,
                    actor,
                    targetSpotId,
                    actorMembershipEpochs.getOrDefault(actor.actorId(), 0L),
                    0,
                    List.of()));
        }
        ZLinkJavaRawSpot.PendingJoin pending =
            new ZLinkJavaRawSpot.PendingJoin();
        ZLinkBackendActorJoinRequest request =
            new ZLinkBackendActorJoinRequest(
                actor,
                actor,
                ZLinkJavaRawSpot.copy(parts),
                pending);
        target.enqueueJoin(request).whenComplete((ignored, failure) -> {
            if (failure != null) {
                pending.fail(failure);
            }
        });
        return pending.completion().thenApply(reply -> {
            long epoch = actorMembershipEpochs.getOrDefault(
                actor.actorId(), 1L);
            if (reply.resultCode() == 0) {
                epoch = epoch == Long.MAX_VALUE ? Long.MAX_VALUE : epoch + 1;
                actorSpots.put(actor.actorId(), targetSpotId);
                actorMembershipEpochs.put(actor.actorId(), epoch);
            }
            return new ZLinkBackendActorJoinResult(
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendRequestResult.OK,
                reply.resultCode(),
                actor,
                targetSpotId,
                epoch,
                0,
                reply.parts());
        });
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        Message request,
        Duration timeout) {
        if (!routingId().equals(targetNodeRid)
            || !actors.containsKey(actor.actorId())) {
            return CompletableFuture.completedFuture(
                new ZLinkBackendActorJoinEntrySpotResult(
                    systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendRequestResult.NOT_FOUND,
                    1,
                    actor,
                    targetNodeRid,
                    entrySpot().spotId(),
                    actorMembershipEpochs.getOrDefault(actor.actorId(), 0L),
                    0,
                    List.of()));
        }
        ZLinkJavaRawSpot target = (ZLinkJavaRawSpot) entrySpot();
        ZLinkJavaRawSpot.PendingJoin pending =
            new ZLinkJavaRawSpot.PendingJoin();
        ZLinkBackendActorJoinRequest join =
            new ZLinkBackendActorJoinRequest(
                actor,
                actor,
                List.of(Message.from(request.dataBuffer())),
                pending);
        target.enqueueJoin(join).whenComplete((ignored, failure) -> {
            if (failure != null) {
                pending.fail(failure);
            }
        });
        return pending.completion().thenApply(reply -> {
            long epoch = actorMembershipEpochs.getOrDefault(
                actor.actorId(), 1L);
            if (reply.resultCode() == 0) {
                epoch = epoch == Long.MAX_VALUE ? Long.MAX_VALUE : epoch + 1;
                actorSpots.put(actor.actorId(), target.spotId());
                actorMembershipEpochs.put(actor.actorId(), epoch);
            }
            return new ZLinkBackendActorJoinEntrySpotResult(
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendRequestResult.OK,
                reply.resultCode(),
                actor,
                targetNodeRid,
                target.spotId(),
                epoch,
                0,
                reply.parts());
        });
    }

    @Override
    public CompletionStage<List<Message>> leaveActor(
        ZLinkBackendActorRef actor,
        String currentSpotId,
        Duration timeout) {
        ZLinkJavaRawSpot current = spots.get(currentSpotId);
        if (current == null || !actors.containsKey(actor.actorId())) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("actor membership is stale"));
        }
        long nextEpoch = actorMembershipEpochs.getOrDefault(
            actor.actorId(), 1L);
        nextEpoch = nextEpoch == Long.MAX_VALUE
            ? Long.MAX_VALUE
            : nextEpoch + 1;
        actorSpots.put(actor.actorId(), entrySpot().spotId());
        actorMembershipEpochs.put(actor.actorId(), nextEpoch);
        ZLinkBackendActorLifecycleEvent left =
            new ZLinkBackendActorLifecycleEvent(
                ZLinkBackendActorLifecycleEventKind.LEFT,
                new ZLinkBackendActorLifecycleInfo(
                    actor,
                    actor,
                    Optional.of(currentSpotId),
                    Optional.of(entrySpot().spotId()),
                    nextEpoch,
                    0));
        return current.enqueueLifecycle(left).thenApply(ignored -> List.of());
    }

    @Override
    public CompletionStage<Void> destroyActor(
        ZLinkBackendActorRef actor,
        Duration timeout) {
        actors.remove(actor.actorId());
        actorSpots.remove(actor.actorId());
        actorMembershipEpochs.remove(actor.actorId());
        StreamBinding binding = streamBindings.remove(actor.actorId());
        if (binding != null) {
            discardStreamSession(
                binding.sessionRid(),
                binding.actor(),
                binding.bindingGeneration(),
                binding.stream());
        }
        streamBindingSequences.remove(actor.actorId());
        remoteStreamBindings.remove(actor.actorId());
        remoteStreamSequences.remove(actor.actorId());
        actorAuthorities.remove(new ActorAuthorityKey(
            actor.nodeRid(), actor.actorId(), actor.generation()));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public boolean sendActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        if (binding == null || !binding.actor().equals(actor)) {
            return false;
        }
        boolean accepted = binding.stream().sendBoundSessionPush(
            binding.sessionRid(), parts, flags);
        streamTrace(STREAM_TRACE ? "bound session local "
            + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(actor)
            + " session=" + binding.sessionRid()
            + " binding=" + binding.bindingGeneration() : null);
        return accepted;
    }

    @Override
    public boolean hasRemoteActorBoundSessionRoute(
        ZLinkBackendActorRef actor) {
        RemoteStreamBinding remote =
            remoteStreamBindings.get(actor.actorId());
        return remote != null && remote.actor().equals(actor);
    }

    @Override
    public boolean hasLocalActorBoundSessionRoute(
        ZLinkBackendActorRef actor) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        return binding != null && binding.actor().equals(actor);
    }

    @Override
    public boolean sendLocalActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        return sendActorBoundSession(actor, parts, flags);
    }

    @Override
    public CompletionStage<Void> sendLocalActorBoundSessionAsync(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        Duration timeout) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        if (binding == null || !binding.actor().equals(actor)) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        }
        CompletionStage<Void> submitted =
            binding.stream().sendBoundSessionPushAsync(
                binding.sessionRid(), parts, timeout);
        submitted.whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
            "bound session local async "
                + (failure == null ? "accepted" : "rejected")
                + " actor=" + actorSummary(actor)
                + " session=" + binding.sessionRid()
                + " binding=" + binding.bindingGeneration() : null));
        return submitted;
    }

    @Override
    public CompletionStage<Void> sendRemoteActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts) {
        RemoteStreamBinding remote =
            remoteStreamBindings.get(actor.actorId());
        if (remote == null || !remote.actor().equals(actor)) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        }
        return owner.sendBoundSession(remote, parts);
    }

    @Override
    public void replyActorNoBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long requestId,
        int flags,
        List<Message> parts) {
        CompletableFuture<List<Message>> pending =
            actorRequests.remove(requestId);
        if (pending != null) {
            pending.complete(ZLinkJavaRawSpot.copy(parts));
            return;
        }
        Consumer<List<Message>> remote = actorRemoteReplies.remove(requestId);
        if (remote != null) {
            remote.accept(parts);
            return;
        }
        if (!isCurrentActor(actor)) {
            throw new IllegalStateException("actor is not local");
        }
    }

    @Override
    public boolean sendToActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        if (!routingId().equals(actor.nodeRid())) {
            return false;
        }
        return dispatchLocalActor(actor, parts, 0, 0);
    }

    @Override
    public CompletionStage<Void> sendToActorAsync(
        ZLinkBackendActorRef actor,
        List<Message> parts) {
        if (!routingId().equals(actor.nodeRid())) {
            return owner.sendActor(actor, parts);
        }
        return dispatchLocalActor(actor, parts, 0, 0)
            ? CompletableFuture.completedFuture(null)
            : CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
    }

    @Override
    public CompletionStage<List<Message>> requestToActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags,
        Duration timeout) {
        if (!routingId().equals(actor.nodeRid())) {
            return owner.requestActor(actor, parts, timeout);
        }
        if (!isCurrentActor(actor)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("actor is not local"));
        }
        long requestId = nextActorRequestSequence.getAndIncrement();
        CompletableFuture<List<Message>> completion = new CompletableFuture<>();
        actorRequests.put(requestId, completion);
        if (!dispatchLocalActor(actor, parts, requestId, 1)) {
            actorRequests.remove(requestId);
            return CompletableFuture.failedFuture(
                new IllegalStateException("actor Spot is not local"));
        }
        if (timeout != null && !timeout.isNegative() && !timeout.isZero()) {
            CompletableFuture.delayedExecutor(
                timeout.toNanos(),
                TimeUnit.NANOSECONDS).execute(() -> {
                    if (actorRequests.remove(requestId, completion)) {
                        completion.completeExceptionally(
                            new TimeoutException(
                                "Actor request timed out"));
                    }
                });
        }
        return completion;
    }

    @Override
    public boolean forwardActorBoundSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        List<Message> parts,
        SendFlags flags) {
        return dispatchLocalActor(
            actor,
            parts,
            0,
            0,
            sourceNodeRid,
            sourceSessionRid,
            0,
            0);
    }

    @Override
    public byte[] encodeLocalSessionActorAccepted(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long requestSequence,
        String packetName,
        Map<String, String> metadata,
        byte[] payload) {
        return owner.encodeLocalActorAccepted(
            actor,
            sourceNodeRid,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            requestSequence,
            packetName,
            metadata,
            payload);
    }

    boolean forwardBoundStreamSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        ZLinkJavaStreamSocket stream,
        List<Message> parts) {
        return forwardBoundStreamSession(
            actor,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            stream,
            null,
            parts);
    }

    boolean forwardBoundStreamSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        ZLinkJavaStreamSocket stream,
        ZLinkStreamHeader streamHeader,
        List<Message> parts) {
        boolean admitted = inStateLane(() -> {
            StreamBinding binding = streamBindings.get(actor.actorId());
            if (binding == null) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " session=" + sourceSessionRid
                    + " binding=" + sourceBindingGeneration
                    + " sequence=" + sourceSessionSequence
                    + " reason=missing-local-binding" : null);
                return false;
            }
            if (!binding.actor().equals(actor)) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " reason=actor-mismatch bindingActor="
                    + actorSummary(binding.actor()) : null);
                return false;
            }
            if (!binding.sessionRid().equals(sourceSessionRid)) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " reason=session-mismatch expected="
                    + binding.sessionRid() + " actual=" + sourceSessionRid : null);
                return false;
            }
            if (binding.bindingGeneration() != sourceBindingGeneration) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " reason=binding-generation-mismatch expected="
                    + binding.bindingGeneration() + " actual="
                    + sourceBindingGeneration : null);
                return false;
            }
            if (binding.stream() != stream) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " reason=stream-mismatch" : null);
                return false;
            }
            if (!acceptStreamBindingSequence(
                    actor.actorId(), sourceSessionSequence)) {
                streamTrace(STREAM_TRACE ? "forward reject actor=" + actorSummary(actor)
                    + " reason=local-sequence sequence="
                    + sourceSessionSequence : null);
                return false;
            }
            return true;
        });
        if (!admitted) {
            return false;
        }
        streamTrace(STREAM_TRACE ? "forward accepted actor=" + actorSummary(actor)
            + " session=" + sourceSessionRid
            + " binding=" + sourceBindingGeneration
            + " sequence=" + sourceSessionSequence
            + " request=" + (streamHeader != null
                && streamHeader.requestSequence().isPresent())
            + " local=" + routingId().equals(actor.nodeRid()) : null);
        if (!routingId().equals(actor.nodeRid())) {
            if (streamHeader != null
                && streamHeader.requestSequence().isPresent()) {
                owner.requestBoundActorAsync(
                        actor,
                        sourceSessionRid,
                        sourceBindingGeneration,
                        sourceSessionSequence,
                        streamHeader.requestSequence().orElseThrow(),
                        parts,
                        Duration.ofSeconds(30))
                    .whenComplete((reply, failure) -> {
                        if (failure == null) {
                            replyBoundStreamSession(
                                stream,
                                sourceSessionRid,
                                streamHeader,
                                reply);
                        } else {
                            replyBoundStreamError(
                                stream,
                                sourceSessionRid,
                                streamHeader,
                                failure);
                        }
                    });
                return true;
            }
            owner.sendBoundActor(
                    actor,
                    sourceSessionRid,
                    sourceBindingGeneration,
                    sourceSessionSequence,
                    parts)
                .exceptionally(failure -> {
                    streamTrace(STREAM_TRACE ? "forward bound Actor send failed actor="
                        + actorSummary(actor)
                        + " error=" + failure.getClass().getSimpleName() : null);
                    return null;
                });
            return true;
        }
        if (streamHeader != null
            && streamHeader.requestSequence().isPresent()) {
            requestToActor(actor, parts, SendFlags.DONT_WAIT,
                    Duration.ofSeconds(30))
                .whenComplete((reply, failure) -> {
                    if (failure == null) {
                        replyBoundStreamSession(
                            stream,
                            sourceSessionRid,
                            streamHeader,
                            reply);
                    } else {
                        replyBoundStreamError(
                            stream,
                            sourceSessionRid,
                            streamHeader,
                            failure);
                    }
                });
            return true;
        }
        return dispatchLocalActor(
            actor,
            parts,
            0,
            0,
            routingId(),
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence);
    }

    CompletionStage<Void> forwardBoundStreamSessionAsync(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        ZLinkJavaStreamSocket stream,
        ZLinkStreamHeader streamHeader,
        List<Message> parts) {
        if (streamHeader != null
            && streamHeader.requestSequence().isPresent()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "async bound-Session relay accepts one-way frames only"));
        }
        CompletionStage<Void> rejection = inStateLane(() -> {
            StreamBinding binding = streamBindings.get(actor.actorId());
            if (binding == null
                || !binding.actor().equals(actor)
                || !binding.sessionRid().equals(sourceSessionRid)
                || binding.bindingGeneration() != sourceBindingGeneration
                || binding.stream() != stream) {
                streamTrace(STREAM_TRACE ? "async forward reject actor=" + actorSummary(actor)
                    + " reason=bound-session-route-mismatch" : null);
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.NOT_FOUND));
            }
            if (!acceptStreamBindingSequence(
                    actor.actorId(), sourceSessionSequence)) {
                streamTrace(STREAM_TRACE ? "async forward reject actor=" + actorSummary(actor)
                    + " reason=local-sequence sequence="
                    + sourceSessionSequence : null);
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.NOT_ADMITTED));
            }
            return null;
        });
        if (rejection != null) {
            return rejection;
        }
        streamTrace(STREAM_TRACE ? "async forward accepted actor=" + actorSummary(actor)
            + " session=" + sourceSessionRid
            + " binding=" + sourceBindingGeneration
            + " sequence=" + sourceSessionSequence
            + " local=" + routingId().equals(actor.nodeRid()) : null);
        if (!routingId().equals(actor.nodeRid())) {
            return owner.sendBoundActor(
                    actor,
                    sourceSessionRid,
                    sourceBindingGeneration,
                    sourceSessionSequence,
                    parts)
                .whenComplete((ignored, failure) -> streamTrace(STREAM_TRACE ?
                    "async forward bound Actor send "
                        + (failure == null ? "accepted" : "failed")
                        + " actor=" + actorSummary(actor) : null));
        }
        return dispatchLocalActor(
                actor,
                parts,
                0,
                0,
                routingId(),
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence)
            ? CompletableFuture.completedFuture(null)
            : CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
    }

    CompletionStage<List<Message>> requestBoundStreamSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        ZLinkJavaStreamSocket stream,
        ZLinkStreamHeader streamHeader,
        List<Message> parts,
        Duration timeout) {
        CompletionStage<List<Message>> rejection = inStateLane(() -> {
            StreamBinding binding = streamBindings.get(actor.actorId());
            if (binding == null
                || !binding.actor().equals(actor)
                || !binding.sessionRid().equals(sourceSessionRid)
                || binding.bindingGeneration() != sourceBindingGeneration
                || binding.stream() != stream) {
                streamTrace(STREAM_TRACE ? "request reject actor=" + actorSummary(actor)
                    + " reason=bound-session-route-mismatch" : null);
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "STREAM session binding is no longer current"));
            }
            if (streamHeader == null
                || streamHeader.requestSequence().isEmpty()) {
                return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                        "bound Actor request requires a request sequence"));
            }
            if (!acceptStreamBindingSequence(
                    actor.actorId(), sourceSessionSequence)) {
                streamTrace(STREAM_TRACE ? "request reject actor=" + actorSummary(actor)
                    + " reason=local-sequence sequence="
                    + sourceSessionSequence : null);
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "STREAM bound-session sequence is stale"));
            }
            return null;
        });
        if (rejection != null) {
            return rejection;
        }
        if (!routingId().equals(actor.nodeRid())) {
            return owner.requestBoundActorAsync(
                actor,
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence,
                streamHeader.requestSequence().orElseThrow(),
                parts,
                timeout);
        }
        return requestToActor(
            actor,
            parts,
            SendFlags.DONT_WAIT,
            timeout);
    }

    private void replyBoundStreamSession(
        ZLinkJavaStreamSocket stream,
        RoutingId sessionRid,
        ZLinkStreamHeader requestHeader,
        List<Message> reply) {
        try {
            streamTrace(STREAM_TRACE ? "bound Session reply received parts="
                + (reply == null ? "null" : reply.size())
                + " requestSequence="
                + (requestHeader == null
                    ? "null"
                    : requestHeader.requestSequence().orElse(null)) : null);
            if (reply == null || reply.size() != 1) {
                replyBoundStreamError(
                    stream,
                    sessionRid,
                    requestHeader,
                    new IllegalArgumentException(
                        "bound Session reply requires one encoded STREAM frame"));
                return;
            }
            stream.sendBoundSessionPushAsync(
                sessionRid,
                reply,
                stream.admissionTimeout()).whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        streamTrace(STREAM_TRACE ? "bound Session reply accepted" : null);
                    } else {
                        streamTrace(STREAM_TRACE ? "bound Session reply rejected after retry: "
                            + failure.getMessage() : null);
                        replyBoundStreamError(
                            stream,
                            sessionRid,
                            requestHeader,
                            failure);
                    }
                });
        } catch (RuntimeException failure) {
            replyBoundStreamError(
                stream,
                sessionRid,
                requestHeader,
                failure);
        } finally {
            if (reply != null) {
                reply.forEach(Message::close);
            }
        }
    }

    private void replyBoundStreamError(
        ZLinkJavaStreamSocket stream,
        RoutingId sessionRid,
        ZLinkStreamHeader requestHeader,
        Throwable failure) {
        if (requestHeader == null
            || requestHeader.requestSequence().isEmpty()) {
            return;
        }
        streamTrace(STREAM_TRACE ? "bound Session error reply scheduled requestSequence="
            + requestHeader.requestSequence().orElseThrow()
            + " failure="
            + (failure == null ? "null" : failure.getClass().getSimpleName()) : null);
        try {
            byte[] errorFrame = ZLinkStreamFrameCodec.encode(
                ZLinkStreamHeaderCodec.encode(
                    ZLinkStreamHeader.createErrorResponse(
                        requestHeader,
                        requestHeader.packetName())),
                ZLinkStreamErrorPayload.encode(failure));
            try (Message error = Message.from(errorFrame)) {
                stream.sendBoundSessionPushAsync(
                    sessionRid,
                    List.of(error),
                    stream.admissionTimeout()).whenComplete(
                        (ignored, sendFailure) -> {
                            if (sendFailure == null) {
                                streamTrace(STREAM_TRACE ?
                                    "bound Session error reply accepted" : null);
                            } else {
                                streamTrace(STREAM_TRACE ?
                                    "bound Session error rejected: "
                                        + sendFailure.getMessage() : null);
                            }
                        });
            }
        } catch (RuntimeException encodingFailure) {
            streamTrace(STREAM_TRACE ? "bound Session error encoding failed: "
                + encodingFailure.getMessage() : null);
        }
    }

    @Override
    public void bindRemoteActorBoundSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        if (!isCurrentActor(actor)) {
            throw new IllegalStateException("actor is not local");
        }
    }

    @Override
    public void installRelocatingActorBoundSession(
        ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
        ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
        Objects.requireNonNull(actorRoute, "actorRoute");
        Objects.requireNonNull(session, "session");
        ZLinkBackendActorRef actor = actorRoute.actor();
        if (!routingId().equals(actor.nodeRid())) {
            throw new IllegalStateException(
                "relocating bound Session does not target this SpotNode");
        }
        RemoteStreamBinding candidate = new RemoteStreamBinding(
            session.nodeRid(),
            session.nodeGeneration(),
            session.ownerId(),
            session.ownerLeaseGeneration(),
            session.sessionRid(),
            actor,
            actorRoute.targetNodeGeneration(),
            session.bindingGeneration(),
            actorRoute.authorityOwnerGeneration(),
            actorRoute.ownerLeaseGeneration());
        inStateLane(() -> {
            RemoteStreamBinding current =
                remoteStreamBindings.get(actor.actorId());
            if (candidate.equals(current)) {
                return null;
            }
            if (current != null) {
                throw new IllegalStateException(
                    "relocating bound Session conflicts with an installed route");
            }
            remoteStreamBindings.put(actor.actorId(), candidate);
            remoteStreamSequences.putIfAbsent(actor.actorId(), 0L);
            streamBindings.remove(actor.actorId());
            return null;
        });
    }

    @Override
    public Optional<ZLinkInternalSpotNode.BoundSessionRoute>
        boundSessionRoute(ZLinkBackendActorRef actor) {
        RemoteStreamBinding remote = remoteStreamBindings.get(actor.actorId());
        if (remote == null || !remote.actor().equals(actor)) {
            return Optional.empty();
        }
        return Optional.of(
            new ZLinkInternalSpotNode.BoundSessionRoute(
                remote.sessionOwnerNodeRid(),
                remote.sessionOwnerNodeGeneration(),
                remote.sessionRid(),
                remote.bindingGeneration()));
    }

    @Override
    public void closeActorBoundSession(
        ZLinkBackendActorRef actor,
        Duration timeout) {
        if (!isCurrentActor(actor)) {
            throw new IllegalStateException("actor is not local");
        }
        StreamBinding binding = streamBindings.remove(actor.actorId());
        if (binding != null) {
            discardStreamSession(
                binding.sessionRid(),
                binding.actor(),
                binding.bindingGeneration(),
                binding.stream());
        }
        streamBindingSequences.remove(actor.actorId());
        remoteStreamBindings.remove(actor.actorId());
        remoteStreamSequences.remove(actor.actorId());
    }

    @Override
    public void close() {
        spots.values().forEach(ZLinkJavaRawSpot::close);
        spots.clear();
        actors.clear();
        actorSpots.clear();
        actorMembershipEpochs.clear();
        actorRequests.values().forEach(completion ->
            completion.completeExceptionally(
                new IllegalStateException("raw SpotNode is closed")));
        actorRequests.clear();
        actorRemoteReplies.clear();
        streamBindings.clear();
        streamBindingSequences.clear();
        remoteStreamBindings.clear();
        remoteStreamSequences.clear();
        spotAuthorities.clear();
        actorAuthorities.clear();
        instanceAuthorities.clear();
        instanceSpots.closeAll();
        owner.close();
    }

    void rekeySpot(
        ZLinkJavaRawSpot spot,
        String previous,
        String current) {
        if (previous.equals(current)) {
            return;
        }
        inStateLane(() -> {
            ZLinkJavaRawSpot conflict = spots.get(current);
            if (conflict != null && conflict != spot) {
                throw new IllegalStateException(
                    "Spot routing id is already registered: " + current);
            }
            spots.remove(previous, spot);
            spots.put(current, spot);
            return null;
        });
    }

    void removeSpot(ZLinkJavaRawSpot spot) {
        spots.remove(spot.spotId(), spot);
        spotAuthorities.remove(new SpotAuthorityKey(
            routingId(),
            spot.spotId(),
            spot.lifecycleGeneration()));
    }

    void rememberSpotAuthority(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration,
        long authorityOwnerGeneration) {
        rememberSpotAuthority(
            targetNodeRid,
            spotId,
            objectGeneration,
            authorityOwnerGeneration,
            owner.localAuthorityLeaseGeneration());
    }

    void rememberSpotAuthority(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        if (targetNodeRid == null
            || spotId == null
            || objectGeneration <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            throw new IllegalArgumentException(
                "Spot authority generations must be positive");
        }
        spotAuthorities.put(
            new SpotAuthorityKey(
                targetNodeRid, spotId, objectGeneration),
            new AuthorityFence(
                authorityOwnerGeneration, ownerLeaseGeneration));
    }

    long spotAuthorityOwnerGeneration(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration) {
        AuthorityFence fence = spotAuthorities.get(
            new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration));
        return fence == null ? 0L : fence.authorityOwnerGeneration();
    }

    long spotAuthorityOwnerLeaseGeneration(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration) {
        AuthorityFence fence = spotAuthorities.get(
            new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration));
        return fence == null ? 0L : fence.ownerLeaseGeneration();
    }

    void forgetSpotAuthority(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration,
        long authorityOwnerGeneration) {
        spotAuthorities.remove(
            new SpotAuthorityKey(
                targetNodeRid, spotId, objectGeneration),
            authorityOwnerGeneration);
    }

    @Override
    public long actorAuthorityOwnerGeneration(ZLinkBackendActorRef actor) {
        AuthorityFence fence = actorAuthorities.get(
            new ActorAuthorityKey(
                actor.nodeRid(), actor.actorId(), actor.generation()));
        return fence == null ? 0L : fence.authorityOwnerGeneration();
    }

    @Override
    public long actorAuthorityOwnerLeaseGeneration(
        ZLinkBackendActorRef actor) {
        AuthorityFence fence = actorAuthorities.get(
            new ActorAuthorityKey(
                actor.nodeRid(), actor.actorId(), actor.generation()));
        return fence == null ? 0L : fence.ownerLeaseGeneration();
    }

    boolean enqueueRemoteSpot(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteSpot(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                sourceNodeRid, 1, "test-owner", 1),
            header,
            metadata,
            new byte[0],
            parts,
            reply);
    }

    boolean enqueueRemoteSpot(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteSpot(
            source,
            header,
            metadata,
            acceptedJournalRecord,
            parts,
            null,
            reply);
    }

    boolean enqueueRemoteSpot(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply) {
        return enqueueRemoteSpotLazy(
            source, header, metadata, () -> acceptedJournalRecord,
            acceptedJournalRecord.length, parts, contentType, reply);
    }

    boolean enqueueRemoteSpotLazy(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        Supplier<byte[]> acceptedJournalRecord,
        int acceptedJournalRecordSizeHint,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply) {
        return enqueueRemoteSpotLazy(
            source,
            header,
            metadata,
            acceptedJournalRecord,
            acceptedJournalRecordSizeHint,
            parts,
            contentType,
            reply,
            ignored -> { },
            () -> { });
    }

    boolean enqueueRemoteSpotLazy(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        Supplier<byte[]> acceptedJournalRecord,
        int acceptedJournalRecordSizeHint,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        return enqueueRemoteSpotLazy(
            source, header, metadata, acceptedJournalRecord,
            acceptedJournalRecordSizeHint, parts, contentType, reply, failure,
            () -> { });
    }

    boolean enqueueRemoteSpotLazy(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.SpotMessage header,
        byte[] metadata,
        Supplier<byte[]> acceptedJournalRecord,
        int acceptedJournalRecordSizeHint,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure,
        Runnable terminalRelease) {
        Objects.requireNonNull(terminalRelease, "terminalRelease");
        ZLinkInternalSpotNode.RelocationStagingIngressHandler staging =
            relocationStagingIngressHandler;
        if (staging != null && staging.handleSpot(
            source,
            header,
            metadata,
            acceptedJournalRecord,
            acceptedJournalRecordSizeHint,
            parts,
                contentType,
                reply,
                failure)) {
            terminalRelease.run();
            return true;
        }
        ZLinkServiceM6BWireCodec.SpotRouteFence forwarded =
            relocationSpotForwards.get(header.target());
        if (forwarded != null) {
            boolean accepted = owner.forwardRelocationSpot(
                header,
                forwarded,
                metadata,
                parts,
                reply,
                failure);
            if (accepted) {
                terminalRelease.run();
            }
            return accepted;
        }
        ZLinkJavaRawSpot target = localSpot(
            routingId(),
            header.target().spotId(),
            header.target().spotGeneration());
        if (target == null
            || spotAuthorityOwnerGeneration(
                routingId(),
                header.target().spotId(),
                header.target().spotGeneration())
                != header.target().authorityOwnerGeneration()) {
            return false;
        }
        target.enqueueRoute(systems.zlink.framework.runtime.internal.backend
            .ZLinkBackendReceived.lazyJournal(
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendRequestResult.OK,
                Optional.of(source.sourceNodeRid()),
                Optional.of(header.sourceSpotId()),
                Optional.ofNullable(header.correlation()),
                metadata,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                parts,
                header.request() ? reply : null,
                  terminalRelease,
                  contentType));
        return true;
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            sourceNodeRid,
            0,
            header,
            new byte[0],
            parts,
            reply);
    }

    boolean enqueueRemoteActor(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            source.sourceNodeRid(),
            source.sourceNodeGeneration(),
            header,
            new byte[0],
            parts,
            reply);
    }

    boolean enqueueRemoteActor(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            source.sourceNodeRid(),
            source.sourceNodeGeneration(),
            header,
            acceptedJournalRecord,
            parts,
            reply);
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            sourceNodeRid,
            sourceNodeGeneration,
            header,
            new byte[0],
            parts,
            reply);
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            sourceNodeRid,
            sourceNodeGeneration,
            header,
            acceptedJournalRecord,
            parts,
            null,
            reply);
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply) {
        return enqueueRemoteActor(
            sourceNodeRid,
            sourceNodeGeneration,
            header,
                        acceptedJournalRecord,
            parts,
            contentType,
            reply,
            ignored -> { });
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        return enqueueRemoteActor(
            sourceNodeRid, sourceNodeGeneration, header,
            () -> acceptedJournalRecord, parts, contentType, reply, failure);
    }

    boolean enqueueRemoteActor(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        Supplier<byte[]> acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        return enqueueRemoteActor(
            new ZLinkInternalMeshNode.PeerAuthorityFence(
                sourceNodeRid,
                Math.max(1, sourceNodeGeneration),
                "legacy:" + sourceNodeRid,
                1),
            header,
            acceptedJournalRecord,
            parts,
            contentType,
            reply,
            failure);
    }

    boolean enqueueRemoteActor(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        Supplier<byte[]> acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        return enqueueRemoteActor(
            source, header, acceptedJournalRecord, parts, contentType, reply,
            failure, () -> { });
    }

    boolean enqueueRemoteActor(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        Supplier<byte[]> acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure,
        Runnable terminalRelease) {
        RoutingId sourceNodeRid = source.sourceNodeRid();
        long sourceNodeGeneration = source.sourceNodeGeneration();
        ZLinkBackendActorRef actor = header.target().actor();
        ZLinkInternalSpotNode.RelocationStagingIngressHandler staging =
            relocationStagingIngressHandler;
        if (staging != null && staging.handleActor(
            source,
            header,
            acceptedJournalRecord,
            parts,
            contentType,
            reply,
            failure)) {
            terminalRelease.run();
            return true;
        }
        ZLinkServiceM6BWireCodec.ActorRouteFence forwarded =
            relocationActorForwards.get(header.target());
        if (forwarded != null) {
            boolean accepted = owner.forwardRelocationActor(
                header,
                forwarded,
                parts,
                reply,
                failure);
            if (accepted) {
                terminalRelease.run();
            }
            return accepted;
        }
        ZLinkInternalSpotNode.MessageFollowRelayHandler relay =
            messageFollowRelayHandler;
        if (relay != null) {
            try {
                if (relay.handle(
                    sourceNodeRid,
                    sourceNodeGeneration,
                    header,
                    acceptedJournalRecord.get(),
                    parts,
                    contentType,
                    reply,
                    failure,
                    terminalRelease)) {
                    return true;
                }
            } catch (RuntimeException relayFailure) {
                streamTrace(STREAM_TRACE ? "remote enqueue relay failed actor="
                    + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " error=" + relayFailure.getClass().getSimpleName() : null);
            }
        }
        if (!isCurrentActor(actor)) {
            streamTrace(STREAM_TRACE ? "remote enqueue reject actor=" + actorSummary(actor)
                + " source=" + sourceNodeRid
                + " reason=not-current-actor current="
                + actorSummary(actors.get(actor == null ? "" : actor.actorId())) : null);
            return false;
        }
        long currentAuthority = actorAuthorityOwnerGeneration(actor);
        long currentOwnerLease = actorAuthorityOwnerLeaseGeneration(actor);
        if (currentAuthority != header.target().authorityOwnerGeneration()
            || currentOwnerLease
                != header.target().ownerLeaseGeneration()) {
            streamTrace(STREAM_TRACE ? "remote enqueue reject actor=" + actorSummary(actor)
                + " source=" + sourceNodeRid
                + " reason=authority-fence expected=" + currentAuthority
                + "/" + currentOwnerLease
                + " actual=" + header.target().authorityOwnerGeneration()
                + "/" + header.target().ownerLeaseGeneration() : null);
            return false;
        }
        if (header.boundSession() != null) {
            RemoteStreamBinding binding =
                remoteStreamBindings.get(actor.actorId());
            if (binding == null) {
                streamTrace(STREAM_TRACE ? "remote enqueue reject actor="
                    + actorSummary(actor) + " source=" + sourceNodeRid
                    + " reason=missing-remote-binding" : null);
                return false;
            }
            if (!binding.actor().equals(actor)
                || !binding.sessionOwnerNodeRid().equals(sourceNodeRid)
                || binding.sessionOwnerNodeGeneration()
                    != sourceNodeGeneration
                || !binding.sessionRid().equals(
                    header.boundSession().sourceSessionRid())
                || binding.bindingGeneration()
                    != header.boundSession().sourceBindingGeneration()) {
                streamTrace(STREAM_TRACE ? "remote enqueue reject actor="
                    + actorSummary(actor) + " source=" + sourceNodeRid
                    + " reason=remote-binding-mismatch binding="
                    + remoteBindingSummary(binding)
                    + " tailSession="
                    + header.boundSession().sourceSessionRid()
                    + " tailGeneration="
                    + header.boundSession().sourceBindingGeneration() : null);
                return false;
            }
            if (!acceptRemoteStreamSequence(
                    actor.actorId(),
                    header.boundSession().sourceSessionSequence())) {
                streamTrace(STREAM_TRACE ? "remote enqueue reject actor="
                    + actorSummary(actor) + " source=" + sourceNodeRid
                    + " reason=remote-sequence sequence="
                    + header.boundSession().sourceSessionSequence() : null);
                return false;
            }
        }
        String targetSpotId = actorSpots.get(actor.actorId());
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            streamTrace(STREAM_TRACE ? "remote enqueue reject actor=" + actorSummary(actor)
                + " source=" + sourceNodeRid
                + " reason=missing-target-spot spot=" + targetSpotId : null);
            return false;
        }
        streamTrace(STREAM_TRACE ? "remote enqueue accepted actor=" + actorSummary(actor)
            + " source=" + sourceNodeRid
            + " request=" + header.request()
            + " sequence=" + (header.boundSession() == null
                ? 0
                : header.boundSession().sourceSessionSequence())
            + " spot=" + targetSpotId : null);
        long requestId = header.request()
            ? nextActorRequestSequence.getAndIncrement()
            : 0;
        if (header.request()) {
            actorRemoteReplies.put(requestId, reply);
            CompletableFuture.delayedExecutor(
                30,
                TimeUnit.SECONDS).execute(
                    () -> actorRemoteReplies.remove(requestId, reply));
        }
        List<ZLinkBackendActorReceived> messages =
            new ArrayList<>(parts.size());
        if (parts.isEmpty()) {
            terminalRelease.run();
            target.enqueueActor(messages);
            return true;
        }
        //  A relocation cut can finish between the forward lookup above and the
        //  Actor queue admission below. The queue then rejects the arrival with
        //  RelocatedOwnerException; routing :222/:240 requires that arrival to
        //  reach the new owner through the forward that the source installs
        //  before the cut, not to be dropped. The Spot runtime still owns the
        //  surviving header/payload copies at that point, so it calls back here.
        ZLinkBackendActorReceived.RelocationRedirect relocationRedirect =
            parts.size() == 2
                ? (headerFrame, payloadFrame) -> redirectRelocatedActor(
                    header, headerFrame, payloadFrame, reply, failure)
                : null;
        AtomicInteger remainingTerminals = new AtomicInteger(parts.size());
        Runnable partTerminal = () -> {
            if (remainingTerminals.decrementAndGet() == 0) {
                terminalRelease.run();
            }
        };
        for (int index = 0; index < parts.size(); index++) {
            messages.add(index == 0
                ? ZLinkBackendActorReceived.lazyJournal(
                    actor,
                    sourceNodeRid,
                    header.boundSession() == null
                        ? null
                        : header.boundSession().sourceSessionRid(),
                    Optional.ofNullable(header.correlation()),
                    requestId,
                    header.request() ? 1 : 0,
                    parts.get(index),
                    index + 1 < parts.size(),
                      acceptedJournalRecord,
                      contentType,
                      partTerminal,
                      relocationRedirect)
                : new ZLinkBackendActorReceived(
                actor,
                sourceNodeRid,
                header.boundSession() == null
                    ? null
                    : header.boundSession().sourceSessionRid(),
                Optional.ofNullable(header.correlation()),
                requestId,
                header.request() ? 1 : 0,
                parts.get(index),
                  index + 1 < parts.size(),
                  new byte[0],
                  contentType,
                  partTerminal));
        }
        reportRelocatedActorIngress(
            target.enqueueActor(messages), header, requestId, reply, failure);
        return true;
    }

    /**
     * Re-routes one post-cut Actor arrival through the relocation forward the
     * source installs before its queue cut finishes. Returns whether the
     * forward accepted it; a refusal leaves the caller with the stale terminal
     * (routing :244 forbids re-submitting the operation here).
     */
    private boolean redirectRelocatedActor(
        ZLinkServiceM6BWireCodec.ActorMessage header,
        Message headerFrame,
        Message payloadFrame,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        ZLinkServiceM6BWireCodec.ActorRouteFence forwarded =
            relocationActorForwards.get(header.target());
        if (forwarded == null) {
            return false;
        }
        List<Message> parts = new ArrayList<>(2);
        parts.add(Message.from(headerFrame));
        parts.add(Message.from(payloadFrame));
        boolean accepted;
        try {
            accepted = owner.forwardRelocationActor(
                header, forwarded, parts, reply, failure);
        } catch (RuntimeException error) {
            parts.forEach(Message::close);
            throw error;
        }
        if (!accepted) {
            parts.forEach(Message::close);
        }
        return accepted;
    }

    /**
     * Surfaces a post-cut Actor queue rejection that the relocation forward did
     * not absorb. Reporting it keeps the ingress from claiming a delivery it
     * never made; other admission failures keep their existing handling.
     */
    private void reportRelocatedActorIngress(
        CompletionStage<Void> dispatched,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        long requestId,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        if (dispatched == null) {
            return;
        }
        dispatched.whenComplete((ignored, error) -> {
            if (error == null) {
                return;
            }
            Throwable cause = error;
            while (cause instanceof CompletionException
                && cause.getCause() != null) {
                cause = cause.getCause();
            }
            if (!(cause instanceof systems.zlink.framework.execution
                .ZLinkSerialExecutionQueue.RelocatedOwnerException)) {
                return;
            }
            streamTrace(STREAM_TRACE ? "remote enqueue post-cut drop actor="
                + actorSummary(header.target().actor()) : null);
            if (header.request()) {
                actorRemoteReplies.remove(requestId, reply);
            }
            if (failure != null) {
                failure.accept(cause);
            }
        });
    }

    private boolean acceptRemoteStreamSequence(
        String actorId,
        long sequence) {
        AtomicBoolean accepted = new AtomicBoolean();
        remoteStreamSequences.computeIfPresent(actorId, (ignored, current) -> {
            if (sequence > current) {
                accepted.set(true);
                return sequence;
            }
            return current;
        });
        return accepted.get();
    }

    long allocateStreamBindingGeneration() {
        return inStateLane(this::allocateStreamBindingGenerationOnLane);
    }

    private long allocateStreamBindingGenerationOnLane() {
        long current = nextStreamBindingGeneration.get();
        if (current == 0) {
            current = owner.bindingGenerationSeed();
        }
        if (current <= 0 || current == Long.MAX_VALUE) {
            throw new IllegalStateException(
                "STREAM binding generation is exhausted");
        }
        nextStreamBindingGeneration.set(current + 1);
        return current;
    }

    CompletionStage<Void> bindStreamSession(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        ZLinkJavaStreamSocket stream,
        Duration timeout) {
        if (bindingGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "binding generation must be positive"));
        }
        long authorityOwnerGeneration =
            actorAuthorityOwnerGeneration(actor);
        StreamBinding binding = new StreamBinding(
            sessionRid,
            actor,
            bindingGeneration,
            authorityOwnerGeneration,
            stream);
        if (isCurrentActor(actor)) {
            StreamBinding previous = streamBindings.get(actor.actorId());
            if (!installStreamBinding(binding)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor has a newer STREAM session binding"));
            }
            remoteStreamBindings.remove(actor.actorId());
            if (previous != null && !previous.equals(binding)) {
                notifyBoundSessionReplaced(previous);
            }
            return CompletableFuture.completedFuture(null);
        }
        return owner.bindRemoteStreamSession(
            sessionRid,
            actor,
            authorityOwnerGeneration,
            bindingGeneration,
            true,
            timeout)
            .thenRun(() -> {
                if (!installStreamBinding(binding)) {
                    throw new IllegalStateException(
                        "Actor has a newer STREAM session binding");
                }
            });
    }

    CompletionStage<Void> unbindStreamSession(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        ZLinkJavaStreamSocket stream,
        Duration timeout) {
        StreamBinding current = streamBindings.get(actor.actorId());
        if (current == null
            || !current.sessionRid().equals(sessionRid)
            || !current.actor().equals(actor)
            || current.bindingGeneration() != bindingGeneration
            || current.stream() != stream) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "STREAM session binding is stale"));
        }
        if (isCurrentActor(actor)) {
            removeStreamBinding(
                sessionRid, actor, bindingGeneration, stream);
            return CompletableFuture.completedFuture(null);
        }
        return owner.bindRemoteStreamSession(
            sessionRid,
            actor,
            current.authorityOwnerGeneration(),
            bindingGeneration,
            false,
            timeout)
            .thenRun(() -> removeStreamBinding(
                sessionRid, actor, bindingGeneration, stream));
    }

    void discardStreamSession(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        ZLinkJavaStreamSocket stream) {
        removeStreamBinding(
            sessionRid, actor, bindingGeneration, stream);
    }

    boolean acceptRemoteStreamBinding(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionBind command) {
        return acceptRemoteStreamBinding(
            sourceNodeRid,
            sourceNodeGeneration,
            sourceNodeRid.toString(),
            1L,
            command);
    }

    boolean acceptRemoteStreamBinding(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sessionOwnerId,
        long sessionOwnerLeaseGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionBind command) {
        ZLinkServiceM6BWireCodec.ActorRouteFence route = command.actor();
        ZLinkBackendActorRef actor = route.actor();
        RemoteStreamBinding candidate = new RemoteStreamBinding(
            sourceNodeRid,
            sourceNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            command.sessionRid(),
            actor,
            route.targetNodeGeneration(),
            command.bindingGeneration(),
            route.authorityOwnerGeneration(),
            route.ownerLeaseGeneration());
        RemoteBindingAdmission admission = inStateLane(() -> {
            if (!routingId().equals(actor.nodeRid())) {
                streamTrace(STREAM_TRACE ? "remote bind reject actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " active=" + command.active()
                    + " reason=actor-node-mismatch local=" + routingId() : null);
                return RemoteBindingAdmission.rejected();
            }
            RemoteStreamBinding current =
                remoteStreamBindings.get(actor.actorId());
            if (command.active() && candidate.equals(current)) {
                return RemoteBindingAdmission.accepted(null);
            }
            if (route.targetNodeGeneration() != owner.lifecycleGeneration()) {
                streamTrace(STREAM_TRACE ? "remote bind reject actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " active=" + command.active()
                    + " reason=node-generation expected="
                    + owner.lifecycleGeneration() + " actual="
                    + route.targetNodeGeneration() : null);
                return RemoteBindingAdmission.rejected();
            }
            if (!command.active()) {
                if (remoteStreamBindings.remove(
                        actor.actorId(), candidate)) {
                    remoteStreamSequences.remove(actor.actorId());
                }
                streamTrace(STREAM_TRACE ? "remote bind removed actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " binding=" + command.bindingGeneration() : null);
                return RemoteBindingAdmission.accepted(null);
            }
            if (!isCurrentActor(actor)) {
                streamTrace(STREAM_TRACE ? "remote bind reject actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " active=true reason=not-current-actor" : null);
                return RemoteBindingAdmission.rejected();
            }
            if (actorAuthorityOwnerGeneration(actor)
                    != route.authorityOwnerGeneration()) {
                streamTrace(STREAM_TRACE ? "remote bind reject actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " active=true reason=authority-generation expected="
                    + actorAuthorityOwnerGeneration(actor) + " actual="
                    + route.authorityOwnerGeneration() : null);
                return RemoteBindingAdmission.rejected();
            }
            if (current != null
                && current.sameSessionOwnerEpoch(candidate)
                && current.bindingGeneration()
                    >= candidate.bindingGeneration()) {
                streamTrace(STREAM_TRACE ? "remote bind reject actor=" + actorSummary(actor)
                    + " source=" + sourceNodeRid
                    + " reason=stale-binding current="
                    + remoteBindingSummary(current) + " candidate="
                    + remoteBindingSummary(candidate) : null);
                return RemoteBindingAdmission.rejected();
            }
            remoteStreamBindings.put(actor.actorId(), candidate);
            remoteStreamSequences.put(actor.actorId(), 0L);
            streamBindings.remove(actor.actorId());
            return RemoteBindingAdmission.accepted(current);
        });
        if (!admission.accepted()) {
            return false;
        }
        streamTrace(STREAM_TRACE ? "remote bind installed actor=" + actorSummary(actor)
            + " source=" + sourceNodeRid
            + " binding=" + command.bindingGeneration() : null);
        if (admission.replaced() != null) {
            notifyBoundSessionReplaced(admission.replaced());
        }
        return true;
    }

    void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] spot=" + name()
                + " rid=" + routingId() + " " + message);
        }
    }

    boolean streamTraceEnabled() {
        return STREAM_TRACE;
    }

    private static String actorSummary(ZLinkBackendActorRef actor) {
        return actor == null
            ? "null"
            : actor.actorId() + "@" + actor.nodeRid()
                + "/g=" + actor.generation();
    }

    private static String remoteBindingSummary(RemoteStreamBinding binding) {
        return binding.sessionRid() + "@" + binding.sessionOwnerNodeRid()
            + "/ownerGen=" + binding.sessionOwnerNodeGeneration()
            + "/binding=" + binding.bindingGeneration();
    }

    private void notifyBoundSessionReplaced(StreamBinding retired) {
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement =
            new ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                actorAuthorityFence(retired.actor()),
                new ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                    owner.routingId(),
                    ownerLifecycleGeneration(),
                    owner.localAuthorityOwnerId(),
                    owner.localAuthorityLeaseGeneration(),
                    retired.sessionRid(),
                    retired.bindingGeneration()));
        owner.sendBoundSessionReplaced(
            owner.routingId(), replacement);
    }

    private void notifyBoundSessionReplaced(RemoteStreamBinding retired) {
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement =
            new ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                actorAuthorityFence(retired.actor()),
                new ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                    retired.sessionOwnerNodeRid(),
                    retired.sessionOwnerNodeGeneration(),
                    retired.sessionOwnerId(),
                    retired.sessionOwnerLeaseGeneration(),
                    retired.sessionRid(),
                    retired.bindingGeneration()));
        owner.sendBoundSessionReplaced(
            retired.sessionOwnerNodeRid(), replacement);
    }

    private ZLinkServiceM6BWireCodec.ActorRouteFence actorAuthorityFence(
        ZLinkBackendActorRef actor) {
        return new ZLinkServiceM6BWireCodec.ActorRouteFence(
            actor,
            ownerLifecycleGeneration(),
            actorAuthorityOwnerGeneration(actor),
            actorAuthorityOwnerLeaseGeneration(actor));
    }

    private long ownerLifecycleGeneration() {
        try {
            return owner.lifecycleGeneration();
        } catch (IllegalStateException notStarted) {
            return owner.bindingGenerationSeed();
        }
    }

    boolean acceptBoundSessionPush(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.BoundSessionSend command,
        List<Message> parts) {
        StreamBinding binding =
            streamBindings.get(command.actor().actor().actorId());
        String packetName = boundSessionPacketName(parts);
        boolean accepted = binding != null
            && binding.actor().equals(command.actor().actor())
            && binding.bindingGeneration()
                == command.expectedBindingGeneration()
            && binding.authorityOwnerGeneration()
                == command.actor().authorityOwnerGeneration()
            && command.actor().actor().nodeRid().equals(sourceNodeRid)
            && command.actor().targetNodeGeneration()
                == sourceNodeGeneration
            && binding.stream().sendBoundSessionPush(
                binding.sessionRid(), parts, SendFlags.DONT_WAIT);
        streamTrace(STREAM_TRACE ? "bound session push "
            + (accepted ? "accepted" : "rejected")
            + " actor=" + actorSummary(command.actor().actor())
            + " source=" + sourceNodeRid
            + " binding=" + command.expectedBindingGeneration()
            + " hasBinding=" + (binding != null)
            + " packet=" + packetName : null);
        return accepted;
    }

    private static String boundSessionPacketName(List<Message> parts) {
        if (!STREAM_TRACE || parts == null || parts.size() != 1) {
            return "unknown";
        }
        try {
            return ZLinkStreamFrameCodec.tryDecode(parts.getFirst().toByteArray())
                .map(ZLinkStreamFrameCodec.DecodedFrame::header)
                .map(ZLinkStreamHeaderCodec::decodeOrPlain)
                .map(ZLinkStreamHeader::name)
                .orElse("unknown");
        } catch (RuntimeException invalidFrame) {
            return "invalid";
        }
    }

    private void removeStreamBinding(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        ZLinkJavaStreamSocket stream) {
        inStateLane(() -> {
            StreamBinding current = streamBindings.get(actor.actorId());
            if (current != null
                && current.sessionRid().equals(sessionRid)
                && current.actor().equals(actor)
                && current.bindingGeneration() == bindingGeneration
                && current.stream() == stream) {
                streamBindings.remove(actor.actorId(), current);
                streamBindingSequences.remove(actor.actorId());
            }
            return null;
        });
    }

    private boolean installStreamBinding(
        StreamBinding binding) {
        return inStateLane(() -> {
            StreamBinding current =
                streamBindings.get(binding.actor().actorId());
            if (current != null
                && current.bindingGeneration()
                    >= binding.bindingGeneration()
                && !current.equals(binding)) {
                return false;
            }
            if (!binding.equals(current)) {
                streamBindings.put(binding.actor().actorId(), binding);
                streamBindingSequences.put(binding.actor().actorId(), 0L);
            }
            return true;
        });
    }

    private boolean acceptStreamBindingSequence(
        String actorId,
        long sequence) {
        long current =
            streamBindingSequences.getOrDefault(actorId, -1L);
        if (sequence <= current) {
            return false;
        }
        streamBindingSequences.put(actorId, sequence);
        return true;
    }

    void registerInstanceSpotType(String stableType) {
        instanceSpots.register(stableType, this::createSpot);
    }

    void registerInstanceSpotType(
        String stableType,
        ZLinkInternalMeshNode.InstanceSpotActivationHandler handler) {
        instanceSpots.register(
            stableType,
            this::createSpot,
            (selectedType, spotId, generation, backendSpot) -> {
                InstanceAuthority authority = instanceAuthorities.get(spotId);
                if (authority == null
                    || authority.route().objectGeneration() != generation) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Instance Spot authority is missing or stale"));
                }
                return handler.activate(
                    selectedType, authority.route(), backendSpot);
            });
    }

    CompletionStage<ZLinkJavaInstanceSpotRegistry.Activation>
        activateInstanceSpot(
            String spotId,
            String stableType) {
        ZLinkJavaRawSpot current = spots.get(spotId);
        long generation = current == null
            ? nextGeneration.getAndIncrement()
            : current.lifecycleGeneration();
        return instanceSpots.activate(spotId, stableType, generation);
    }

    boolean closeInstanceSpot(String spotId, long generation) {
        boolean closed = instanceSpots.close(spotId, generation);
        streamTrace(STREAM_TRACE ? "instance-close spot=" + spotId
            + " generation=" + generation + " closed=" + closed : null);
        return closed;
    }

    ZLinkBackendSpot localSpot(String spotId) {
        return spots.get(spotId);
    }

    void registerInstanceSpotAuthority(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        Objects.requireNonNull(route, "route");
        if (!routingId().equals(route.targetNodeRid())) {
            throw new IllegalArgumentException(
                "Instance authority target is not local");
        }
        InstanceAuthority authority =
            new InstanceAuthority(stableType, route);
        InstanceAuthority current = instanceAuthorities.putIfAbsent(
            route.targetSpotId(), authority);
        if (current != null && !current.equals(authority)) {
            throw new IllegalStateException(
                "Instance Spot authority fence changed without replacement");
        }
    }

    void reconcileInstanceSpotAuthority(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        Objects.requireNonNull(route, "route");
        if (!routingId().equals(route.targetNodeRid())) {
            return;
        }
        instanceAuthorities.put(
            route.targetSpotId(),
            new InstanceAuthority(stableType, route));
    }

    void forgetInstanceSpotAuthority(
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        InstanceAuthority current =
            instanceAuthorities.get(route.targetSpotId());
        if (current != null
            && sameInstanceAuthorityFence(current.route(), route)) {
            instanceAuthorities.remove(
                route.targetSpotId(), current);
        }
    }

    boolean enqueueRemoteInstanceSpot(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
        byte[] metadata,
        List<Message> parts,
        Consumer<List<Message>> reply) {
        return enqueueRemoteInstanceSpot(
            sourceNodeRid,
            header,
            metadata,
            parts,
            null,
            reply,
            ignored -> { });
    }

    boolean enqueueRemoteInstanceSpot(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
        byte[] metadata,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply) {
        return enqueueRemoteInstanceSpot(
            sourceNodeRid,
            header,
            metadata,
            parts,
            contentType,
            reply,
            ignored -> { });
    }

    boolean enqueueRemoteInstanceSpot(
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
        byte[] metadata,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        InstanceAuthority authority = selectRemoteInstanceSpotAuthority(
            header.stableType(), header.route());
        if (authority == null
            || !authority.stableType().equals(header.stableType())
            || !sameInstanceAuthorityFence(
                authority.route(), header.route())) {
            return false;
        }
        try {
            owner.executeApplication(() -> {
                CompletionStage<ZLinkJavaInstanceSpotRegistry.Activation> activation;
                try {
                    activation = instanceSpots.activate(
                        header.route().targetSpotId(),
                        authority.stableType(),
                        header.route().objectGeneration());
                } catch (Throwable activationFailure) {
                    closeRemoteInstancePayload(parts);
                    failure.accept(activationFailure);
                    return;
                }
                activation.whenComplete((value, activationFailure) -> {
                    if (activationFailure != null) {
                        closeRemoteInstancePayload(parts);
                        failure.accept(activationFailure);
                        return;
                    }
                    if (value == null) {
                        closeRemoteInstancePayload(parts);
                        failure.accept(new IllegalStateException(
                            "Instance Spot activation completed without a Spot"));
                        return;
                    }
                    if (!(value.spot() instanceof ZLinkJavaRawSpot target)
                        || target.lifecycleGeneration()
                            != header.route().objectGeneration()) {
                        closeRemoteInstancePayload(parts);
                        failure.accept(new IllegalStateException(
                            "Instance Spot activation returned a stale generation"));
                        return;
                    }
                    ZLinkBackendReceived received =
                        new ZLinkBackendReceived(
                            ZLinkBackendRequestResult.OK,
                            Optional.of(sourceNodeRid),
                            Optional.ofNullable(header.sourceSpotId()),
                            Optional.ofNullable(header.replyRouteId()),
                            metadata,
                            new byte[0],
                            parts,
                            header.request() ? reply : null,
                              () -> { },
                              contentType);
                    target.enqueueRoute(received).whenComplete(
                        (ignored, enqueueFailure) -> {
                            if (enqueueFailure != null) {
                                failure.accept(enqueueFailure);
                            }
                        });
                });
            });
            return true;
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    private static void closeRemoteInstancePayload(List<Message> parts) {
        parts.forEach(Message::close);
    }

    private InstanceAuthority selectRemoteInstanceSpotAuthority(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        InstanceAuthority candidate = new InstanceAuthority(stableType, route);
        InstanceAuthority selected = instanceAuthorities.compute(
            route.targetSpotId(),
            (spotId, current) -> {
                if (current == null
                    || sameInstanceAuthorityFence(
                        current.route(), route)) {
                    return current == null ? candidate : current;
                }
                if (!current.stableType().equals(stableType)
                    || !isNewerInstanceAuthorityFence(
                        route, current.route())) {
                    return current;
                }
                return candidate;
            });
        streamTrace(STREAM_TRACE ? "instance-authority-select spot="
            + route.targetSpotId()
            + " candidate-object=" + route.objectGeneration()
            + " selected-object=" + selected.route().objectGeneration()
            + " accepted=" + selected.route().equals(route) : null);
        return selected;
    }

    static boolean isNewerInstanceAuthorityFence(
        ZLinkServiceM6BWireCodec.InstanceRouteFence candidate,
        ZLinkServiceM6BWireCodec.InstanceRouteFence current) {
        if (!candidate.targetNodeRid().equals(current.targetNodeRid())
            || !candidate.targetSpotId().equals(current.targetSpotId())
            || candidate.storeVersion().equals(current.storeVersion())) {
            return false;
        }
        boolean targetNodeGenerationNotOlder =
            candidate.targetNodeGeneration()
                >= current.targetNodeGeneration();
        boolean objectGenerationNotOlder =
            candidate.objectGeneration() >= current.objectGeneration();
        boolean ownerGenerationNotOlder =
            candidate.authorityOwnerGeneration()
                >= current.authorityOwnerGeneration();
        boolean leaseGenerationNotOlder =
            candidate.leaseGeneration() >= current.leaseGeneration();
        boolean advanced =
            candidate.targetNodeGeneration()
                > current.targetNodeGeneration()
            || candidate.objectGeneration() > current.objectGeneration()
            || candidate.authorityOwnerGeneration()
                > current.authorityOwnerGeneration()
            || candidate.leaseGeneration() > current.leaseGeneration();
        return targetNodeGenerationNotOlder
            && objectGenerationNotOlder
            && ownerGenerationNotOlder
            && leaseGenerationNotOlder
            && advanced;
    }

    private static boolean sameInstanceAuthorityFence(
        ZLinkServiceM6BWireCodec.InstanceRouteFence left,
        ZLinkServiceM6BWireCodec.InstanceRouteFence right) {
        return left.targetNodeRid().equals(right.targetNodeRid())
            && left.targetNodeGeneration()
                == right.targetNodeGeneration()
            && left.targetSpotId().equals(right.targetSpotId())
            && left.objectGeneration() == right.objectGeneration()
            && left.ownerId().equals(right.ownerId())
            && left.authorityOwnerGeneration()
                == right.authorityOwnerGeneration()
            && left.leaseGeneration() == right.leaseGeneration();
    }

    private boolean dispatchLocalActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        long requestId,
        int flags) {
        return dispatchLocalActor(
            actor, parts, requestId, flags, routingId(), null);
    }

    private boolean dispatchLocalActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        long requestId,
        int flags,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        return dispatchLocalActor(
            actor,
            parts,
            requestId,
            flags,
            sourceNodeRid,
            sourceSessionRid,
            0,
            0);
    }

    private boolean dispatchLocalActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        long requestId,
        int flags,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence) {
        if (!isCurrentActor(actor)) {
            return false;
        }
        String targetSpotId = actorSpots.get(actor.actorId());
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            return false;
        }
        List<Message> copied = ZLinkJavaRawSpot.copy(parts);
        String contentType = ZLinkChannelContentTypeFrame.decode(parts);
        byte[] acceptedRecord = owner.encodeLocalActorAccepted(
            actor,
            sourceNodeRid,
            sourceSessionRid,
            sourceBindingGeneration,
            sourceSessionSequence,
            requestId,
            parts);
        List<ZLinkBackendActorReceived> messages =
            new ArrayList<>(copied.size());
        for (int index = 0; index < copied.size(); index++) {
            messages.add(new ZLinkBackendActorReceived(
                actor,
                sourceNodeRid,
                sourceSessionRid,
                Optional.empty(),
                requestId,
                flags,
                copied.get(index),
                index + 1 < copied.size(),
                index == 0
                    ? acceptedRecord
                    : new byte[0],
                  contentType));
        }
        target.enqueueActor(messages);
        return true;
    }

    private boolean isCurrentActor(ZLinkBackendActorRef actor) {
        return actor != null && actor.equals(actors.get(actor.actorId()));
    }

    boolean isCurrentBoundActor(ZLinkBackendActorRef actor) {
        return isCurrentActor(actor);
    }

    private record StreamBinding(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        long authorityOwnerGeneration,
        ZLinkJavaStreamSocket stream) {
    }

    private record RemoteBindingAdmission(
        boolean accepted,
        RemoteStreamBinding replaced) {
        private static RemoteBindingAdmission rejected() {
            return new RemoteBindingAdmission(false, null);
        }

        private static RemoteBindingAdmission accepted(
            RemoteStreamBinding replaced) {
            return new RemoteBindingAdmission(true, replaced);
        }
    }

    record RemoteStreamBinding(
        RoutingId sessionOwnerNodeRid,
        long sessionOwnerNodeGeneration,
        String sessionOwnerId,
        long sessionOwnerLeaseGeneration,
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long targetNodeGeneration,
        long bindingGeneration,
        long authorityOwnerGeneration,
        long actorOwnerLeaseGeneration) {
        boolean sameSessionOwnerEpoch(RemoteStreamBinding other) {
            return sessionOwnerNodeRid.equals(
                    other.sessionOwnerNodeRid)
                && sessionOwnerNodeGeneration
                    == other.sessionOwnerNodeGeneration
                && sessionOwnerId.equals(other.sessionOwnerId)
                && sessionOwnerLeaseGeneration
                    == other.sessionOwnerLeaseGeneration;
        }
    }

    private record InstanceAuthority(
        String stableType,
        ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        private InstanceAuthority {
            if (stableType == null || stableType.isBlank()) {
                throw new IllegalArgumentException(
                    "Instance Spot stable type is required");
            }
        }
    }

    private record SpotAuthorityKey(
        RoutingId nodeRid,
        String spotId,
        long objectGeneration) {
    }

    private record ActorAuthorityKey(
        RoutingId nodeRid,
        String actorId,
        long objectGeneration) {
    }

    boolean publish(
        ZLinkJavaRawSpot source,
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts) {
        owner.publishLogicalMulticast(
            source, channelName, topic, metadata, parts);
        return true;
    }

    CompletionStage<Void> publishAsync(
        ZLinkJavaRawSpot source,
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts) {
        return owner.publishLogicalMulticast(
            source, channelName, topic, metadata, parts);
    }

    void enqueueLogicalMulticast(
        String channelName,
        String topic,
        String sourceSpotId,
        RoutingId sourceNodeRid,
        byte[] metadata,
        List<Message> parts) {
        enqueueLogicalMulticast(
            channelName,
            topic,
            sourceSpotId,
            sourceNodeRid,
            metadata,
            null,
            parts);
    }

    void enqueueLogicalMulticast(
        String channelName,
        String topic,
        String sourceSpotId,
        RoutingId sourceNodeRid,
        byte[] metadata,
        String contentType,
        List<Message> parts) {
        streamTrace(STREAM_TRACE ? "logical-multicast-enqueue channel=" + channelName
            + " topic=" + topic
            + " sourceSpot=" + sourceSpotId
            + " targets=" + spots.values().stream()
                .filter(target -> target.accepts(topic))
                .map(ZLinkJavaRawSpot::spotId)
                .sorted()
                .toList() : null);
        for (ZLinkJavaRawSpot target : spots.values()) {
            if (!target.accepts(topic)) {
                continue;
            }
            target.enqueueTopic(
                new systems.zlink.framework.runtime.internal.backend
                .ZLinkBackendTopicMessage(
                    Optional.of(sourceNodeRid),
                    channelName,
                    topic,
                    metadata == null ? new byte[0] : metadata.clone(),
                      ZLinkJavaRawSpot.copy(parts),
                      contentType));
        }
    }

    CompletionStage<Void> sendToSpot(
        ZLinkJavaRawSpot source,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetGeneration,
        byte[] metadata,
        List<Message> parts) {
        if (!routingId().equals(targetNodeRid)) {
            return owner.sendSpot(
                source.spotId(),
                targetNodeRid,
                targetSpotId,
                targetGeneration,
                metadata,
                parts);
        }
        ZLinkJavaRawSpot target = localSpot(
            targetNodeRid, targetSpotId, targetGeneration);
        if (target == null) {
            return CompletableFuture.failedFuture(
                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        }
        byte[] acceptedRecord = owner.encodeLocalSpotAccepted(
            source.spotId(),
            targetSpotId,
            targetGeneration,
            metadata,
            parts,
            null);
        target.enqueueRoute(new systems.zlink.framework.runtime.internal.backend
            .ZLinkBackendReceived(
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendRequestResult.OK,
                Optional.of(routingId()),
                Optional.of(source.spotId()),
                Optional.empty(),
                metadata,
                acceptedRecord,
                ZLinkJavaRawSpot.copy(parts),
                null,
                () -> { },
                  ZLinkChannelContentTypeFrame.decode(parts)));
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<ZLinkBackendReceived> requestToSpot(
        ZLinkJavaRawSpot source,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetGeneration,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        if (!routingId().equals(targetNodeRid)) {
            return owner.requestSpot(
                source.spotId(),
                targetNodeRid,
                targetSpotId,
                targetGeneration,
                metadata,
                parts,
                timeout);
        }
        ZLinkJavaRawSpot target = localSpot(
            targetNodeRid, targetSpotId, targetGeneration);
        if (target == null) {
            return CompletableFuture.failedFuture(
                new ZlinkRequestException(RequestResult.NOT_FOUND));
        }
        long sequence = nextRequestSequence.getAndIncrement();
        byte[] acceptedRecord = owner.encodeLocalSpotAccepted(
            source.spotId(),
            targetSpotId,
            targetGeneration,
            metadata,
            parts,
            sequence);
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        CompletableFuture<ZLinkBackendReceived> completion =
            new CompletableFuture<>();
        CompletionStage<Void> enqueued = target.enqueueRoute(
            new systems.zlink.framework.runtime.internal.backend
            .ZLinkBackendReceived(
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendRequestResult.OK,
                Optional.of(routingId()),
                Optional.of(source.spotId()),
                Optional.of(sequence),
                metadata,
                acceptedRecord,
                ZLinkJavaRawSpot.copy(parts),
                reply -> {
                    if (!terminal.tryWin(ZLinkTerminalWinner.Cause.RESPONSE)) {
                        return;
                    }
                    completion.complete(new ZLinkBackendReceived(
                            systems.zlink.framework.runtime.internal.backend
                                .ZLinkBackendRequestResult.OK,
                            Optional.of(targetNodeRid),
                            Optional.of(targetSpotId),
                            Optional.of(sequence),
                            ZLinkJavaRawSpot.copy(reply)));
                },
                () -> { },
                  ZLinkChannelContentTypeFrame.decode(parts)));
        enqueued.whenComplete((ignored, failure) -> {
            if (failure != null
                && terminal.tryWin(ZLinkTerminalWinner.Cause.FAILURE)) {
                completion.completeExceptionally(failure);
            }
        });
        if (timeout != null && !timeout.isNegative() && !timeout.isZero()) {
            CompletableFuture.delayedExecutor(
                timeout.toNanos(),
                TimeUnit.NANOSECONDS).execute(() -> {
                    if (terminal.tryWin(ZLinkTerminalWinner.Cause.TIMEOUT)) {
                        completion.completeExceptionally(
                            new ZlinkRequestException(
                                RequestResult.TIMED_OUT));
                    }
                });
        }
        return completion;
    }

    private ZLinkJavaRawSpot localSpot(
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetGeneration) {
        if (!routingId().equals(targetNodeRid)) {
            return null;
        }
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            return null;
        }
        return targetGeneration == 0
                || target.lifecycleGeneration() == targetGeneration
            ? target
            : null;
    }

    private record AuthorityFence(
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
    }
}
