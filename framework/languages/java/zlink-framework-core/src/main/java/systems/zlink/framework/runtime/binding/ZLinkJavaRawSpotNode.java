package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.calls.ZLinkRequestStages;
import systems.zlink.framework.runtime.internal.completion.ZLinkTerminalWinner;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Framework-owned service runtime projected over the raw MeshNode transport. Stateful Spot and
 * Actor identity remains inside the Framework runtime.
 */
final class ZLinkJavaRawSpotNode implements ZLinkInternalSpotNode {
    private static final Logger LOGGER = Logger.getLogger(ZLinkJavaRawSpotNode.class.getName());
    private final ZLinkJavaRawMeshNode owner;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ZLinkJavaRawSpot> spots = new ConcurrentHashMap<>();
    private final Map<String, ZLinkBackendActorRef> actors = new ConcurrentHashMap<>();
    private final Map<String, String> actorSpots = new ConcurrentHashMap<>();
    private final Map<String, Long> actorMembershipEpochs = new ConcurrentHashMap<>();
    private final Map<SpotAuthorityKey, AuthorityFence> spotAuthorities = new ConcurrentHashMap<>();
    private final Map<ActorAuthorityKey, AuthorityFence> actorAuthorities =
            new ConcurrentHashMap<>();
    private final AtomicLong nextGeneration = new AtomicLong(1);
    private final AtomicLong nextRequestSequence = new AtomicLong(1);
    private final AtomicLong nextActorRequestSequence = new AtomicLong(1);
    private final AtomicLong nextStreamBindingGeneration = new AtomicLong();
    private final Map<Long, CompletableFuture<List<Message>>> actorRequests =
            new ConcurrentHashMap<>();
    private final Map<Long, Consumer<List<Message>>> actorRemoteReplies = new ConcurrentHashMap<>();
    // Session owner index of this node's published Session routes, by Actor. Only the Session
    // owner changes it, in this node's lane; push and command 36 delivery read it.
    private final Map<String, StreamBinding> streamBindings = new ConcurrentHashMap<>();
    // Actor owner record: the current binding of each local Actor, local or remote Session. Only
    // the binding owner's turn changes it; Actor-side ingress fences read it.
    private final Map<String, BindingEntry> actorBindings = new ConcurrentHashMap<>();
    // Without an attached Actor runtime this node's own lane is the binding owner.
    private volatile RemoteSessionBindingOwner remoteSessionBindingOwner =
            (node, actor, transition) ->
                    this.stateLane.runNowOrQueue(
                            () -> transition.apply(RemoteSessionBindingTransition.NONE));
    private final ZLinkJavaInstanceSpotRegistry instanceSpots;
    private volatile java.util.function.Function<
                    ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope,
                    CompletionStage<ZLinkAuthorityReadResult>>
            instanceActivationValidator = envelope -> CompletableFuture.completedFuture(null);

    private final Map<String, InstanceAuthority> instanceAuthorities = new ConcurrentHashMap<>();
    private volatile ZLinkJavaRawSpot entrySpot;
    private volatile ZLinkMeshApplicationReceiver applicationReceiver;
    private volatile ZLinkInternalSpotNode.MessageFollowRelayHandler messageFollowRelayHandler;
    private volatile ZLinkInternalSpotNode.RelocationStagingIngressHandler
            relocationStagingIngressHandler;
    private volatile ZLinkInternalSpotNode.SpotAdmissionResolver spotAdmissionResolver;
    private final Map<
                    ZLinkServiceM6BWireCodec.SpotRouteFence,
                    ZLinkServiceM6BWireCodec.SpotRouteFence>
            relocationSpotForwards = new ConcurrentHashMap<>();

    ZLinkJavaRawSpotNode(ZLinkJavaRawMeshNode owner) {
        this.owner = owner;
        this.instanceSpots =
                new ZLinkJavaInstanceSpotRegistry(stateLane, owner::executeApplication);
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
    public void setPublisherRoutingId(RoutingId routingId) {}

    @Override
    public void setSubscriberRoutingId(RoutingId routingId) {}

    @Override
    public void setRouterBind(String endpoint) {
        owner.setBind(endpoint);
    }

    @Override
    public void setPubBind(String endpoint) {}

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
                .filter(
                        intent ->
                                owner.peers().stream()
                                        .anyMatch(
                                                peer ->
                                                        peer.connectionIntentId() == intent
                                                                && peer.endpoint()
                                                                        .equals(endpoint)))
                .findFirst()
                .ifPresent(owner::removePeerConnection);
    }

    @Override
    public void disconnectPeer(RoutingId peerRid) {
        owner.peers().stream()
                .filter(peer -> peer.routingId().equals(peerRid))
                .findFirst()
                .ifPresent(peer -> owner.removePeerConnection(peer.connectionIntentId()));
    }

    @Override
    public void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        applicationReceiver = Objects.requireNonNull(receiver, "receiver");
        receiver.setLocalNodeReadyHandler(() -> {});
    }

    @Override
    public void setMessageFollowRelayHandler(
            ZLinkInternalSpotNode.MessageFollowRelayHandler handler) {
        messageFollowRelayHandler = Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setRelocationStagingIngressHandler(
            ZLinkInternalSpotNode.RelocationStagingIngressHandler handler) {
        relocationStagingIngressHandler = Objects.requireNonNull(handler, "handler");
    }

    @Override
    public void setSpotAdmissionResolver(ZLinkInternalSpotNode.SpotAdmissionResolver resolver) {
        spotAdmissionResolver = Objects.requireNonNull(resolver, "resolver");
    }

    @Override
    public void installRelocationSpotForward(
            ZLinkServiceM6BWireCodec.SpotRouteFence source,
            ZLinkServiceM6BWireCodec.SpotRouteFence target,
            Duration retention) {
        installRelocationForward(relocationSpotForwards, source, target, retention);
    }

    @Override
    public CompletionStage<List<Message>> forwardMessageFollowActor(
            ZLinkServiceM6BWireCodec.ActorMessage stale,
            ZLinkServiceM6BWireCodec.ActorRouteFence target,
            List<Message> parts) {
        return owner.forwardMessageFollowActor(stale, target, parts);
    }

    private static <T> void installRelocationForward(
            Map<T, T> forwards, T source, T target, Duration retention) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(retention, "retention");
        if (retention.isNegative() || retention.isZero()) {
            throw new IllegalArgumentException("relocation forward retention must be positive");
        }
        T previous = forwards.putIfAbsent(source, target);
        if (previous != null && !previous.equals(target)) {
            throw new IllegalStateException("relocation forward source already has another target");
        }
        CompletableFuture.delayedExecutor(retention.toMillis(), TimeUnit.MILLISECONDS)
                .execute(() -> forwards.remove(source, target));
    }

    @Override
    public Optional<CompletionStage<Integer>> submitLocalNodeSend(
            RoutingId targetNodeRid, byte[] metadata, List<Message> parts) {
        if (!routingId().equals(targetNodeRid)) {
            return Optional.empty();
        }
        ZLinkMeshApplicationReceiver current = applicationReceiver;
        if (current == null) {
            return Optional.of(
                    CompletableFuture.completedFuture(ZLinkOneWayCalls.TARGET_NOT_FOUND));
        }
        return Optional.of(current.submitLocalNodeSend(routingId(), metadata, parts));
    }

    @Override
    public Optional<Integer> classifyNodeSendTarget(RoutingId targetNodeRid) {
        if (owner.isObjectClientNodeDirectTarget(targetNodeRid)) {
            return Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        if (routingId().equals(targetNodeRid)) {
            return Optional.empty();
        }
        return owner.classifyNodeSendTarget(targetNodeRid);
    }

    @Override
    public Optional<Integer> classifyChannelTarget(String channelName) {
        return owner.classifyChannelTarget(channelName);
    }

    @Override
    public CompletionStage<Void> sendToNode(RoutingId targetNodeRid, List<Message> parts) {
        return sendToNode(targetNodeRid, new byte[0], parts);
    }

    @Override
    public ZLinkStateLane sourceTransportLane() {
        return owner.sourceTransportLane();
    }

    @Override
    public CompletionStage<Void> sendToNode(
            RoutingId targetNodeRid, byte[] metadata, List<Message> parts) {
        return owner.inSourceTurn(
                () -> owner.sendNode(targetNodeRid, metadata, parts, false, null));
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            java.util.UUID operationId,
            CompletableFuture<Void> requestAdmission) {
        return owner.inSourceTurn(
                () ->
                        owner.requestNode(
                                targetNodeRid,
                                metadata,
                                parts,
                                timeout,
                                operations,
                                operationId,
                                requestAdmission));
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            java.util.UUID operationId,
            CompletableFuture<Void> requestAdmission) {
        return owner.inSourceTurn(
                () ->
                        owner.requestChannel(
                                channelName,
                                metadata,
                                parts,
                                timeout,
                                operations,
                                operationId,
                                requestAdmission));
    }

    @Override
    public CompletionStage<Void> sendToChannel(String channelName, List<Message> parts) {
        return sendToChannel(channelName, new byte[0], parts);
    }

    @Override
    public CompletionStage<Void> sendToChannel(
            String channelName, byte[] metadata, List<Message> parts) {
        return owner.inSourceTurn(() -> owner.sendChannel(channelName, metadata, parts));
    }

    @Override
    public void publish(String channelName, String topic, List<Message> parts, SendFlags flags) {
        owner.publishLogicalMulticast(null, channelName, topic, new byte[0], parts);
    }

    @Override
    public void publish(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
        owner.publishLogicalMulticast(null, channelName, topic, metadata, parts);
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
    public ZLinkBackendSpot createSpot(String spotId, long lifecycleGeneration) {
        Objects.requireNonNull(spotId, "spotId");
        if (lifecycleGeneration == 0) {
            throw new IllegalArgumentException("Spot lifecycle generation must be non-zero");
        }
        ZLinkJavaRawSpot created = new ZLinkJavaRawSpot(this, spotId, lifecycleGeneration);
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
        return inStateLane(
                () -> {
                    if (entrySpot == null) {
                        entrySpot =
                                (ZLinkJavaRawSpot)
                                        createSpot(routingId() + "-entry-" + UUID.randomUUID());
                    }
                    return entrySpot;
                });
    }

    @Override
    public ZLinkBackendActorRef createActor(String actorId, Message createRequest) {
        return createActor(actorId, 1, createRequest);
    }

    @Override
    public ZLinkBackendActorRef createActor(
            String actorId, long objectGeneration, Message createRequest) {
        if (actorId == null || actorId.isBlank()) {
            throw new IllegalArgumentException("actorId is required");
        }
        if (objectGeneration <= 0) {
            throw new IllegalArgumentException("Actor object generation must be positive");
        }
        ZLinkBackendActorRef created =
                new ZLinkBackendActorRef(routingId(), actorId, objectGeneration);
        if (actors.putIfAbsent(actorId, created) != null) {
            throw new IllegalStateException("actor already exists: " + actorId);
        }
        actorSpots.put(actorId, entrySpot().spotId());
        actorMembershipEpochs.put(actorId, 1L);
        rememberActorAuthority(
                created, created.generation(), owner.localAuthorityLeaseGeneration());
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
    public CompletionStage<Void> awaitPendingActorRequests() {
        // The request caller owns its result. Drain joins only terminal lifetime.
        var pending =
                actorRequests.values().stream()
                        .map(request -> request.handle((reply, failure) -> (Void) null))
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(pending);
    }

    @Override
    public void rememberActorAuthority(ZLinkBackendActorRef actor, long authorityOwnerGeneration) {
        rememberActorAuthority(
                actor, authorityOwnerGeneration, owner.localAuthorityLeaseGeneration());
    }

    @Override
    public void rememberActorAuthority(
            ZLinkBackendActorRef actor, long authorityOwnerGeneration, long ownerLeaseGeneration) {
        if (actor == null || authorityOwnerGeneration <= 0 || ownerLeaseGeneration <= 0) {
            throw new IllegalArgumentException("Actor authority generations must be positive");
        }
        actorAuthorities.put(
                new ActorAuthorityKey(actor.nodeRid(), actor.actorId(), actor.generation()),
                new AuthorityFence(authorityOwnerGeneration, ownerLeaseGeneration));
    }

    @Override
    public void registerTransferredActor(
            ZLinkBackendActorRef actor, String spotId, long membershipEpoch) {
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
        ZLinkBackendActorRef current = actors.putIfAbsent(actor.actorId(), actor);
        if (current != null && !current.equals(actor)) {
            throw new IllegalStateException(
                    "actor generation already exists on target: " + actor.actorId());
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
        return joinActor(actor, targetNodeRid, targetSpotId, 0, parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
            ZLinkBackendActorRef actor,
            RoutingId targetNodeRid,
            String targetSpotId,
            long targetSpotGeneration,
            List<Message> parts,
            Duration timeout) {
        return CompletableFuture.completedFuture(
                new ZLinkBackendActorJoinResult(
                        ZLinkBackendRequestResult.NOT_FOUND,
                        1,
                        actor,
                        targetSpotId,
                        actorMembershipEpochs.getOrDefault(actor.actorId(), 0L),
                        0,
                        List.of()));
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(
            ZLinkBackendActorRef actor,
            RoutingId targetNodeRid,
            Message request,
            Duration timeout) {
        return CompletableFuture.completedFuture(
                new ZLinkBackendActorJoinEntrySpotResult(
                        ZLinkBackendRequestResult.NOT_FOUND,
                        1,
                        actor,
                        targetNodeRid,
                        entrySpot().spotId(),
                        actorMembershipEpochs.getOrDefault(actor.actorId(), 0L),
                        0,
                        List.of()));
    }

    @Override
    public CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) {
        actors.remove(actor.actorId());
        actorSpots.remove(actor.actorId());
        actorMembershipEpochs.remove(actor.actorId());
        actorAuthorities.remove(
                new ActorAuthorityKey(actor.nodeRid(), actor.actorId(), actor.generation()));
        return retireStreamRoute(actor).thenCombine(retireActorBinding(actor), (a, b) -> null);
    }

    @Override
    public boolean sendActorBoundSession(
            ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        if (binding == null || !binding.actor().equals(actor)) {
            return false;
        }
        boolean accepted =
                binding.stream()
                        .sendBoundSessionPush(
                                binding.sessionRid(), binding.actorSlot(), parts, flags);
        return accepted;
    }

    @Override
    public boolean hasRemoteActorBoundSessionRoute(ZLinkBackendActorRef actor) {
        BindingEntry current = actorBindings.get(actor.actorId());
        return current != null
                && current.binding().remote()
                && current.binding().actor().equals(actor);
    }

    @Override
    public boolean hasLocalActorBoundSessionRoute(ZLinkBackendActorRef actor) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        return binding != null && binding.actor().equals(actor);
    }

    @Override
    public boolean sendLocalActorBoundSession(
            ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
        return sendActorBoundSession(actor, parts, flags);
    }

    @Override
    public CompletionStage<Void> sendLocalActorBoundSessionAsync(
            ZLinkBackendActorRef actor, List<Message> parts) {
        StreamBinding binding = streamBindings.get(actor.actorId());
        if (binding == null || !binding.actor().equals(actor)) {
            return CompletableFuture.failedFuture(new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        }
        CompletionStage<Void> submitted =
                binding.stream()
                        .sendBoundSessionPushAsync(
                                binding.sessionRid(), binding.actorSlot(), parts);
        return submitted;
    }

    @Override
    public CompletionStage<Void> sendRemoteActorBoundSession(
            ZLinkBackendActorRef actor, List<Message> parts) {
        BindingEntry current = actorBindings.get(actor.actorId());
        if (current == null
                || !current.binding().remote()
                || !current.binding().actor().equals(actor)) {
            return CompletableFuture.failedFuture(new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        }
        return owner.sendBoundSession(current.binding(), parts);
    }

    @Override
    public void replyActorNoBind(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long requestId,
            int flags,
            List<Message> parts) {
        CompletableFuture<List<Message>> pending = actorRequests.remove(requestId);
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
    public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
        if (!routingId().equals(actor.nodeRid())) {
            return false;
        }
        return dispatchLocalActor(actor, parts, 0, 0, null) != null;
    }

    @Override
    public CompletionStage<Void> sendToActorAsync(ZLinkBackendActorRef actor, List<Message> parts) {
        if (!routingId().equals(actor.nodeRid())) {
            return owner.sendActor(actor, parts);
        }
        CompletableFuture<Void> admission = new CompletableFuture<>();
        CompletionStage<Void> dispatched = dispatchLocalActor(actor, parts, 0, 0, admission);
        return actorAdmission(dispatched, admission);
    }

    @Override
    public CompletionStage<List<Message>> requestToActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            SendFlags flags,
            Duration timeout,
            CompletableFuture<Void> requestAdmission) {
        if (!routingId().equals(actor.nodeRid())) {
            return owner.requestActor(actor, parts, timeout, requestAdmission);
        }
        if (actor == null || !actors.containsKey(actor.actorId())) {
            return CompletableFuture.failedFuture(new IllegalStateException("actor is not local"));
        }
        long requestId = nextActorRequestSequence.getAndIncrement();
        CompletableFuture<List<Message>> completion = new CompletableFuture<>();
        actorRequests.put(requestId, completion);
        if (timeout != null && !timeout.isNegative() && !timeout.isZero()) {
            CompletableFuture.delayedExecutor(timeout.toNanos(), TimeUnit.NANOSECONDS)
                    .execute(
                            () -> {
                                if (actorRequests.remove(requestId, completion)) {
                                    completion.completeExceptionally(
                                            new ZlinkRequestException(RequestResult.TIMED_OUT));
                                }
                            });
        }
        CompletionStage<Void> dispatched =
                dispatchLocalActor(actor, parts, requestId, 1, requestAdmission);
        if (dispatched == null) {
            actorRequests.remove(requestId);
            return CompletableFuture.failedFuture(
                    new IllegalStateException("actor Spot is not local"));
        }
        dispatched.whenComplete(
                (ignored, error) -> {
                    if (error != null && actorRequests.remove(requestId, completion)) {
                        completion.completeExceptionally(unwrapActorDispatchFailure(error));
                    }
                });
        return completion.whenComplete(
                (reply, failure) -> dispatched.toCompletableFuture().cancel(false));
    }

    @Override
    public boolean forwardActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            List<Message> parts,
            SendFlags flags) {
        return dispatchLocalActor(actor, parts, 0, 0, sourceNodeRid, sourceSessionRid, 0, 0, null)
                != null;
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
        StreamBinding admitted = streamBindings.get(actor.actorId());
        if (admitIngress(
                        admitted,
                        actor,
                        sourceSessionRid,
                        sourceBindingGeneration,
                        sourceSessionSequence,
                        stream)
                != IngressAdmission.ADMITTED) {
            return false;
        }
        if (!routingId().equals(actor.nodeRid())) {
            if (streamHeader != null && streamHeader.requestSequence().isPresent()) {
                owner.requestBoundActorAsync(
                                actor,
                                sourceSessionRid,
                                sourceBindingGeneration,
                                sourceSessionSequence,
                                streamHeader.requestSequence().orElseThrow(),
                                parts,
                                Duration.ofSeconds(30))
                        .whenComplete(
                                (reply, failure) -> {
                                    if (failure == null) {
                                        replyBoundStreamSession(
                                                stream,
                                                sourceSessionRid,
                                                admitted.actorSlot(),
                                                streamHeader,
                                                reply);
                                    } else {
                                        replyBoundStreamError(
                                                stream,
                                                sourceSessionRid,
                                                admitted.actorSlot(),
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
                    .exceptionally(
                            failure -> {
                                return null;
                            });
            return true;
        }
        if (streamHeader != null && streamHeader.requestSequence().isPresent()) {
            requestToActor(actor, parts, SendFlags.DONT_WAIT, Duration.ofSeconds(30), null)
                    .whenComplete(
                            (reply, failure) -> {
                                if (failure == null) {
                                    replyBoundStreamSession(
                                            stream,
                                            sourceSessionRid,
                                            admitted.actorSlot(),
                                            streamHeader,
                                            reply);
                                } else {
                                    replyBoundStreamError(
                                            stream,
                                            sourceSessionRid,
                                            admitted.actorSlot(),
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
                        sourceSessionSequence,
                        null)
                != null;
    }

    CompletionStage<Void> forwardBoundStreamSessionAsync(
            ZLinkBackendActorRef actor,
            RoutingId sourceSessionRid,
            long sourceBindingGeneration,
            long sourceSessionSequence,
            ZLinkJavaStreamSocket stream,
            ZLinkStreamHeader streamHeader,
            List<Message> parts) {
        if (streamHeader != null && streamHeader.requestSequence().isPresent()) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "async bound-Session relay accepts one-way frames only"));
        }
        switch (admitIngress(
                streamBindings.get(actor.actorId()),
                actor,
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence,
                stream)) {
            case NOT_CURRENT -> {
                return CompletableFuture.failedFuture(
                        new ZlinkSubmitException(SubmitResult.NOT_FOUND));
            }
            case STALE_SEQUENCE -> {
                return CompletableFuture.failedFuture(
                        new ZlinkSubmitException(SubmitResult.NOT_ADMITTED));
            }
            case ADMITTED -> {}
        }
        if (!routingId().equals(actor.nodeRid())) {
            return owner.sendBoundActor(
                    actor, sourceSessionRid, sourceBindingGeneration, sourceSessionSequence, parts);
        }
        CompletableFuture<Void> admission = new CompletableFuture<>();
        CompletionStage<Void> dispatched =
                dispatchLocalActor(
                        actor,
                        parts,
                        0,
                        0,
                        routingId(),
                        sourceSessionRid,
                        sourceBindingGeneration,
                        sourceSessionSequence,
                        admission);
        return actorAdmission(dispatched, admission);
    }

    private static CompletionStage<Void> actorAdmission(
            CompletionStage<Void> dispatched, CompletableFuture<Void> admission) {
        if (dispatched == null)
            return CompletableFuture.failedFuture(new ZlinkSubmitException(SubmitResult.NOT_FOUND));
        // The owning Actor queue completes the admission when it accepts the message. A
        // dispatch that ends before any acceptance (no Actor, no matching handler) was not
        // admitted: it fails with its own failure, or NotFound when it ended without one
        // (spec 00-foundation/07-framework-error-model §5).
        dispatched.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) {
                        admission.completeExceptionally(failure);
                    } else if (!admission.isDone()) {
                        admission.completeExceptionally(
                                systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin
                                        .framework(
                                                systems.zlink.framework.errors
                                                        .ZLinkFrameworkErrorKind.NOT_FOUND,
                                                "No local Actor handler accepted the message"));
                    }
                });
        systems.zlink.framework.runtime.internal.ZLinkCompletionBridge.forwardCancellation(
                admission, dispatched);
        return admission;
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
        if (streamHeader == null || streamHeader.requestSequence().isEmpty()) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "bound Actor request requires a request sequence"));
        }
        switch (admitIngress(
                streamBindings.get(actor.actorId()),
                actor,
                sourceSessionRid,
                sourceBindingGeneration,
                sourceSessionSequence,
                stream)) {
            case NOT_CURRENT -> {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("STREAM session binding is no longer current"));
            }
            case STALE_SEQUENCE -> {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("STREAM bound-session sequence is stale"));
            }
            case ADMITTED -> {}
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
        return requestToActor(actor, parts, SendFlags.DONT_WAIT, timeout, null);
    }

    private void replyBoundStreamSession(
            ZLinkJavaStreamSocket stream,
            RoutingId sessionRid,
            int actorSlot,
            ZLinkStreamHeader requestHeader,
            List<Message> reply) {
        try {
            if (reply == null || reply.size() != 1) {
                replyBoundStreamError(
                        stream,
                        sessionRid,
                        actorSlot,
                        requestHeader,
                        new IllegalArgumentException(
                                "bound Session reply requires one encoded STREAM frame"));
                return;
            }
            stream.sendBoundSessionPushAsync(sessionRid, actorSlot, reply)
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) {
                                    replyBoundStreamError(
                                            stream, sessionRid, actorSlot, requestHeader, failure);
                                }
                            });
        } catch (RuntimeException failure) {
            replyBoundStreamError(stream, sessionRid, actorSlot, requestHeader, failure);
        } finally {
            if (reply != null) {
                reply.forEach(Message::close);
            }
        }
    }

    private void replyBoundStreamError(
            ZLinkJavaStreamSocket stream,
            RoutingId sessionRid,
            int actorSlot,
            ZLinkStreamHeader requestHeader,
            Throwable failure) {
        if (requestHeader == null || requestHeader.requestSequence().isEmpty()) {
            return;
        }
        try {
            byte[] errorFrame =
                    ZLinkStreamFrameCodec.encode(
                            ZLinkStreamHeaderCodec.encode(
                                    ZLinkStreamHeader.createErrorResponse(
                                                    requestHeader, requestHeader.packetName())
                                            .withActorSlot(
                                                    requestHeader.actorSlot().orElse(actorSlot))),
                            ZLinkStreamErrorPayload.encode(failure));
            try (Message error = Message.from(errorFrame)) {
                stream.sendBoundSessionPushAsync(sessionRid, List.of(error));
            }
        } catch (RuntimeException encodingFailure) {
        }
    }

    @Override
    public CompletionStage<Void> installRelocatingActorBoundSession(
            ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
            ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
        Objects.requireNonNull(actorRoute, "actorRoute");
        Objects.requireNonNull(session, "session");
        ZLinkBackendActorRef actor = actorRoute.actor();
        if (!routingId().equals(actor.nodeRid())) {
            throw new IllegalStateException(
                    "relocating bound Session does not target this SpotNode");
        }
        ActorSessionBinding candidate =
                new ActorSessionBinding(
                        session.nodeRid(),
                        session.nodeGeneration(),
                        session.ownerId(),
                        session.ownerLeaseGeneration(),
                        session.sessionRid(),
                        actor,
                        actorRoute.targetNodeGeneration(),
                        session.bindingGeneration(),
                        actorRoute.authorityOwnerGeneration(),
                        actorRoute.ownerLeaseGeneration(),
                        true);
        return remoteSessionBindingOwner
                .apply(
                        this,
                        actor,
                        applied -> {
                            BindingEntry current = actorBindings.get(actor.actorId());
                            if (current != null && candidate.equals(current.binding())) {
                                return true;
                            }
                            if (current != null) {
                                throw new IllegalStateException(
                                        "relocating bound Session conflicts with an installed"
                                                + " route");
                            }
                            applied.bind(candidate.route());
                            installActorBinding(candidate);
                            return true;
                        })
                .thenApply(ignored -> null);
    }

    @Override
    public Optional<ZLinkInternalSpotNode.BoundSessionRoute> boundSessionRoute(
            ZLinkBackendActorRef actor) {
        BindingEntry current = actorBindings.get(actor.actorId());
        if (current == null
                || !current.binding().remote()
                || !current.binding().actor().equals(actor)) {
            return Optional.empty();
        }
        return Optional.of(current.binding().route());
    }

    @Override
    public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) {
        if (!isCurrentActor(actor)) {
            throw new IllegalStateException("actor is not local");
        }
        // The interface completes nothing, so a failed retirement is reported where this node
        // reports its other unobserved failures.
        retireStreamRoute(actor)
                .thenCombine(retireActorBinding(actor), (route, record) -> null)
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure != null) {
                                LOGGER.log(
                                        Level.WARNING,
                                        "bound Session close failed: " + actor.actorId(),
                                        failure);
                            }
                        });
    }

    /** Removes the Actor's current binding record in the binding owner's turn. */
    private CompletionStage<Boolean> retireActorBinding(ZLinkBackendActorRef actor) {
        return remoteSessionBindingOwner.apply(
                this, actor, applied -> actorBindings.remove(actor.actorId()) != null);
    }

    /** Removes the Session owner's published route for the Actor in this node's lane. */
    private CompletionStage<Void> retireStreamRoute(ZLinkBackendActorRef actor) {
        return inSessionOwnerTurn(() -> streamBindings.remove(actor.actorId()));
    }

    /** Runs a Session owner change in this node's lane without waiting for a busy turn. */
    private CompletionStage<Void> inSessionOwnerTurn(Runnable change) {
        if (stateLane.isOnLane()) {
            change.run();
            return CompletableFuture.completedFuture(null);
        }
        return stateLane.runAsync(change);
    }

    @Override
    public void close() {
        spots.values().forEach(ZLinkJavaRawSpot::close);
        spots.clear();
        actors.clear();
        actorSpots.clear();
        actorMembershipEpochs.clear();
        actorRequests
                .values()
                .forEach(
                        completion ->
                                completion.completeExceptionally(
                                        new IllegalStateException("raw SpotNode is closed")));
        actorRequests.clear();
        actorRemoteReplies.clear();
        streamBindings.clear();
        actorBindings.clear();
        spotAuthorities.clear();
        actorAuthorities.clear();
        instanceAuthorities.clear();
        instanceSpots.closeAll();
        owner.close();
    }

    @Override
    public CompletionStage<Void> instanceActivationTerminals() {
        return instanceSpots.terminals();
    }

    void rekeySpot(ZLinkJavaRawSpot spot, String previous, String current) {
        if (previous.equals(current)) {
            return;
        }
        inStateLane(
                () -> {
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
        spotAuthorities.remove(
                new SpotAuthorityKey(routingId(), spot.spotId(), spot.lifecycleGeneration()));
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
            throw new IllegalArgumentException("Spot authority generations must be positive");
        }
        spotAuthorities.put(
                new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration),
                new AuthorityFence(authorityOwnerGeneration, ownerLeaseGeneration));
    }

    long spotAuthorityOwnerGeneration(
            RoutingId targetNodeRid, String spotId, long objectGeneration) {
        AuthorityFence fence =
                spotAuthorities.get(new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration));
        return fence == null ? 0L : fence.authorityOwnerGeneration();
    }

    long spotAuthorityOwnerLeaseGeneration(
            RoutingId targetNodeRid, String spotId, long objectGeneration) {
        AuthorityFence fence =
                spotAuthorities.get(new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration));
        return fence == null ? 0L : fence.ownerLeaseGeneration();
    }

    void forgetSpotAuthority(
            RoutingId targetNodeRid,
            String spotId,
            long objectGeneration,
            long authorityOwnerGeneration) {
        spotAuthorities.remove(
                new SpotAuthorityKey(targetNodeRid, spotId, objectGeneration),
                authorityOwnerGeneration);
    }

    @Override
    public long actorAuthorityOwnerGeneration(ZLinkBackendActorRef actor) {
        AuthorityFence fence =
                actorAuthorities.get(
                        new ActorAuthorityKey(
                                actor.nodeRid(), actor.actorId(), actor.generation()));
        return fence == null ? 0L : fence.authorityOwnerGeneration();
    }

    @Override
    public long actorAuthorityOwnerLeaseGeneration(ZLinkBackendActorRef actor) {
        AuthorityFence fence =
                actorAuthorities.get(
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
                new ZLinkInternalMeshNode.PeerAuthorityFence(sourceNodeRid, 1, "test-owner", 1),
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
                source, header, metadata, acceptedJournalRecord, parts, null, reply);
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
                source,
                header,
                metadata,
                () -> acceptedJournalRecord,
                acceptedJournalRecord.length,
                parts,
                contentType,
                reply);
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
                ignored -> {},
                () -> {});
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
                source,
                header,
                metadata,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                parts,
                contentType,
                reply,
                failure,
                () -> {});
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
        if (staging != null
                && staging.handleSpot(
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
            boolean accepted =
                    owner.forwardRelocationSpot(header, forwarded, metadata, parts, reply, failure);
            if (accepted) {
                terminalRelease.run();
            }
            return accepted;
        }
        ZLinkJavaRawSpot target =
                localSpot(routingId(), header.target().spotId(), header.target().spotGeneration());
        if (target == null
                || spotAuthorityOwnerGeneration(
                                routingId(), header.target().spotId(), target.lifecycleGeneration())
                        != header.target().authorityOwnerGeneration()) {
            ZLinkInternalSpotNode.SpotAdmissionResolver resolver = spotAdmissionResolver;
            if (resolver == null) {
                return false;
            }
            resolver.resolve(
                            header.target().spotId(),
                            header.target().authorityOwnerGeneration(),
                            false,
                            false)
                    .whenComplete(
                            (rejection, readFailure) -> {
                                try {
                                    parts.forEach(Message::close);
                                    failure.accept(readFailure == null ? rejection : readFailure);
                                } finally {
                                    terminalRelease.run();
                                }
                            });
            return true;
        }
        var received =
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived.lazyJournal(
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                                .OK,
                        Optional.of(source.sourceNodeRid()),
                        Optional.of(header.sourceSpotId()),
                        Optional.ofNullable(header.correlation()),
                        metadata,
                        acceptedJournalRecord,
                        acceptedJournalRecordSizeHint,
                        parts,
                        header.request() ? reply : null,
                        terminalRelease,
                        contentType);
        CompletionStage<Void> admitted = target.enqueueRoute(received, null);
        admitted.whenComplete(
                (ignored, enqueueFailure) -> {
                    if (enqueueFailure != null) {
                        failure.accept(enqueueFailure);
                    }
                });
        return true;
    }

    boolean enqueueRemoteActor(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.ActorMessage header,
            List<Message> parts,
            Consumer<List<Message>> reply) {
        return enqueueRemoteActor(sourceNodeRid, 0, header, new byte[0], parts, reply);
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
                sourceNodeRid, sourceNodeGeneration, header, new byte[0], parts, reply);
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
                ignored -> {});
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
                sourceNodeRid,
                sourceNodeGeneration,
                header,
                () -> acceptedJournalRecord,
                parts,
                contentType,
                reply,
                failure);
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
                source,
                header,
                acceptedJournalRecord,
                parts,
                contentType,
                reply,
                failure,
                () -> {});
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
        if (staging != null
                && staging.handleActor(
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
        ZLinkInternalSpotNode.MessageFollowRelayHandler relay = messageFollowRelayHandler;
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
                try {
                    if (failure != null) failure.accept(relayFailure);
                    else LOGGER.log(Level.WARNING, "message follow relay failed", relayFailure);
                } finally {
                    terminalRelease.run();
                }
                return true;
            }
        }
        ZLinkBackendActorRef currentActor = actor == null ? null : actors.get(actor.actorId());
        if (currentActor == null) {
            return false;
        }
        long currentAuthority = actorAuthorityOwnerGeneration(currentActor);
        long currentOwnerLease = actorAuthorityOwnerLeaseGeneration(currentActor);
        if (currentAuthority != header.target().authorityOwnerGeneration()
                || currentOwnerLease != header.target().ownerLeaseGeneration()) {
            return false;
        }
        if (header.boundSession() != null) {
            BindingEntry current = actorBindings.get(actor.actorId());
            if (current == null
                    || !current.binding()
                            .identifies(
                                    actor,
                                    sourceNodeRid,
                                    header.boundSession().sourceSessionRid(),
                                    header.boundSession().sourceBindingGeneration())
                    || current.binding().sessionOwnerNodeGeneration() != sourceNodeGeneration
                    || !current.accept(header.boundSession().sourceSessionSequence())) {
                return false;
            }
        }
        String targetSpotId = actorSpots.get(actor.actorId());
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            return false;
        }
        long requestId = header.request() ? nextActorRequestSequence.getAndIncrement() : 0;
        if (header.request()) {
            actorRemoteReplies.put(requestId, reply);
        }
        List<ZLinkBackendActorReceived> messages = new ArrayList<>(parts.size());
        if (parts.isEmpty()) {
            terminalRelease.run();
            reportActorIngressFailure(
                    target.enqueueActor(messages, null), header, requestId, reply, failure);
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
                        ? (headerFrame, payloadFrame) ->
                                redirectRelocatedActor(
                                        source,
                                        header,
                                        acceptedJournalRecord,
                                        contentType,
                                        headerFrame,
                                        payloadFrame,
                                        reply,
                                        failure)
                        : null;
        AtomicInteger remainingTerminals = new AtomicInteger(parts.size());
        Runnable partTerminal =
                () -> {
                    if (remainingTerminals.decrementAndGet() == 0) {
                        terminalRelease.run();
                    }
                };
        for (int index = 0; index < parts.size(); index++) {
            messages.add(
                    index == 0
                            ? ZLinkBackendActorReceived.lazyJournal(
                                    currentActor,
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
                                    currentActor,
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
        reportActorIngressFailure(
                target.enqueueActor(messages, null), header, requestId, reply, failure);
        return true;
    }

    /**
     * Re-routes one post-cut Actor arrival through the relocation forward the source installs
     * before its queue cut finishes. Returns whether the forward accepted it; a refusal leaves the
     * caller with the stale terminal (routing :244 forbids re-submitting the operation here).
     */
    private boolean redirectRelocatedActor(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkServiceM6BWireCodec.ActorMessage header,
            Supplier<byte[]> acceptedJournalRecord,
            String contentType,
            Message headerFrame,
            Message payloadFrame,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        ZLinkInternalSpotNode.MessageFollowRelayHandler relay = messageFollowRelayHandler;
        if (relay == null) {
            return false;
        }
        List<Message> parts = new ArrayList<>(2);
        parts.add(Message.from(headerFrame));
        parts.add(Message.from(payloadFrame));
        boolean accepted;
        try {
            accepted =
                    relay.handle(
                            source.sourceNodeRid(),
                            source.sourceNodeGeneration(),
                            header,
                            acceptedJournalRecord.get(),
                            parts,
                            contentType,
                            reply,
                            failure,
                            () -> {});
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
     * Surfaces an Actor dispatch rejection that the relocation forward did not absorb. The same
     * terminal path covers ordinary admission and closed-Spot failures, so an accepted ingress
     * cannot silently discard its dispatch stage.
     */
    private void reportActorIngressFailure(
            CompletionStage<Void> dispatched,
            ZLinkServiceM6BWireCodec.ActorMessage header,
            long requestId,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        if (dispatched == null) {
            return;
        }
        dispatched.whenComplete(
                (ignored, error) -> {
                    if (header.request()) {
                        actorRemoteReplies.remove(requestId, reply);
                    }
                    if (error == null) {
                        return;
                    }
                    Throwable cause = unwrapActorDispatchFailure(error);
                    if (failure != null) {
                        failure.accept(cause);
                    }
                });
    }

    private static Throwable unwrapActorDispatchFailure(Throwable error) {
        Throwable cause = error;
        while (cause instanceof CompletionException && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
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
            throw new IllegalStateException("STREAM binding generation is exhausted");
        }
        nextStreamBindingGeneration.set(current + 1);
        return current;
    }

    CompletionStage<Void> bindStreamSession(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long bindingGeneration,
            int actorSlot,
            ZLinkJavaStreamSocket stream,
            Duration timeout) {
        if (bindingGeneration <= 0) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("binding generation must be positive"));
        }
        if (actorSlot <= 0 || actorSlot > 0xffff) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("Actor slot must be in 1..65535"));
        }
        long authorityOwnerGeneration = actorAuthorityOwnerGeneration(actor);
        if (isCurrentActor(actor)) {
            return bindLocalSession(sessionRid, actor, bindingGeneration);
        }
        return owner.bindRemoteStreamSession(
                sessionRid, actor, authorityOwnerGeneration, bindingGeneration, true, timeout);
    }

    void publishStreamSession(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long bindingGeneration,
            int actorSlot,
            ZLinkJavaStreamSocket stream) {
        StreamBinding binding =
                new StreamBinding(
                        sessionRid,
                        actor,
                        bindingGeneration,
                        actorAuthorityOwnerGeneration(actor),
                        actorSlot,
                        stream);
        inStateLane(
                () -> {
                    StreamBinding current = streamBindings.get(actor.actorId());
                    if (current != null
                            && current.bindingGeneration() >= binding.bindingGeneration()
                            && !current.equals(binding)) {
                        throw new IllegalStateException("Actor has a newer STREAM session binding");
                    }
                    if (!binding.equals(current)) {
                        streamBindings.put(actor.actorId(), binding);
                    }
                    return current;
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
                    new IllegalStateException("STREAM session binding is stale"));
        }
        if (isCurrentActor(actor)) {
            // Ending order: stop new slotted submissions first, then retire the Actor's record
            // (02-session-actor-binding.ko.md:225-227).
            return removeStreamBinding(sessionRid, actor, bindingGeneration, stream)
                    .thenCompose(
                            ignored -> retireLocalSession(sessionRid, actor, bindingGeneration))
                    .thenApply(ignored -> null);
        }
        return owner.bindRemoteStreamSession(
                        sessionRid,
                        actor,
                        current.authorityOwnerGeneration(),
                        bindingGeneration,
                        false,
                        timeout)
                .handle(
                        (ignored, failure) ->
                                removeStreamBinding(sessionRid, actor, bindingGeneration, stream)
                                        .thenCompose(
                                                removed ->
                                                        failure == null
                                                                ? CompletableFuture
                                                                        .<Void>completedFuture(null)
                                                                : CompletableFuture
                                                                        .<Void>failedFuture(
                                                                                failure)))
                .thenCompose(stage -> stage);
    }

    /**
     * Step one of a same-node bind: the binding owner commits the Actor's current binding record
     * (its linearization point) before the Session owner commits and publishes its route, as a
     * command 38 reply does for a remote Session.
     */
    private CompletionStage<Void> bindLocalSession(
            RoutingId sessionRid, ZLinkBackendActorRef actor, long bindingGeneration) {
        // This node's owner lifecycle values are read when a replacement notice is sent, as a
        // stream-only node may bind before its mesh lifecycle starts.
        ActorSessionBinding candidate =
                new ActorSessionBinding(
                        routingId(),
                        0,
                        "",
                        0,
                        sessionRid,
                        actor,
                        0,
                        bindingGeneration,
                        actorAuthorityOwnerGeneration(actor),
                        actorAuthorityOwnerLeaseGeneration(actor),
                        false);
        return remoteSessionBindingOwner
                .apply(
                        this,
                        actor,
                        applied -> {
                            installActorBinding(candidate);
                            return true;
                        })
                .thenApply(ignored -> null);
    }

    /**
     * Retires exactly the same-node Session binding record, as a tombstone does for a remote one.
     */
    private CompletionStage<Boolean> retireLocalSession(
            RoutingId sessionRid, ZLinkBackendActorRef actor, long bindingGeneration) {
        return remoteSessionBindingOwner.apply(
                this,
                actor,
                applied -> {
                    BindingEntry current = actorBindings.get(actor.actorId());
                    return current != null
                            && current.binding()
                                    .identifies(actor, routingId(), sessionRid, bindingGeneration)
                            && actorBindings.remove(actor.actorId(), current);
                });
    }

    /**
     * Installs the Actor's current binding in the binding owner's turn and notifies the replaced
     * binding's Session owner, this node included (02-session-actor-binding.ko.md:333-358).
     */
    private void installActorBinding(ActorSessionBinding candidate) {
        String actorId = candidate.actor().actorId();
        BindingEntry current = actorBindings.get(actorId);
        if (current != null && current.binding().equals(candidate)) {
            return;
        }
        actorBindings.put(actorId, new BindingEntry(candidate, new AtomicLong()));
        if (current != null) {
            notifyBoundSessionReplaced(current.binding());
        }
    }

    @Override
    public void setRemoteSessionBindingOwner(RemoteSessionBindingOwner owner) {
        remoteSessionBindingOwner = Objects.requireNonNull(owner, "owner");
    }

    CompletionStage<Boolean> applyRemoteStreamBinding(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            String sessionOwnerId,
            long sessionOwnerLeaseGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionBind command,
            Consumer<Boolean> reply) {
        return remoteSessionBindingOwner.apply(
                this,
                command.actor().actor(),
                applied ->
                        acceptRemoteStreamBindingOnOwner(
                                sourceNodeRid,
                                sourceNodeGeneration,
                                sessionOwnerId,
                                sessionOwnerLeaseGeneration,
                                command,
                                reply,
                                applied));
    }

    private boolean acceptRemoteStreamBindingOnOwner(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            String sessionOwnerId,
            long sessionOwnerLeaseGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionBind command,
            Consumer<Boolean> reply,
            RemoteSessionBindingTransition applied) {
        ZLinkServiceM6BWireCodec.ActorRouteFence route = command.actor();
        ZLinkBackendActorRef actor = route.actor();
        ActorSessionBinding candidate =
                new ActorSessionBinding(
                        sourceNodeRid,
                        sourceNodeGeneration,
                        sessionOwnerId,
                        sessionOwnerLeaseGeneration,
                        command.sessionRid(),
                        actor,
                        route.targetNodeGeneration(),
                        command.bindingGeneration(),
                        route.authorityOwnerGeneration(),
                        route.ownerLeaseGeneration(),
                        true);
        BindingEntry entry = actorBindings.get(actor.actorId());
        ActorSessionBinding current = entry == null ? null : entry.binding();
        // The terminal reply is submitted once, here, in this owner turn and before the accepted
        // binding is published to the command 36 path (02-session-actor-binding.ko.md:215-219).
        boolean accepted = false;
        try {
            accepted =
                    admitRemoteStreamBinding(route, candidate, current, command.active(), applied);
        } finally {
            reply.accept(accepted);
        }
        if (accepted && command.active()) {
            installActorBinding(candidate);
        }
        return accepted;
    }

    private boolean admitRemoteStreamBinding(
            ZLinkServiceM6BWireCodec.ActorRouteFence route,
            ActorSessionBinding candidate,
            ActorSessionBinding current,
            boolean active,
            RemoteSessionBindingTransition applied) {
        ZLinkBackendActorRef actor = route.actor();
        if (!routingId().equals(actor.nodeRid())) {
            return false;
        }
        if (active && candidate.equals(current)) {
            return true;
        }
        if (route.targetNodeGeneration() != owner.lifecycleGeneration()) {
            return false;
        }
        if (!active) {
            BindingEntry entry = actorBindings.get(actor.actorId());
            if (entry != null
                    && candidate.equals(entry.binding())
                    && actorBindings.remove(actor.actorId(), entry)) {
                applied.retire(candidate.route());
            }
            return true;
        }
        if (!isCurrentActor(actor)
                || actorAuthorityOwnerGeneration(actor) != route.authorityOwnerGeneration()) {
            return false;
        }
        if (current != null
                && current.sameSessionOwnerEpoch(candidate)
                && current.bindingGeneration() >= candidate.bindingGeneration()) {
            return false;
        }
        applied.bind(candidate.route());
        return true;
    }

    private void notifyBoundSessionReplaced(ActorSessionBinding retired) {
        ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement =
                new ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                        actorAuthorityFence(retired.actor()),
                        retired.remote()
                                ? new ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                                        retired.sessionOwnerNodeRid(),
                                        retired.sessionOwnerNodeGeneration(),
                                        retired.sessionOwnerId(),
                                        retired.sessionOwnerLeaseGeneration(),
                                        retired.sessionRid(),
                                        retired.bindingGeneration())
                                : new ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                                        owner.routingId(),
                                        ownerLifecycleGeneration(),
                                        owner.localAuthorityOwnerId(),
                                        owner.localAuthorityLeaseGeneration(),
                                        retired.sessionRid(),
                                        retired.bindingGeneration()));
        owner.sendBoundSessionReplaced(retired.sessionOwnerNodeRid(), replacement);
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
        StreamBinding binding = streamBindings.get(command.actor().actor().actorId());
        boolean accepted =
                binding != null
                        && binding.actor().equals(command.actor().actor())
                        && binding.bindingGeneration() == command.expectedBindingGeneration()
                        && binding.authorityOwnerGeneration()
                                == command.actor().authorityOwnerGeneration()
                        && command.actor().actor().nodeRid().equals(sourceNodeRid)
                        && command.actor().targetNodeGeneration() == sourceNodeGeneration
                        && binding.stream()
                                .sendBoundSessionPush(
                                        binding.sessionRid(),
                                        binding.actorSlot(),
                                        parts,
                                        SendFlags.DONT_WAIT);
        return accepted;
    }

    private CompletionStage<Void> removeStreamBinding(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long bindingGeneration,
            ZLinkJavaStreamSocket stream) {
        return inSessionOwnerTurn(
                () -> {
                    StreamBinding current = streamBindings.get(actor.actorId());
                    if (current != null
                            && current.sessionRid().equals(sessionRid)
                            && current.actor().equals(actor)
                            && current.bindingGeneration() == bindingGeneration
                            && current.stream() == stream) {
                        streamBindings.remove(actor.actorId(), current);
                    }
                });
    }

    /**
     * Admits one Session ingress frame. The Session owner's published route must be this binding;
     * for a local Actor the Actor owner's record must be the same binding and accept the sequence.
     * A remote Actor owner applies its own record on arrival.
     */
    private IngressAdmission admitIngress(
            StreamBinding route,
            ZLinkBackendActorRef actor,
            RoutingId sessionRid,
            long bindingGeneration,
            long sessionSequence,
            ZLinkJavaStreamSocket stream) {
        if (route == null
                || !route.actor().equals(actor)
                || !route.sessionRid().equals(sessionRid)
                || route.bindingGeneration() != bindingGeneration
                || route.stream() != stream) {
            return IngressAdmission.NOT_CURRENT;
        }
        if (!routingId().equals(actor.nodeRid())) {
            return IngressAdmission.ADMITTED;
        }
        BindingEntry current = actorBindings.get(actor.actorId());
        if (current == null
                || !current.binding()
                        .identifies(actor, routingId(), sessionRid, bindingGeneration)) {
            return IngressAdmission.NOT_CURRENT;
        }
        return current.accept(sessionSequence)
                ? IngressAdmission.ADMITTED
                : IngressAdmission.STALE_SEQUENCE;
    }

    private enum IngressAdmission {
        ADMITTED,
        NOT_CURRENT,
        STALE_SEQUENCE
    }

    void setInstanceSpotActivationValidator(
            java.util.function.Function<
                            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope,
                            CompletionStage<ZLinkAuthorityReadResult>>
                    validator) {
        instanceActivationValidator = Objects.requireNonNull(validator, "validator");
    }

    void registerInstanceSpotType(String stableType) {
        instanceSpots.register(stableType, this::createSpot);
    }

    void registerInstanceSpotType(
            String stableType, ZLinkInternalMeshNode.InstanceSpotActivationHandler handler) {
        instanceSpots.register(
                stableType,
                this::createSpot,
                new ZLinkJavaInstanceSpotRegistry.ActivationHook() {
                    @Override
                    public <T> CompletionStage<T> admit(
                            java.util.function.Supplier<CompletionStage<T>> work) {
                        return handler.admit(work);
                    }

                    @Override
                    public CompletionStage<Void> activate(
                            String selectedType,
                            String spotId,
                            long generation,
                            ZLinkBackendSpot backendSpot) {
                        InstanceAuthority authority = instanceAuthorities.get(spotId);
                        if (authority == null
                                || authority.route().objectGeneration() != generation) {
                            return CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                            "Instance Spot authority is missing or stale"));
                        }
                        return handler.activate(selectedType, authority.route(), backendSpot);
                    }

                    @Override
                    public CompletionStage<Void> activate(
                            String selectedType,
                            String spotId,
                            long generation,
                            ZLinkBackendSpot backendSpot,
                            long deadlineUnixMs) {
                        InstanceAuthority authority = instanceAuthorities.get(spotId);
                        if (authority == null
                                || authority.route().objectGeneration() != generation) {
                            return CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                            "Instance Spot authority is missing or stale"));
                        }
                        return handler.activate(
                                selectedType, authority.route(), backendSpot, deadlineUnixMs);
                    }

                    @Override
                    public CompletionStage<Void> activate(
                            String selectedType,
                            String spotId,
                            long generation,
                            ZLinkBackendSpot backendSpot,
                            long deadlineUnixMs,
                            java.util.function.Consumer<ZLinkBackendSpot> restoreFirst) {
                        InstanceAuthority authority = instanceAuthorities.get(spotId);
                        if (authority == null || authority.route().objectGeneration() != generation)
                            return CompletableFuture.failedFuture(
                                    new IllegalStateException(
                                            "Instance Spot authority is missing or stale"));
                        return handler.activate(
                                selectedType,
                                authority.route(),
                                backendSpot,
                                deadlineUnixMs,
                                restoreFirst);
                    }

                    @Override
                    public CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                            ZLinkAuthorityReadResult authority) {
                        return handler.reserve(envelope, authority);
                    }

                    @Override
                    public CompletionStage<Void> completed(
                            ZLinkServiceM6BWireCodec.InstanceSpotMessage message) {
                        return handler.completed(message);
                    }

                    @Override
                    public CompletionStage<Void> admitExisting(
                            ZLinkServiceM6BWireCodec.InstanceSpotMessage message,
                            ZLinkBackendReceived received) {
                        return handler.admitExisting(message, received);
                    }
                });
    }

    CompletionStage<ZLinkJavaInstanceSpotRegistry.Activation> activateInstanceSpot(
            String spotId, String stableType) {
        ZLinkJavaRawSpot current = spots.get(spotId);
        long generation =
                current == null ? nextGeneration.getAndIncrement() : current.lifecycleGeneration();
        return instanceSpots.activate(spotId, stableType, generation);
    }

    boolean closeInstanceSpot(String spotId, long generation) {
        boolean closed = instanceSpots.close(spotId, generation);
        return closed;
    }

    ZLinkBackendSpot localSpot(String spotId) {
        return spots.get(spotId);
    }

    void registerInstanceSpotAuthority(
            String stableType, ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        Objects.requireNonNull(route, "route");
        if (!routingId().equals(route.targetNodeRid())) {
            throw new IllegalArgumentException("Instance authority target is not local");
        }
        InstanceAuthority authority = new InstanceAuthority(stableType, route);
        InstanceAuthority current =
                instanceAuthorities.putIfAbsent(route.targetSpotId(), authority);
        if (current != null && !current.equals(authority)) {
            throw new IllegalStateException(
                    "Instance Spot authority fence changed without replacement");
        }
    }

    void reconcileInstanceSpotAuthority(
            String stableType, ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        Objects.requireNonNull(route, "route");
        if (!routingId().equals(route.targetNodeRid())) {
            return;
        }
        instanceAuthorities.put(route.targetSpotId(), new InstanceAuthority(stableType, route));
    }

    void forgetInstanceSpotAuthority(ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        InstanceAuthority current = instanceAuthorities.get(route.targetSpotId());
        if (current != null && current.route().equals(route)) {
            instanceAuthorities.remove(route.targetSpotId(), current);
        }
    }

    CompletionStage<Void> recoverInstanceSpot(
            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        reconcileInstanceSpotAuthority(envelope.stableType(), route);
        ZLinkServiceM6AWireCodec.ApplicationPayload payload =
                new ZLinkServiceM6AWireCodec()
                        .decodeApplicationPayload(envelope.applicationPayloadFrame());
        List<Message> parts = ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(payload);
        byte[] metadataFrame = envelope.metadataFrame();
        ZLinkServiceM6BWireCodec.InstanceSpotMessage header =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        metadataFrame.length == 0 ? 0 : ServiceWireConstants.FLAG_METADATA,
                        new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                                envelope.targetNodeRid(),
                                envelope.targetNodeGeneration(),
                                envelope.targetSpotId(),
                                envelope.targetMeshName(),
                                envelope.stableType(),
                                envelope.descriptorVersion(),
                                envelope.deadlineUnixMs()),
                        true,
                        envelope.sourceNodeGeneration(),
                        envelope.sourceNodeRid(),
                        envelope.sourceSpotId().orElse(null),
                        envelope.request(),
                        envelope.operationHigh(),
                        envelope.operationLow(),
                        envelope.replyRouteId());
        CompletableFuture<Void> terminal = new CompletableFuture<>();
        var received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.of(envelope.sourceNodeRid()),
                        envelope.sourceSpotId(),
                        Optional.ofNullable(envelope.replyRouteId()),
                        metadataFrame,
                        new byte[0],
                        parts,
                        envelope.request() ? reply -> reply.forEach(Message::close) : null,
                        () -> {},
                        payload.contentType());
        received.retainActivationMessage(header);
        try {
            instanceSpots
                    .activate(
                            route.targetSpotId(),
                            envelope.stableType(),
                            route.objectGeneration(),
                            envelope.deadlineUnixMs(),
                            spot ->
                                    ((ZLinkJavaRawSpot) spot)
                                            .enqueueRoute(received, null)
                                            .whenComplete(
                                                    (ignored, failure) -> {
                                                        if (failure == null)
                                                            terminal.complete(null);
                                                        else
                                                            terminal.completeExceptionally(failure);
                                                    }))
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) {
                                    received.close();
                                    terminal.completeExceptionally(failure);
                                }
                            });
        } catch (RuntimeException failure) {
            received.close();
            terminal.completeExceptionally(failure);
        }
        return terminal;
    }

    boolean enqueueRemoteInstanceSpot(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
            byte[] metadata,
            List<Message> parts,
            Consumer<List<Message>> reply) {
        return enqueueRemoteInstanceSpot(
                sourceNodeRid, header, metadata, parts, null, reply, ignored -> {});
    }

    boolean enqueueRemoteInstanceSpot(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
            byte[] metadata,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply) {
        return enqueueRemoteInstanceSpot(
                sourceNodeRid, header, metadata, parts, contentType, reply, ignored -> {});
    }

    boolean enqueueRemoteInstanceSpot(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
            byte[] metadata,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        if (header.route() instanceof ZLinkServiceM6BWireCodec.InstanceColdActivation cold) {
            var envelope =
                    new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                            cold.targetSpotId(),
                            cold.stableType(),
                            cold.targetMeshName(),
                            cold.targetNodeRid(),
                            cold.targetNodeGeneration(),
                            cold.targetDescriptorVersion(),
                            header.sourceNodeRid(),
                            header.sourceNodeGeneration(),
                            Optional.ofNullable(header.sourceSpotId()),
                            header.request(),
                            header.operationHigh(),
                            header.operationLow(),
                            header.replyRouteId(),
                            cold.deadlineUnixMs(),
                            metadata,
                            ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(parts));
            var received =
                    new ZLinkBackendReceived(
                            ZLinkBackendRequestResult.OK,
                            Optional.of(sourceNodeRid),
                            Optional.ofNullable(header.sourceSpotId()),
                            Optional.ofNullable(header.replyRouteId()),
                            metadata,
                            new byte[0],
                            parts,
                            header.request() ? reply : null,
                            () -> {},
                            contentType);
            received.retainActivationMessage(header);
            instanceActivationValidator
                    .apply(envelope)
                    .thenCompose(
                            authority ->
                                    instanceSpots.activate(
                                            cold.targetSpotId(),
                                            cold.stableType(),
                                            cold.deadlineUnixMs(),
                                            () ->
                                                    instanceSpots
                                                            .reserve(envelope, authority)
                                                            .thenApply(
                                                                    route -> {
                                                                        reconcileInstanceSpotAuthority(
                                                                                cold.stableType(),
                                                                                route);
                                                                        return route
                                                                                .objectGeneration();
                                                                    }),
                                            spot ->
                                                    completeRemoteInstanceHandler(
                                                            (ZLinkJavaRawSpot) spot,
                                                            received,
                                                            () ->
                                                                    instanceSpots.completed(
                                                                            cold.stableType(),
                                                                            header))))
                    .whenComplete(
                            (activation, activationFailure) -> {
                                if (activationFailure != null) {
                                    received.close();
                                    failure.accept(activationFailure);
                                }
                            });
            return true;
        }
        ZLinkInternalSpotNode.SpotAdmissionResolver resolver = spotAdmissionResolver;
        if (resolver == null) {
            return enqueueRemoteInstanceSpotAdmitted(
                    sourceNodeRid, header, metadata, parts, contentType, reply, failure);
        }
        resolver.resolve(
                        header.route().targetSpotId(),
                        ((ZLinkServiceM6BWireCodec.InstanceRouteFence) header.route())
                                .authorityOwnerGeneration(),
                        header.instanceIntent(),
                        false)
                .whenComplete(
                        (rejection, readFailure) -> {
                            if (readFailure != null || rejection != null) {
                                closeRemoteInstancePayload(parts);
                                failure.accept(readFailure == null ? rejection : readFailure);
                            } else if (!enqueueRemoteInstanceSpotAdmitted(
                                    sourceNodeRid,
                                    header,
                                    metadata,
                                    parts,
                                    contentType,
                                    reply,
                                    failure)) {
                                // The release ended after the admission read: this node no longer
                                // holds the route's owner fence, so the message was never accepted.
                                closeRemoteInstancePayload(parts);
                                failure.accept(
                                        systems.zlink.framework.runtime.messaging
                                                .ZLinkFrameworkErrorOrigin.ownerFenceRefusal(
                                                "Spot owner fence has changed: "
                                                        + header.route().targetSpotId()));
                            }
                        });
        return true;
    }

    private boolean enqueueRemoteInstanceSpotAdmitted(
            RoutingId sourceNodeRid,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage header,
            byte[] metadata,
            List<Message> parts,
            String contentType,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        InstanceAuthority authority = instanceAuthorities.get(header.route().targetSpotId());
        if (authority == null) {
            return false;
        }
        try {
            owner.executeApplication(
                    () -> {
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
                                        () -> {},
                                        contentType);
                        if (header.instanceIntent()) received.retainActivationMessage(header);
                        CompletionStage<Void> retained;
                        try {
                            retained =
                                    instanceSpots.admitExisting(
                                            authority.stableType(), header, received);
                        } catch (RuntimeException | Error retainedFailure) {
                            received.close();
                            failure.accept(retainedFailure);
                            return;
                        }
                        if (retained != null) {
                            retained.whenComplete(
                                    (ignored, retainedFailure) -> {
                                        if (retainedFailure != null)
                                            failure.accept(retainedFailure);
                                    });
                            return;
                        }
                        CompletionStage<ZLinkJavaInstanceSpotRegistry.Activation> activation;
                        try {
                            activation =
                                    instanceSpots.activate(
                                            header.route().targetSpotId(),
                                            authority.stableType(),
                                            ((ZLinkServiceM6BWireCodec.InstanceRouteFence)
                                                            header.route())
                                                    .objectGeneration());
                        } catch (Throwable activationFailure) {
                            received.close();
                            failure.accept(activationFailure);
                            return;
                        }
                        activation.whenComplete(
                                (value, activationFailure) -> {
                                    if (activationFailure != null) {
                                        received.close();
                                        failure.accept(activationFailure);
                                        return;
                                    }
                                    if (value == null) {
                                        received.close();
                                        failure.accept(
                                                new IllegalStateException(
                                                        "Instance Spot activation completed without"
                                                                + " a Spot"));
                                        return;
                                    }
                                    if (!(value.spot() instanceof ZLinkJavaRawSpot target)) {
                                        received.close();
                                        failure.accept(
                                                new IllegalStateException(
                                                        "Instance Spot activation returned an"
                                                                + " invalid Spot"));
                                        return;
                                    }
                                    target.enqueueRoute(received, null)
                                            .whenComplete(
                                                    (ignored, enqueueFailure) -> {
                                                        if (enqueueFailure != null)
                                                            failure.accept(enqueueFailure);
                                                    });
                                });
                    });
            return true;
        } catch (RuntimeException dispatchFailure) {
            closeRemoteInstancePayload(parts);
            failure.accept(dispatchFailure);
            return true;
        }
    }

    private static CompletionStage<Void> completeRemoteInstanceHandler(
            ZLinkJavaRawSpot target,
            ZLinkBackendReceived received,
            java.util.function.Supplier<CompletionStage<Void>> completion) {
        var admitted = new CompletableFuture<Void>();
        CompletionStage<Void> handler = target.enqueueRoute(received, admitted);
        return admitted.thenCompose(
                        admission ->
                                handler.handle(
                                        (ignored, handlerFailure) ->
                                                completion
                                                        .get()
                                                        .thenCompose(
                                                                done ->
                                                                        handlerFailure == null
                                                                                ? CompletableFuture
                                                                                        .<Void>
                                                                                                completedFuture(
                                                                                                        null)
                                                                                : CompletableFuture
                                                                                        .<Void>
                                                                                                failedFuture(
                                                                                                        handlerFailure))))
                .thenCompose(java.util.function.Function.identity());
    }

    private static void closeRemoteInstancePayload(List<Message> parts) {
        parts.forEach(Message::close);
    }

    private CompletionStage<Void> dispatchLocalActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            long requestId,
            int flags,
            CompletableFuture<Void> requestAdmission) {
        return dispatchLocalActor(
                actor, parts, requestId, flags, routingId(), null, 0, 0, requestAdmission);
    }

    private CompletionStage<Void> dispatchLocalActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            long requestId,
            int flags,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
        return dispatchLocalActor(
                actor, parts, requestId, flags, sourceNodeRid, sourceSessionRid, 0, 0, null);
    }

    private CompletionStage<Void> dispatchLocalActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            long requestId,
            int flags,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long sourceBindingGeneration,
            long sourceSessionSequence,
            CompletableFuture<Void> requestAdmission) {
        ZLinkBackendActorRef currentActor = actor == null ? null : actors.get(actor.actorId());
        if (currentActor == null) {
            return null;
        }
        String targetSpotId = actorSpots.get(actor.actorId());
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            return null;
        }
        List<Message> copied = ZLinkJavaRawSpot.copy(parts);
        String contentType = ZLinkChannelContentTypeFrame.decode(parts);
        byte[] acceptedRecord =
                owner.encodeLocalActorAccepted(
                        currentActor,
                        sourceNodeRid,
                        sourceSessionRid,
                        sourceBindingGeneration,
                        sourceSessionSequence,
                        requestId,
                        parts);
        List<ZLinkBackendActorReceived> messages = new ArrayList<>(copied.size());
        for (int index = 0; index < copied.size(); index++) {
            messages.add(
                    new ZLinkBackendActorReceived(
                            currentActor,
                            sourceNodeRid,
                            sourceSessionRid,
                            Optional.empty(),
                            requestId,
                            flags,
                            copied.get(index),
                            index + 1 < copied.size(),
                            index == 0 ? acceptedRecord : new byte[0],
                            contentType));
        }
        return target.enqueueActor(messages, requestAdmission);
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
            int actorSlot,
            ZLinkJavaStreamSocket stream) {}

    /**
     * One Actor's current bound Session, kept by the Actor owner. {@code remote} pushes travel as
     * command 36 to the Session owner; otherwise this node's published Session route carries them.
     */
    record ActorSessionBinding(
            RoutingId sessionOwnerNodeRid,
            long sessionOwnerNodeGeneration,
            String sessionOwnerId,
            long sessionOwnerLeaseGeneration,
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long targetNodeGeneration,
            long bindingGeneration,
            long authorityOwnerGeneration,
            long actorOwnerLeaseGeneration,
            boolean remote) {
        boolean identifies(
                ZLinkBackendActorRef actor,
                RoutingId sessionOwnerNodeRid,
                RoutingId sessionRid,
                long bindingGeneration) {
            return this.actor.equals(actor)
                    && this.sessionOwnerNodeRid.equals(sessionOwnerNodeRid)
                    && this.sessionRid.equals(sessionRid)
                    && this.bindingGeneration == bindingGeneration;
        }

        BoundSessionRoute route() {
            return new BoundSessionRoute(
                    sessionOwnerNodeRid, sessionOwnerNodeGeneration, sessionRid, bindingGeneration);
        }

        boolean sameSessionOwnerEpoch(ActorSessionBinding other) {
            return sessionOwnerNodeRid.equals(other.sessionOwnerNodeRid)
                    && sessionOwnerNodeGeneration == other.sessionOwnerNodeGeneration
                    && sessionOwnerId.equals(other.sessionOwnerId)
                    && sessionOwnerLeaseGeneration == other.sessionOwnerLeaseGeneration;
        }
    }

    /** The Actor's current binding and the last Session sequence accepted for that binding. */
    private record BindingEntry(ActorSessionBinding binding, AtomicLong lastSequence) {
        boolean accept(long sequence) {
            long current;
            do {
                current = lastSequence.get();
                if (sequence <= current) {
                    return false;
                }
            } while (!lastSequence.compareAndSet(current, sequence));
            return true;
        }
    }

    private record InstanceAuthority(
            String stableType, ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        private InstanceAuthority {
            if (stableType == null || stableType.isBlank()) {
                throw new IllegalArgumentException("Instance Spot stable type is required");
            }
        }
    }

    private record SpotAuthorityKey(RoutingId nodeRid, String spotId, long objectGeneration) {}

    private record ActorAuthorityKey(RoutingId nodeRid, String actorId, long objectGeneration) {}

    boolean publish(
            ZLinkJavaRawSpot source,
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts) {
        owner.publishLogicalMulticast(source, channelName, topic, metadata, parts);
        return true;
    }

    CompletionStage<Void> publishAsync(
            ZLinkJavaRawSpot source,
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts) {
        return owner.publishLogicalMulticast(source, channelName, topic, metadata, parts);
    }

    /** Posts a local publication to every local Spot subscribed to its topic. */
    void enqueueLogicalMulticast(
            String channelName,
            String topic,
            RoutingId sourceNodeRid,
            byte[] metadata,
            String contentType,
            List<Message> parts) {
        for (ZLinkJavaRawSpot target : spots.values()) {
            if (target.accepts(topic)) {
                enqueueLogicalMulticast(
                        target, channelName, topic, sourceNodeRid, metadata, contentType, parts);
            }
        }
    }

    /** The local Spots a received Logical Multicast fans out to, in the order they are posted. */
    List<ZLinkJavaRawSpot> logicalMulticastTargets(String topic) {
        List<ZLinkJavaRawSpot> targets = new ArrayList<>();
        for (ZLinkJavaRawSpot target : spots.values()) {
            if (target.accepts(topic)) {
                targets.add(target);
            }
        }
        return targets;
    }

    /** Posts one Logical Multicast child to one Spot with its own copy of the payload. */
    static void enqueueLogicalMulticast(
            ZLinkJavaRawSpot target,
            String channelName,
            String topic,
            RoutingId sourceNodeRid,
            byte[] metadata,
            String contentType,
            List<Message> parts) {
        target.enqueueTopic(
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage(
                        Optional.of(sourceNodeRid),
                        channelName,
                        topic,
                        metadata == null ? new byte[0] : metadata.clone(),
                        ZLinkJavaRawSpot.copy(parts),
                        contentType));
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
        ZLinkJavaRawSpot target = localSpot(targetNodeRid, targetSpotId, targetGeneration);
        if (target == null) {
            ZLinkInternalSpotNode.SpotAdmissionResolver resolver = spotAdmissionResolver;
            return resolver == null
                    ? CompletableFuture.failedFuture(
                            new ZlinkSubmitException(SubmitResult.NOT_FOUND))
                    : resolver.resolve(targetSpotId, 0, false, false)
                            .thenCompose(CompletableFuture::failedFuture);
        }
        byte[] acceptedRecord =
                owner.encodeLocalSpotAccepted(
                        source.spotId(), targetSpotId, targetGeneration, metadata, parts, null);
        return enqueueLocalSpot(
                target,
                false,
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived(
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                                .OK,
                        Optional.of(routingId()),
                        Optional.of(source.spotId()),
                        Optional.empty(),
                        metadata,
                        acceptedRecord,
                        ZLinkJavaRawSpot.copy(parts),
                        null,
                        () -> {},
                        ZLinkChannelContentTypeFrame.decode(parts)),
                null);
    }

    private static CompletionStage<Void> enqueueLocalSpot(
            ZLinkJavaRawSpot target,
            boolean awaitHandlerCompletion,
            ZLinkBackendReceived received,
            CompletableFuture<Void> requestAdmission) {
        CompletableFuture<Void> admission = new CompletableFuture<>();
        ZLinkRequestStages.forwardAdmission(requestAdmission, admission);
        CompletionStage<Void> dispatched;
        try {
            dispatched = target.enqueueRoute(received, admission);
        } catch (RuntimeException | Error failure) {
            received.close();
            admission.completeExceptionally(failure);
            dispatched = CompletableFuture.failedFuture(failure);
        }
        return awaitHandlerCompletion ? dispatched : admission;
    }

    CompletionStage<ZLinkBackendReceived> requestToSpot(
            ZLinkJavaRawSpot source,
            RoutingId targetNodeRid,
            String targetSpotId,
            long targetGeneration,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            CompletableFuture<Void> requestAdmission) {
        if (!routingId().equals(targetNodeRid)) {
            return owner.requestSpot(
                    source.spotId(),
                    targetNodeRid,
                    targetSpotId,
                    targetGeneration,
                    metadata,
                    parts,
                    timeout,
                    requestAdmission);
        }
        return requestToLocalSpot(
                source,
                targetNodeRid,
                targetSpotId,
                targetGeneration,
                metadata,
                parts,
                timeout,
                null,
                null,
                requestAdmission);
    }

    CompletionStage<ZLinkBackendReceived> requestToSpot(
            ZLinkJavaRawSpot source,
            RoutingId targetNodeRid,
            String targetSpotId,
            long targetGeneration,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId,
            CompletableFuture<Void> requestAdmission) {
        if (!routingId().equals(targetNodeRid)) {
            return owner.requestSpot(
                    source.spotId(),
                    targetNodeRid,
                    targetSpotId,
                    targetGeneration,
                    metadata,
                    parts,
                    timeout,
                    operations,
                    operationId,
                    requestAdmission);
        }
        return requestToLocalSpot(
                source,
                targetNodeRid,
                targetSpotId,
                targetGeneration,
                metadata,
                parts,
                timeout,
                operations,
                operationId,
                requestAdmission);
    }

    private CompletionStage<ZLinkBackendReceived> requestToLocalSpot(
            ZLinkJavaRawSpot source,
            RoutingId targetNodeRid,
            String targetSpotId,
            long targetGeneration,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId,
            CompletableFuture<Void> requestAdmission) {
        long sequence = nextRequestSequence.getAndIncrement();
        byte[] acceptedRecord =
                owner.encodeLocalSpotAccepted(
                        source.spotId(), targetSpotId, targetGeneration, metadata, parts, sequence);
        ZLinkJavaRawSpot target = localSpot(targetNodeRid, targetSpotId, targetGeneration);
        if (target == null) {
            ZLinkInternalSpotNode.SpotAdmissionResolver resolver = spotAdmissionResolver;
            return resolver == null
                    ? CompletableFuture.failedFuture(
                            new ZlinkRequestException(RequestResult.NOT_FOUND))
                    : resolver.resolve(targetSpotId, 0, false, false)
                            .thenCompose(CompletableFuture::failedFuture);
        }
        ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();
        CompletableFuture<ZLinkBackendReceived> completion = new CompletableFuture<>();
        ZLinkBackendReceived request =
                new ZLinkBackendReceived(
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                                .OK,
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
                            completion.complete(
                                    new ZLinkBackendReceived(
                                            systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkBackendRequestResult.OK,
                                            Optional.of(targetNodeRid),
                                            Optional.of(targetSpotId),
                                            Optional.of(sequence),
                                            ZLinkJavaRawSpot.copy(reply)));
                        },
                        () -> {},
                        ZLinkChannelContentTypeFrame.decode(parts));
        // The local ingress takes the prepared record; rejected registration
        // leaves that ownership here. Payload preparation stays outside E3.
        boolean[] handedOff = {false};
        Supplier<CompletionStage<ZLinkBackendReceived>> enqueue =
                () -> {
                    handedOff[0] = true;
                    var dispatched = enqueueLocalSpot(target, true, request, requestAdmission);
                    dispatched.whenComplete(
                            (ignored, failure) -> {
                                if (failure != null
                                        && terminal.tryWin(ZLinkTerminalWinner.Cause.FAILURE)) {
                                    completion.completeExceptionally(failure);
                                }
                            });
                    return completion.whenComplete(
                            (reply, failure) -> {
                                if (completion.isCancelled())
                                    terminal.tryWin(ZLinkTerminalWinner.Cause.CANCELLATION);
                                dispatched.toCompletableFuture().cancel(false);
                            });
                };
        if (operations != null) {
            try {
                var registered =
                        operations.submit(
                                operationId, timeout, enqueue, ZLinkBackendReceived::close);
                return registered.whenComplete((reply, failure) -> completion.cancel(false));
            } finally {
                if (!handedOff[0]) {
                    request.close();
                }
            }
        }
        if (timeout != null && !timeout.isNegative() && !timeout.isZero()) {
            CompletableFuture.delayedExecutor(timeout.toNanos(), TimeUnit.NANOSECONDS)
                    .execute(
                            () -> {
                                if (terminal.tryWin(ZLinkTerminalWinner.Cause.TIMEOUT)) {
                                    completion.completeExceptionally(
                                            new ZlinkRequestException(RequestResult.TIMED_OUT));
                                }
                            });
        }
        return enqueue.get();
    }

    private ZLinkJavaRawSpot localSpot(
            RoutingId targetNodeRid, String targetSpotId, long targetGeneration) {
        if (!routingId().equals(targetNodeRid)) {
            return null;
        }
        ZLinkJavaRawSpot target = spots.get(targetSpotId);
        if (target == null) {
            return null;
        }
        return target;
    }

    private record AuthorityFence(long authorityOwnerGeneration, long ownerLeaseGeneration) {}
}
