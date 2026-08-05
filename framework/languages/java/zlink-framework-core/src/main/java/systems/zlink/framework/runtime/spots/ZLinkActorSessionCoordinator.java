package systems.zlink.framework.runtime.spots;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorReplyRoute;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkActorSessionCoordinator {
    record ActorRoute(
        Optional<String> joinedSpotId,
        ZLinkBackendActorRef actorRef,
        boolean remoteJoinedSpot) {
    }

    record LocalDispatch(
        ZLinkActor actor,
        Optional<String> joinedSpotId) {
    }

    private ZLinkActorRuntime actors;

    void attach(
        ZLinkActorRuntime actors,
        ZLinkActorRuntime.CreatedNotifier createdNotifier,
        Supplier<Object> actorCreateContextSupplier,
        Function<ZLinkActor, CompletionStage<Void>> disconnectedNotifier,
        ZLinkActorRuntime.SourceActorLeaver sourceActorLeaver,
        Function<String, ZLinkSpot<?>> spotResolver,
        Function<String, String> spotMeshResolver) {
        this.actors = actors;
        actors.setCreatedNotifier(createdNotifier);
        actors.setActorCreateContextSupplier(actorCreateContextSupplier);
        actors.setDisconnectedNotifier(disconnectedNotifier);
        actors.setSourceActorLeaver(sourceActorLeaver);
        actors.setSpotResolver(spotResolver);
        actors.setSpotMeshResolver(spotMeshResolver);
    }

    boolean available() {
        return actors != null;
    }

    boolean hasActorsInSpot(String spotId) {
        return actors != null && actors.hasActorsInSpot(spotId);
    }

    List<String> actorIdsInSpot(String spotId) {
        return actors == null
            ? List.of()
            : actors.actorIdsInSpot(spotId);
    }

    Optional<systems.zlink.framework.execution.ZLinkAsyncSerialQueue.RelocationSeal>
        trySealActorRelocation(String actorId) {
        return requireActors().trySealActorRelocation(actorId);
    }

    systems.zlink.framework.execution.ZLinkAsyncSerialQueue
        actorRelocationLane(String actorId) {
        return requireActors().actorRelocationLane(actorId);
    }

    boolean abortActorRelocation(
        String actorId,
        systems.zlink.framework.execution.ZLinkAsyncSerialQueue.RelocationSeal
            seal) {
        return requireActors().abortActorRelocation(actorId, seal);
    }

    Optional<List<systems.zlink.framework.execution.ZLinkAsyncSerialQueue.QueuedRecord>>
        commitActorRelocation(
            String actorId,
            systems.zlink.framework.execution.ZLinkAsyncSerialQueue.RelocationSeal
                seal) {
        return requireActors().commitActorRelocation(actorId, seal);
    }

    Optional<List<systems.zlink.framework.execution.ZLinkAsyncSerialQueue.QueuedRecord>>
        freezeActorRelocationIngress(
            String actorId,
            systems.zlink.framework.execution.ZLinkAsyncSerialQueue.RelocationSeal
                seal) {
        return requireActors().freezeActorRelocationIngress(actorId, seal);
    }

    Optional<ZLinkActor> localActor(String actorId) {
        return actors == null ? Optional.empty() : actors.localActor(actorId);
    }

    String actorType(String actorId) {
        ZLinkActor actor = localActor(actorId).orElseThrow(() ->
            new ZLinkConfigurationException(
                "local actor is not available: " + actorId));
        return requireActors().actorTypeFor(actor);
    }

    String actorMeshName(String actorId) {
        return localActor(actorId).orElseThrow(() ->
            new ZLinkConfigurationException(
                "local actor is not available: " + actorId))
            .context().meshName();
    }

    ZLinkBackendActorRef actorRef(String actorId) {
        ZLinkActor actor = localActor(actorId).orElseThrow(() ->
            new ZLinkConfigurationException(
                "local actor is not available: " + actorId));
        return requireActors().currentRef(actor);
    }

    CompletionStage<ZLinkActorRuntime.PreparedTransferredActor>
        prepareRelocatedActor(
            String actorId,
            String actorType,
            byte[] state,
            boolean restoreSnapshot,
            systems.zlink.framework.runtime.internal.relocation
                .ZLinkRelocationAdapterRegistry adapters,
            systems.zlink.framework.actors.ZLinkRelocationCancellation
                cancellation,
            ZLinkBackendActorRef actorRef) {
        return requireActors().prepareRelocatedActor(
            actorId,
            actorType,
            state,
            restoreSnapshot,
            adapters,
            cancellation,
            actorRef);
    }

    void publishRelocatedActor(
        ZLinkActorRuntime.PreparedTransferredActor actor,
        String targetSpotId,
        long authorityOwnerGeneration) {
        requireActors().publishPreparedTransferredActor(
            actor,
            targetSpotId,
            authorityOwnerGeneration);
    }

    void openRelocatedActorAdmission(
        ZLinkActorRuntime.PreparedTransferredActor actor) {
        requireActors().completePreparedTransferredActor(actor);
    }

    CompletionStage<Void> discardRelocatedActor(
        ZLinkActorRuntime.PreparedTransferredActor actor) {
        return requireActors().discardPreparedTransferredActor(actor);
    }

    boolean hasBoundSession(String actorId) {
        ZLinkActor actor = localActor(actorId).orElseThrow(() ->
            new ZLinkConfigurationException(
                "local actor is not available: " + actorId));
        return requireActors().hasBoundSession(actor);
    }

    Optional<ZLinkActorRuntime.BoundSessionRouteSnapshot> boundSessionRoute(
        String actorId) {
        ZLinkActor actor = localActor(actorId).orElseThrow(() ->
            new ZLinkConfigurationException(
                "local actor is not available: " + actorId));
        return requireActors().boundSessionRoute(actor);
    }

    CompletionStage<Void> completeRelocationSource(List<String> actorIds) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (String actorId : List.copyOf(actorIds)) {
            chain = chain.thenCompose(ignored ->
                requireActors().completeRelocationSource(actorId));
        }
        return chain;
    }

    boolean isActorMember(String spotId, String actorId) {
        if (actors == null) {
            return false;
        }
        return actors.localActor(actorId)
            .flatMap(actors::spotId)
            .filter(spotId::equals)
            .isPresent();
    }

    CompletionStage<Void> dispatch(
        ZLinkActor actor,
        Supplier<CompletionStage<Void>> operation) {
        return requireActors().submitActorDispatch(actor.context().actorId(), operation);
    }

    CompletionStage<Optional<Message>> captureMoving(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute) {
        return requireActors().captureMovingPacket(actor, header, payload, replyRoute);
    }

    Optional<ZLinkBackendActorRef> messageFollowTargetActorRef(
        ZLinkActor actor) {
        return requireActors().messageFollowTargetActorRef(actor);
    }

    boolean isMoving(ZLinkActor actor) {
        return requireActors().isMoving(actor);
    }

    CompletionStage<Void> awaitMoveCompletion(ZLinkActor actor) {
        return requireActors().awaitMoveCompletion(actor);
    }

    CompletionStage<Optional<Message>> dispatchLocalSession(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        Predicate<String> isLocalSpot,
        Function<LocalDispatch, CompletionStage<Optional<Message>>> localDispatch) {
        return dispatchLocalSession(
            actorRef, header, payload, isLocalSpot, localDispatch, true);
    }

    CompletionStage<Optional<Message>> dispatchTransferBacklog(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        byte[] acceptedJournalRecord,
        Predicate<String> isLocalSpot,
        Function<LocalDispatch, CompletionStage<Optional<Message>>> localDispatch) {
        ZLinkActorAcceptedJournal.Record accepted =
            ZLinkActorAcceptedJournal.decode(acceptedJournalRecord);
        if (!accepted.actorId().equals(actorRef.actorId())
            || accepted.objectGeneration() != actorRef.generation()
            || !accepted.header().packetName().equals(header.packetName())
            || accepted.header().requestSequence().isPresent()
            || !java.util.Arrays.equals(accepted.payload(), payload.toByteArray())) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "Actor handoff record does not match its frozen target and payload"));
        }
        ZLinkActorRuntime runtime = requireActors();
        Optional<ZLinkActor> localActor = runtime.localActor(actorRef.actorId());
        if (localActor.isEmpty()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "local actor is not available: " + actorRef.actorId()));
        }
        ZLinkActor actor = localActor.get();
        Optional<String> joinedSpotId = runtime.spotId(actor);
        CompletableFuture<Optional<Message>> result = new CompletableFuture<>();
        return runtime.submitActorDispatch(
                actor.context().actorId(),
                acceptedJournalRecord,
                () -> {
                    if (!runtime.claimAcceptedHandoffOperation(
                            actor,
                            accepted.operationHigh(),
                            accepted.operationLow())) {
                        result.complete(Optional.empty());
                        return CompletableFuture.completedFuture(null);
                    }
                    return invokeLocalDispatch(
                        localDispatch,
                        new LocalDispatch(actor, joinedSpotId),
                        result);
                })
            .thenCompose(ignored -> result);
    }

    private CompletionStage<Optional<Message>> dispatchLocalSession(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        Predicate<String> isLocalSpot,
        Function<LocalDispatch, CompletionStage<Optional<Message>>> localDispatch,
        boolean captureMovingPacket) {
        ZLinkActorRuntime runtime = requireActors();
        Optional<ZLinkActor> localActor = runtime.localActor(actorRef.actorId());
        if (localActor.isEmpty()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "local actor is not available: " + actorRef.actorId()));
        }
        ZLinkActor actor = localActor.get();
        if (captureMovingPacket && runtime.isMoving(actor)) {
            CompletionStage<Optional<Message>> captured =
                runtime.captureMovingPacket(actor, header, payload);
            if (captured != null) {
                return captured;
            }
            return runtime.awaitMoveCompletion(actor)
                .thenCompose(ignored -> dispatchLocalSession(
                    actorRef,
                    header,
                    payload,
                    isLocalSpot,
                    localDispatch));
        }
        Optional<String> joinedSpotId = runtime.spotId(actor);
        if (joinedSpotId.isPresent()
            && currentSpotSurface(actor) == null
            && !isLocalSpot.test(joinedSpotId.get())
            && runtime.canRouteRemoteJoinedSpot(joinedSpotId.get())) {
            return runtime.dispatchRemoteJoinedActor(
                runtime.currentRef(actor),
                joinedSpotId.get(),
                header,
                payload);
        }
        if (!captureMovingPacket) {
            // Transfer commit already owns the actor's serialized turn. Queuing
            // replay behind that turn would wait on the commit that is waiting
            // for this replay to finish.
            return localDispatch.apply(new LocalDispatch(actor, joinedSpotId));
        }
        CompletableFuture<Optional<Message>> result = new CompletableFuture<>();
        byte[] acceptedRecord = runtime.encodeLocalSessionActorAccepted(
            actor,
            header,
            payload);
        if (acceptedRecord.length == 0) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "canonical local Session Actor journal context is unavailable"));
        }
            return runtime.submitActorDispatch(
                actor.context().actorId(),
                ZLinkActorAcceptedJournal.encode(
                    actor.context().actorId(),
                    header,
                    payload,
                    acceptedRecord),
                () -> invokeLocalDispatch(
                    localDispatch,
                    new LocalDispatch(actor, joinedSpotId),
                    result))
            .thenCompose(ignored -> result);
    }

    CompletionStage<Optional<Message>> runPacketTurn(
        ZLinkActor actor,
        boolean request,
        boolean noBindRequest,
        ZLinkBackendActorReceived headerPart,
        ZLinkInternalSpotNode primaryNode,
        Supplier<CompletionStage<Optional<Message>>> operation,
        Runnable relocationRelease) {
        ZLinkActorRuntime runtime = requireActors();
        if (request
            && !noBindRequest
            && !runtime.hasBoundSession(
                actor,
                headerPart.sourceNodeRid(),
                headerPart.sourceSessionRid())) {
            runtime.bindNativeSession(
                actor,
                primaryNode,
                headerPart.actor(),
                headerPart.sourceNodeRid(),
                headerPart.sourceSessionRid());
        }
        String actorId = actor.context().actorId();
        CompletableFuture<Optional<Message>> result =
            new CompletableFuture<>();
        Supplier<CompletionStage<Void>> turn = () ->
            operation.get().handle((reply, failure) -> {
                if (failure == null) {
                    result.complete(reply);
                } else {
                    result.completeExceptionally(failure);
                }
                return null;
            });
        byte[] acceptedRecord = headerPart.acceptedJournalRecord();
        CompletionStage<Void> submitted = acceptedRecord.length == 0
            ? runtime.submitActorDispatch(actorId, turn)
            : runtime.submitActorDispatch(
                actorId,
                acceptedRecord,
                turn,
                () -> {
                    relocationRelease.run();
                    result.complete(Optional.empty());
                });
        return submitted
            .thenCompose(ignored -> result);
    }

    boolean hasBoundSession(ZLinkActor actor) {
        return requireActors().hasBoundSession(actor);
    }

    void bindNativeSession(
        ZLinkActor actor,
        ZLinkInternalSpotNode node,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        requireActors().bindNativeSession(
            actor,
            node,
            actorRef,
            sourceNodeRid,
            sourceSessionRid);
    }

    boolean isJoinedTo(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId) {
        if (actors == null || actor == null || actorRef == null || spotId == null) {
            return false;
        }
        return actors.spotId(actor).filter(spotId::equals).isPresent()
            && actorRef.equals(actors.currentRef(actor));
    }

    boolean isJoinedToDifferentSpot(ZLinkActor actor, String spotId) {
        if (actors == null || actor == null || spotId == null) {
            return false;
        }
        return actors.spotId(actor)
            .map(current -> !spotId.equals(current))
            .orElse(false);
    }

    Object spotSurface(
        ZLinkActor actor,
        Function<String, Object> resolver,
        Supplier<Object> fallback) {
        Object current = currentSpotSurface(actor);
        if (current != null) {
            return current;
        }
        if (actors != null) {
            Object resolved = actors.spotId(actor).map(resolver).orElse(null);
            if (resolved != null) {
                return resolved;
            }
        }
        return fallback.get();
    }

    ActorRoute routeFor(
        ZLinkActor actor,
        RoutingId localNodeRid,
        Predicate<String> isLocalSpot) {
        ZLinkActorRuntime runtime = requireActors();
        Optional<String> joinedSpotId = runtime.spotId(actor);
        ZLinkBackendActorRef actorRef = runtime.currentRef(actor);
        boolean remote = joinedSpotId.isPresent()
            && !actorRef.nodeRid().equals(localNodeRid)
            && !isLocalSpot.test(joinedSpotId.get())
            && runtime.canRouteRemoteJoinedSpot(joinedSpotId.get());
        return new ActorRoute(joinedSpotId, actorRef, remote);
    }

    CompletionStage<Void> sendBoundSession(
        ZLinkBackendActorRef actorRef,
        byte[] frameBytes,
        Supplier<CompletionStage<Void>> fallback) {
        ZLinkActorRuntime runtime = requireActors();
        return runtime.localActor(actorRef.actorId())
            .map(actor -> runtime.sendBoundSessionFrame(actor, frameBytes)
                .thenCompose(sent -> sent
                    ? CompletableFuture.completedFuture(null)
                    : fallback.get()))
            .orElseGet(fallback);
    }

    private static CompletionStage<Void> invokeLocalDispatch(
        Function<LocalDispatch, CompletionStage<Optional<Message>>> dispatch,
        LocalDispatch local,
        CompletableFuture<Optional<Message>> result) {
        try {
            return dispatch.apply(local)
                .whenComplete((reply, error) -> {
                    if (error != null) {
                        result.completeExceptionally(error);
                    } else {
                        result.complete(reply);
                    }
                })
                .thenApply(ignored -> null);
        } catch (RuntimeException error) {
            result.completeExceptionally(error);
            return CompletableFuture.failedFuture(error);
        }
    }

    private ZLinkActorRuntime requireActors() {
        if (actors == null) {
            throw new ZLinkConfigurationException("actor runtime is required for Spot actor operation");
        }
        return actors;
    }

    ZLinkActorRuntime runtime() {
        return requireActors();
    }

    private Object currentSpotSurface(ZLinkActor actor) {
        return requireActors().currentSpot(actor);
    }
}
