package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.Actor;
import systems.zlink.framework.runtime.internal.binding.spot.ActorControlRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ActorLifecycleKind;
import systems.zlink.framework.runtime.internal.binding.spot.ActorRef;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferToken;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshDestinationKind;
import systems.zlink.framework.runtime.internal.binding.spot.MeshSendReadyData;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;


final class ZLinkJavaMeshSpotNode
    implements ZLinkInternalSpotNode, ZLinkJavaAdmissionBacked {

    private final ZLinkJavaMeshNode owner;
    private final MeshNode node;
    private final Map<String, ZLinkJavaMeshSpot> spots = new ConcurrentHashMap<>();
    private final Map<String, Actor> actors = new ConcurrentHashMap<>();
    private final Map<String, String> actorSpots = new ConcurrentHashMap<>();
    private final Map<String, String> localActorDispatchSpots = new ConcurrentHashMap<>();
    private final Map<String, Long> actorMembershipEpochs = new ConcurrentHashMap<>();
    private final Map<String, CompletableFuture<List<Message>>> pendingLeaves =
        new ConcurrentHashMap<>();
    private final Map<String, Long> peerIntents = new ConcurrentHashMap<>();
    private final Map<String, RoutingId> expectedPeerRids = new ConcurrentHashMap<>();
    private final java.util.Set<String> untypedPeerEndpoints = ConcurrentHashMap.newKeySet();
    private volatile java.util.Set<RoutingId> ownerExpectedPeerRids = java.util.Set.of();
    private volatile boolean ownerHasUntypedPeerIntent;
    private volatile ZLinkJavaMeshSpot entrySpot;
    private volatile String primaryChannelName;
    private volatile Consumer<ZLinkBackendAdmissionKey> admissionReadyHandler =
        ignored -> { };
    private volatile Runnable admissionShutdownHandler = () -> { };
    private volatile ZLinkMeshApplicationReceiver applicationReceiver;
    private volatile Duration admissionTimeout = Duration.ofSeconds(1);
    private volatile int admissionPendingCapacity = 4096;

    ZLinkJavaMeshSpotNode(ZLinkJavaMeshNode owner) {
        this.owner = owner;
        this.node = owner.nativeNode();
        owner.startServiceDispatch(this::dispatch);
    }

    void channelName(String name) {
        if (primaryChannelName == null) {
            primaryChannelName = name;
        }
    }

    @Override public String name() { return owner.name(); }
    @Override
    public void setAdmissionReadyHandler(
        Consumer<ZLinkBackendAdmissionKey> handler) {
        admissionReadyHandler = java.util.Objects.requireNonNull(handler, "handler");
    }
    @Override
    public void setAdmissionShutdownHandler(Runnable handler) {
        admissionShutdownHandler = java.util.Objects.requireNonNull(handler, "handler");
    }
    @Override public Duration admissionTimeout() { return admissionTimeout; }
    @Override public int admissionPendingCapacity() { return admissionPendingCapacity; }

    void setAdmissionTimeout(Duration value) {
        admissionTimeout = java.util.Objects.requireNonNull(value, "value");
    }

    void setAdmissionPendingCapacity(int value) {
        if (value <= 0) {
            throw new IllegalArgumentException(
                "pending admission capacity must be positive");
        }
        admissionPendingCapacity = value;
    }
    @Override public RoutingId routingId() { return node.getRoutingId(); }
    @Override public void setRoutingId(RoutingId routingId) { node.setRoutingId(routingId); }
    @Override public void setPublisherRoutingId(RoutingId routingId) { }
    @Override public void setSubscriberRoutingId(RoutingId routingId) { }
    @Override public void setRouterBind(String endpoint) { node.setBind(endpoint); }
    @Override public void setPubBind(String endpoint) { }

    @Override
    public void connectPeer(String endpoint) {
        long intent = node.connectPeer(endpoint);
        untypedPeerEndpoints.add(endpoint);
        peerIntents.put(endpoint, intent);
    }

    @Override
    public void connectPeer(RoutingId peerRid, String endpoint) {
        long intent = node.connectPeer(endpoint, peerRid);
        expectedPeerRids.put(endpoint, peerRid);
        peerIntents.put(endpoint, intent);
    }

    @Override
    public void disconnectPeer(String endpoint) {
        untypedPeerEndpoints.remove(endpoint);
        expectedPeerRids.remove(endpoint);
        Long intent = peerIntents.remove(endpoint);
        if (intent != null) {
            node.removePeerConnection(intent);
        }
    }

    @Override
    public void disconnectPeer(RoutingId peerRid) {
        node.peers().stream()
            .filter(peer -> peer.routingId().equals(peerRid))
            .forEach(peer -> node.disconnectPeer(
                peer.routingId(),
                peer.lifecycleGeneration()));
    }

    @Override
    public void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        applicationReceiver = java.util.Objects.requireNonNull(receiver, "receiver");
        receiver.setLocalNodeReadyHandler(() -> admissionReadyHandler.accept(
            ZLinkBackendAdmissionKey.node(routingId())));
    }

    @Override
    public Optional<Integer> submitLocalNodeSend(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts) {
        if (!routingId().equals(targetNodeRid)) {
            return Optional.empty();
        }
        ZLinkMeshApplicationReceiver receiver = applicationReceiver;
        if (receiver == null) {
            return Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        return Optional.of(receiver.submitLocalNodeSend(routingId(), metadata, parts));
    }

    @Override
    public Optional<Integer> classifyNodeSendTarget(RoutingId targetNodeRid) {
        boolean admitted = node.peers().stream()
            .anyMatch(peer -> peer.routingId().equals(targetNodeRid));
        boolean hasAuthority = !expectedPeerRids.isEmpty() || !ownerExpectedPeerRids.isEmpty();
        boolean incompleteAuthority = ownerHasUntypedPeerIntent || !untypedPeerEndpoints.isEmpty();
        if (admitted || !hasAuthority || incompleteAuthority
            || expectedPeerRids.containsValue(targetNodeRid)
            || ownerExpectedPeerRids.contains(targetNodeRid)) {
            return Optional.empty();
        }
        return Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
    }

    void updateOwnerPeerCatalog(
        java.util.Collection<RoutingId> expectedRids,
        boolean hasUntypedPeerIntent) {
        ownerExpectedPeerRids = java.util.Set.copyOf(expectedRids);
        ownerHasUntypedPeerIntent = hasUntypedPeerIntent;
    }

    @Override
    public ZLinkBackendSpotRouteBridge createRouteBridge() {
        throw new UnsupportedOperationException(
            "MeshNode routes Spot records directly without a route bridge");
    }

    @Override
    public ZLinkBackendSpot createSpot() {
        return register(node.getOrCreateSpot(
            java.util.UUID.randomUUID().toString()).spot());
    }

    @Override
    public ZLinkBackendSpot createSpot(String spotId) {
        return register(node.getOrCreateSpot(spotId).spot());
    }

    @Override
    public boolean sendToNode(
        RoutingId targetNodeRid,
        List<Message> parts,
        SendFlags flags) {
        node.sendToNode(targetNodeRid, parts, flags);
        return true;
    }

    @Override
    public boolean sendToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        node.sendToNode(targetNodeRid, metadata, parts, flags);
        return true;
    }

    @Override
    public boolean requestToNode(
        RoutingId targetNodeRid,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        var operation = node.requestToNode(targetNodeRid, parts, flags, timeout);
        completeRequest(operation, callback);
        return true;
    }

    @Override
    public boolean requestToNode(
        RoutingId targetNodeRid,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        var operation =
            node.requestToNode(targetNodeRid, metadata, parts, flags, timeout);
        completeRequest(operation, callback);
        return true;
    }

    @Override
    public boolean sendToChannel(
        String channelName,
        List<Message> parts,
        SendFlags flags) {
        node.sendToChannel(channelName, parts, flags);
        return true;
    }

    @Override
    public boolean sendToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        node.sendToChannel(channelName, metadata, parts, flags);
        return true;
    }

    @Override
    public boolean requestToChannel(
        String channelName,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        var operation = node.requestToChannel(channelName, parts, flags, timeout);
        completeRequest(operation, callback);
        return true;
    }

    private void completeRequest(
        OperationId operation,
        ZLinkBackendRequestCallback callback) {
        owner.trackCompletion(operation).whenComplete((completion, error) -> {
            if (error != null) {
                callback.handle(new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.INTERNAL_ERROR,
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of()));
                return;
            }
            callback.handle(new ZLinkBackendReceived(
                mapResult(completion.receive().terminalResult()),
                Optional.ofNullable(completion.receive().sourceNodeRid()),
                Optional.ofNullable(completion.receive().sourceSpotId())
                    .map(RoutingId::toString),
                Optional.ofNullable(completion.receive().operationId())
                    .map(operationId -> operationId.low()),
                completion.parts(),
                null,
                () -> { }));
        });
    }

    @Override
    public boolean requestToChannel(
        String channelName,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        var operation =
            node.requestToChannel(channelName, metadata, parts, flags, timeout);
        completeRequest(operation, callback);
        return true;
    }

    @Override
    public void publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags) {
        try (var publisher = node.createPublisher()) {
            publisher.publish(channelName, topic, parts, flags);
        }
    }

    @Override
    public void publish(
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        try (var publisher = node.createPublisher()) {
            publisher.publish(
                channelName, topic, metadata, parts, flags);
        }
    }

    @Override
    public ZLinkBackendSpot entrySpot() {
        ZLinkJavaMeshSpot current = entrySpot;
        if (current != null) {
            return current;
        }
        synchronized (this) {
            if (entrySpot == null) {
                entrySpot = register(node.entrySpot());
            }
            return entrySpot;
        }
    }

    @Override
    public ZLinkBackendActorRef createActor(String actorId, Message createRequest) {
        Actor actor = node.createActor(actorId, List.of(createRequest));
        actors.put(actorId, actor);
        ZLinkBackendActorRef ref = backendRef(actor.ref());
        actorSpots.put(actorId, entrySpot().spotId());
        localActorDispatchSpots.put(actorId, entrySpot().spotId());
        actorMembershipEpochs.put(actorId, 1L);
        return concreteRef(ref);
    }

    @Override
    public ZLinkBackendActorRef actorLookup(String actorId) {
        return node.actorLookup(actorId)
            .map(location -> {
                actorSpots.put(actorId, location.spotId().toString());
                actorMembershipEpochs.put(actorId, location.membershipEpoch());
                return concreteRef(backendRef(location.actor()));
            })
            .orElse(null);
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        String targetSpotId,
        List<Message> parts,
        Duration timeout) {
        return joinActor(actor, targetNodeRid, targetSpotId, 0L, parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        String targetSpotId,
        long targetSpotGeneration,
        List<Message> parts,
        Duration timeout) {
        var operation = node.joinActorSpot(
            nativeRef(actor),
            targetNodeRid,
            RoutingId.from(targetSpotId),
            targetSpotGeneration > 0
                ? targetSpotGeneration
                : spotGeneration(targetSpotId),
            parts,
            timeout);
        return owner.trackCompletion(operation).thenApply(completion -> {
            ZLinkBackendRequestResult result =
                mapResult(completion.receive().terminalResult());
            List<Message> reply = completion.parts();
            var join = completion.receive().joinCompletion();
            String completedSpot = join == null
                ? targetSpotId
                : join.location().spotId().toString();
            long completedEpoch = join == null
                ? actorMembershipEpochs.getOrDefault(actor.actorId(), 0L)
                : join.location().membershipEpoch();
            if (result == ZLinkBackendRequestResult.OK && join != null
                && join.joinResult()
                    == systems.zlink.framework.runtime.internal.binding.spot.ActorJoinDecision.ACCEPTED) {
                actorSpots.put(actor.actorId(), completedSpot);
                if (join.actor().nodeRid().equals(node.getRoutingId())) {
                    localActorDispatchSpots.put(actor.actorId(), completedSpot);
                }
                actorMembershipEpochs.put(actor.actorId(), completedEpoch);
            }
            return new ZLinkBackendActorJoinResult(
                result,
                join != null
                    && join.joinResult()
                        == systems.zlink.framework.runtime.internal.binding.spot.ActorJoinDecision.ACCEPTED
                    ? 0 : 1,
                join == null ? actor : backendRef(join.actor()),
                completedSpot,
                completedEpoch,
                0,
                reply);
        });
    }

    @Override
    public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(
        ZLinkBackendActorRef actor,
        RoutingId targetNodeRid,
        Message request,
        Duration timeout) {
        var operation = node.joinActorEntrySpot(
            nativeRef(actor), targetNodeRid, List.of(request), timeout);
        return owner.trackCompletion(operation).thenApply(completion -> {
            ZLinkBackendRequestResult result =
                mapResult(completion.receive().terminalResult());
            var join = completion.receive().joinCompletion();
            String targetSpotId = join == null
                ? entrySpot().spotId()
                : join.location().spotId().toString();
            long epoch = join == null
                ? actorMembershipEpochs.getOrDefault(actor.actorId(), 0L)
                : join.location().membershipEpoch();
            if (result == ZLinkBackendRequestResult.OK && join != null
                && join.joinResult()
                    == systems.zlink.framework.runtime.internal.binding.spot.ActorJoinDecision.ACCEPTED) {
                actorSpots.put(actor.actorId(), targetSpotId);
                if (join.actor().nodeRid().equals(node.getRoutingId())) {
                    localActorDispatchSpots.put(actor.actorId(), targetSpotId);
                }
                actorMembershipEpochs.put(actor.actorId(), epoch);
            }
            return new ZLinkBackendActorJoinEntrySpotResult(
                result,
                join != null
                    && join.joinResult()
                        == systems.zlink.framework.runtime.internal.binding.spot.ActorJoinDecision.ACCEPTED
                    ? 0 : 1,
                join == null ? actor : backendRef(join.actor()),
                targetNodeRid,
                targetSpotId,
                epoch,
                0,
                completion.parts());
        });
    }

    @Override
    public CompletionStage<List<Message>> leaveActor(
        ZLinkBackendActorRef actor,
        String currentSpotId,
        Duration timeout) {
        long epoch = actorMembershipEpochs.getOrDefault(actor.actorId(), 1L);
        CompletableFuture<List<Message>> completion = new CompletableFuture<>();
        if (pendingLeaves.putIfAbsent(actor.actorId(), completion) != null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("actor leave is already pending: " + actor.actorId()));
        }
        try {
            owner.track(node.leaveActor(nativeRef(actor), epoch, timeout))
                .whenComplete((ignored, error) -> {
                    if (error != null && pendingLeaves.remove(actor.actorId(), completion)) {
                        completion.completeExceptionally(error);
                    }
                });
        } catch (RuntimeException error) {
            pendingLeaves.remove(actor.actorId(), completion);
            completion.completeExceptionally(error);
        }
        return completion;
    }

    @Override
    public CompletionStage<Void> destroyActor(
        ZLinkBackendActorRef actor,
        Duration timeout) {
        return owner.track(node.destroyActor(nativeRef(actor), timeout))
            .whenComplete((ignored, error) -> {
            Actor local = actors.remove(actor.actorId());
            actorSpots.remove(actor.actorId());
            localActorDispatchSpots.remove(actor.actorId());
            actorMembershipEpochs.remove(actor.actorId());
            if (local != null) {
                local.close();
            }
        });
    }

    @Override
    public PrepareActorTransferResult prepareActorTransfer(
        ActorTransferPrepare prepare, Duration timeout) {
        return node.prepareActorTransfer(prepare, timeout);
    }

    @Override
    public void commitActorTransfer(
        ActorTransferToken token, long newMembershipEpoch) {
        node.commitActorTransfer(token, newMembershipEpoch);
    }

    @Override
    public void activateActorTransfer(ActorTransferToken token) {
        node.activateActorTransfer(token);
    }

    @Override
    public void abortActorTransfer(ActorTransferToken token) {
        node.abortActorTransfer(token);
    }

    @Override
    public long actorMembershipEpoch(String actorId) {
        return actorMembershipEpochs.getOrDefault(actorId, 1L);
    }

    @Override
    public void registerTransferredActor(
        ZLinkBackendActorRef actor, String spotId, long membershipEpoch) {
        actorSpots.put(actor.actorId(), spotId);
        localActorDispatchSpots.put(actor.actorId(), spotId);
        actorMembershipEpochs.put(actor.actorId(), membershipEpoch);
    }

    @Override
    public boolean sendActorBoundSession(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        node.sendActorBoundSession(nativeRef(actor), parts, flags);
        return true;
    }

    @Override
    public void replyActorNoBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long requestId,
        int flags,
        List<Message> parts) {
        owner.replyActor(requestId, parts);
    }

    @Override
    public boolean sendToActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags) {
        node.sendToActor(nativeRef(actor), parts, flags);
        return true;
    }

    @Override
    public CompletionStage<List<Message>> requestToActor(
        ZLinkBackendActorRef actor,
        List<Message> parts,
        SendFlags flags,
        Duration timeout) {
        var operation = node.requestToActor(nativeRef(actor), parts, flags, timeout);
        return owner.trackCompletion(operation).thenApply(completion -> {
            ZLinkJavaMeshOperationTracker.Terminal.requireSuccess(completion);
            return completion.parts();
        });
    }

    @Override
    public boolean forwardActorBoundSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        List<Message> parts,
        SendFlags flags) {
        node.sendActorBoundSession(nativeRef(actor), parts, flags);
        return true;
    }

    @Override
    public void bindRemoteActorBoundSession(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
    }

    @Override
    public void closeActorBoundSession(
        ZLinkBackendActorRef actor,
        Duration timeout) {
        owner.track(node.closeActorBoundSession(nativeRef(actor), 0, timeout));
    }

    @Override
    public void close() {
        admissionShutdownHandler.run();
    }

    private ZLinkJavaMeshSpot register(systems.zlink.framework.runtime.internal.binding.spot.Spot spot) {
        return spots.computeIfAbsent(
            spot.spotId(),
            ignored -> new ZLinkJavaMeshSpot(
                owner,
                spot,
                () -> primaryChannelName));
    }

    long spotGeneration(String spotId) {
        ZLinkJavaMeshSpot target = spots.get(spotId);
        return target == null ? 0L : target.lifecycleGeneration();
    }

    private CompletionStage<Void> dispatch(ZLinkMeshDispatchRecord record) {
        if (record.receive().kind() == RecordKind.SEND_READY) {
            MeshSendReadyData ready = record.receive().sendReady();
            if (ready != null) {
                admissionReadyHandler.accept(admissionKey(ready));
            }
            record.close();
            return CompletableFuture.completedFuture(null);
        }
        String ownerSpotId = null;
        if (record.owner().actor() != null
            && !record.owner().actor().actorId().isEmpty()) {
            ownerSpotId = localActorDispatchSpots.get(record.owner().actor().actorId());
        }
        if (ownerSpotId == null || ownerSpotId.isEmpty()) {
            ownerSpotId = record.owner().spotId() == null
                ? null : record.owner().spotId().toString();
        }
        if (ownerSpotId == null || ownerSpotId.isEmpty()) {
            ownerSpotId = record.receive().sourceSpotId() == null
                ? null : record.receive().sourceSpotId().toString();
        }
        ZLinkJavaMeshSpot target = spots.get(ownerSpotId);
        if (target == null) {
            record.close();
            return CompletableFuture.completedFuture(null);
        }
        return target.accept(record);
    }

    private static ZLinkBackendAdmissionKey admissionKey(MeshSendReadyData ready) {
        MeshDestinationKind kind = ready.destinationKind();
        return switch (kind) {
            case NODE -> ZLinkBackendAdmissionKey.node(ready.targetNodeRid());
            case CHANNEL -> ZLinkBackendAdmissionKey.channel(ready.channelName());
            case SPOT -> ZLinkBackendAdmissionKey.spot(
                ready.targetNodeRid(), ready.targetSpotId().toString());
            case ACTOR -> ZLinkBackendAdmissionKey.actor(
                ready.targetActor().nodeRid(),
                ready.targetActor().actorId(),
                ready.targetActor().generation());
            case BOUND_SESSION -> ZLinkBackendAdmissionKey.boundSession(
                ready.targetActor().nodeRid(),
                ready.targetActor().actorId(),
                ready.targetActor().generation());
        };
    }

    void actorControl(ActorControlRecord control) {
        if (control.kind() == ActorLifecycleKind.CREATED
            || control.kind() == ActorLifecycleKind.JOINED) {
            String actorId = control.currentActor().actorId();
            actorSpots.put(actorId, control.currentSpotId().toString());
            if (control.currentActor().nodeRid().equals(node.getRoutingId())) {
                localActorDispatchSpots.put(
                    actorId, control.currentSpotId().toString());
            }
            actorMembershipEpochs.put(actorId, control.currentMembershipEpoch());
            return;
        }
        if (control.kind() != ActorLifecycleKind.LEFT) {
            return;
        }
        String actorId = control.previousActor().actorId();
        actorSpots.put(actorId, control.currentSpotId().toString());
        if (control.currentActor().nodeRid().equals(node.getRoutingId())) {
            localActorDispatchSpots.put(
                actorId, control.currentSpotId().toString());
        }
        actorMembershipEpochs.put(actorId, control.currentMembershipEpoch());
        CompletableFuture<List<Message>> completion = pendingLeaves.remove(actorId);
        if (completion != null) {
            completion.complete(List.of());
        }
        ZLinkJavaMeshSpot previous = spots.get(control.previousSpotId());
        if (previous != null) {
            previous.raisePendingLifecycle();
        }
    }

    private Actor requireActor(String actorId) {
        Actor actor = actors.get(actorId);
        if (actor == null) {
            throw new IllegalStateException("unknown local actor: " + actorId);
        }
        return actor;
    }

    private ZLinkBackendActorRef concreteRef(ZLinkBackendActorRef ref) {
        return ref.nodeRid() == null || ref.nodeRid().size() == 0
            ? new ZLinkBackendActorRef(routingId(), ref.actorId(), ref.generation())
            : ref;
    }

    static ZLinkBackendActorRef backendRef(ActorRef ref) {
        if (ref == null) {
            return null;
        }
        return new ZLinkBackendActorRef(ref.nodeRid(), ref.actorId(), ref.generation());
    }

    private static ActorRef nativeRef(ZLinkBackendActorRef ref) {
        return new ActorRef(ref.nodeRid(), ref.actorId(), ref.generation());
    }

    static ZLinkBackendRequestResult mapResult(int value) {
        RequestResult result;
        try {
            result = RequestResult.fromValue(value);
        } catch (RuntimeException ignored) {
            return ZLinkBackendRequestResult.INTERNAL_ERROR;
        }
        return switch (result) {
            case OK -> ZLinkBackendRequestResult.OK;
            case TIMED_OUT -> ZLinkBackendRequestResult.TIMED_OUT;
            case NOT_FOUND -> ZLinkBackendRequestResult.NOT_FOUND;
            case TERMINATED -> ZLinkBackendRequestResult.TERMINATED;
            case PROTOCOL_ERROR -> ZLinkBackendRequestResult.PROTOCOL_ERROR;
            case REJECTED -> ZLinkBackendRequestResult.REJECTED;
            case CONFLICT -> ZLinkBackendRequestResult.CONFLICT;
            case BUSY, BACKPRESSURED -> ZLinkBackendRequestResult.BUSY;
            case NOT_CONNECTED -> ZLinkBackendRequestResult.NOT_CONNECTED;
            case INVALID_ARGUMENT -> ZLinkBackendRequestResult.INVALID_ARGUMENT;
            case INVALID_STATE -> ZLinkBackendRequestResult.INVALID_STATE;
            case NOT_SUPPORTED -> ZLinkBackendRequestResult.NOT_SUPPORTED;
            default -> ZLinkBackendRequestResult.INTERNAL_ERROR;
        };
    }

    private void notifyLifecycle(
        String previousSpotId,
        String currentSpotId,
        ZLinkBackendActorRef actor,
        long epoch) {
        if (previousSpotId != null) {
            notifyLeft(previousSpotId, actor, epoch);
        }
        ZLinkJavaMeshSpot current = spots.get(currentSpotId);
        if (current != null) {
            current.lifecycle(new ZLinkBackendActorLifecycleEvent(
                ZLinkBackendActorLifecycleEventKind.JOINED,
                new ZLinkBackendActorLifecycleInfo(
                    null,
                    actor,
                    Optional.ofNullable(previousSpotId),
                    Optional.of(currentSpotId),
                    epoch,
                    0)));
        }
    }

    private void notifyLeft(
        String previousSpotId,
        ZLinkBackendActorRef actor,
        long epoch) {
        ZLinkJavaMeshSpot previous = spots.get(previousSpotId);
        if (previous != null) {
            previous.lifecycle(new ZLinkBackendActorLifecycleEvent(
                ZLinkBackendActorLifecycleEventKind.LEFT,
                new ZLinkBackendActorLifecycleInfo(
                    actor,
                    null,
                    Optional.of(previousSpotId),
                    Optional.empty(),
                    epoch,
                    0)));
        }
    }
}
