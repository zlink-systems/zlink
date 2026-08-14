package systems.zlink.framework.runtime.spots;
import java.util.UUID;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.function.BooleanSupplier;
import java.util.logging.Logger;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.spots.ZLinkEntrySpot;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ActorRef;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferId;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferRole;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinAdmissionProfileCodec;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;

final class ZLinkActorSpotAdmission {
    record RoutedJoin(
        ZLinkBackendActorRef actorRef,
        ZLinkSpotActorJoinResult response,
        List<Message> handoffReplies) {
    }

    private record CommittedJoin(
        ZLinkActorRuntime.PreparedTransferredActor prepared,
        systems.zlink.framework.runtime.internal.locations
            .ZLinkDirectJoinRelocationAuthority.CommittedActorTenure tenure) {
    }

    private record BoundSessionFence(long bindingGeneration) {
    }

    private record BoundSessionRouteUpdate(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command,
        ZLinkBackendActorRef targetActor) {
    }

    /** Owns the target-side ordering required before Ready is observable. */
    static <T> CompletionStage<T> completeTargetBeforeReady(
        Supplier<CompletionStage<Void>> lifecycle,
        Supplier<CompletionStage<Void>> joinCompletion,
        Supplier<CompletionStage<T>> replay,
        Runnable dispatchSwitch,
        Supplier<CompletionStage<Void>> publishReady) {
        return lifecycle.get()
            .thenCompose(ignored -> joinCompletion.get())
            .thenCompose(ignored -> replay.get())
            .thenApply(result -> {
                dispatchSwitch.run();
                return result;
            })
            .thenCompose(result -> publishReady.get()
                .thenApply(ignored -> result));
    }

    private ZLinkActorRuntime actors;
    private ZLinkSessionRelocationPeerClient sessionRoutes;
    private BooleanSupplier draining = () -> false;
    private final ZLinkPendingActorTransfers pendingTransfers =
        new ZLinkPendingActorTransfers();
    private final ZLinkActorTransferCommitRegistry commitRegistry =
        new ZLinkActorTransferCommitRegistry();
    private final ConcurrentMap<String, CompletableFuture<Void>>
        pendingEntryJoins = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, CompletableFuture<Void>>
        pendingLeaves = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, LocalJoin> pendingLocalJoins =
        new ConcurrentHashMap<>();

    private record LocalJoin(
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
    }

    void attach(
        ZLinkActorRuntime actors,
        BooleanSupplier draining,
        ZLinkSessionRelocationPeerClient sessionRoutes) {
        this.actors = actors;
        this.draining = draining == null ? () -> false : draining;
        this.sessionRoutes = sessionRoutes;
    }

    void traceTransferMarker(String marker, String actorId, long arrivalIndex) {
        requireActors().traceActorTransferMarker(marker, actorId, Long.toString(arrivalIndex));
    }

    boolean isActorAtSpot(String actorId, String spotId) {
        return requireActors().isActorAtSpot(actorId, spotId);
    }

    Object deferredJoinRuntimeScope() {
        ZLinkActorRuntime runtime = actors;
        return runtime == null ? this : runtime;
    }

    CompletionStage<Void> destroyFromEntry(RoutingId nodeRid, ZLinkActor actor) {
        return requireActors().destroyFromEntrySpot(nodeRid, actor);
    }

    CompletionStage<Void> leaveSpot(
        ZLinkInternalSpotNode node,
        ZLinkActor actor,
        String fallbackSpotId,
        RoutingId entryNodeRid,
        Duration timeout) {
        ZLinkActorRuntime runtime = requireActors();
        String currentSpotId = runtime.spotId(actor).orElse(fallbackSpotId);
        ZLinkBackendActorRef actorRef = runtime.actorRef(actor);
        CompletableFuture<Void> entryJoined = entryNodeRid == null
            ? null
            : new CompletableFuture<>();
        CompletableFuture<Void> actorLeft = new CompletableFuture<>();
        if (entryJoined != null) {
            CompletableFuture<Void> previous = pendingEntryJoins.putIfAbsent(
                actor.context().actorId(), entryJoined);
            if (previous != null) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor Entry Spot join is already pending: " + actor.context().actorId()));
            }
        }
        CompletableFuture<Void> previousLeave = pendingLeaves.putIfAbsent(
            actor.context().actorId(), actorLeft);
        if (previousLeave != null) {
            if (entryJoined != null) {
                pendingEntryJoins.remove(actor.context().actorId(), entryJoined);
            }
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor leave is already pending: " + actor.context().actorId()));
        }
        CompletionStage<Void> leaving = node.leaveActor(actorRef, currentSpotId, timeout)
            .whenComplete((replyParts, error) -> {
                if (replyParts != null) {
                    replyParts.forEach(Message::close);
                }
                if (error != null && entryJoined != null
                    && pendingEntryJoins.remove(actor.context().actorId(), entryJoined)) {
                    entryJoined.completeExceptionally(error);
                }
                if (error != null && pendingLeaves.remove(actor.context().actorId(), actorLeft)) {
                    actorLeft.completeExceptionally(error);
                }
            })
            .thenCompose(ignored -> actorLeft)
            .thenCompose(ignored -> entryNodeRid == null
                ? CompletableFuture.completedFuture(null)
                : joinEntrySpotAfterLeave(runtime, actor, entryNodeRid, timeout)
                    .thenCompose(joined -> entryJoined));
        return ZLinkAsyncSerialQueue.manageCurrent(leaving);
    }

    private static CompletionStage<Void> joinEntrySpotAfterLeave(
        ZLinkActorRuntime runtime,
        ZLinkActor actor,
        RoutingId entryNodeRid,
        Duration timeout) {
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterApplicationExecution(null)) {
            return runtime.joinEntrySpot(actor, entryNodeRid, timeout);
        }
    }

    CompletionStage<Void> markLeft(ZLinkActor actor) {
        return requireActors().markLeft(actor);
    }

    CompletionStage<Void> leaveRoutedActorToLocalEntry(
        ZLinkActor actor,
        RoutingId entryNodeRid,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> admissionCallback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        ZLinkActorRuntime runtime = requireActors();
        return invokeAdmissionCallback(admissionCallback, actor.context().actorId())
            .thenCompose(response -> effectiveResponse(response).accepted()
                ? runtime.leaveSourceForLocalMove(actor)
                : CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor Entry Spot join was rejected: " + actor.context().actorId())))
            .thenCompose(ignored -> runtime.commitEntryLocation(actor, entryNodeRid))
            .thenRun(() -> runtime.completeRemoteMove(actor))
            .thenCompose(ignored -> runtime.invokeActorLifecycle(
                actor,
                () -> joinedCallback.apply(actor)));
    }

    CompletionStage<Void> markJoined(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        return requireActors().markJoined(actor, actorRef, spotId, spot);
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitEntryActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback) {
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        String actorId = request.targetActor().actorId();
        return invokeAdmissionCallback(callback, actorId)
            .thenApply(ZLinkActorSpotAdmission::effectiveResponse)
            .whenComplete((response, error) -> {
                if (error != null || response == null || !response.accepted()) {
                    completeEntryJoin(actorId, error == null
                        ? new ZLinkConfigurationException(
                            "actor Entry Spot join was rejected: " + actorId)
                        : error);
                }
            });
    }

    CompletionStage<Void> completeEntryActorJoin(
        ZLinkBackendActorJoinRequest request,
        RoutingId entryNodeRid,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        String actorId = request.targetActor().actorId();
        ZLinkActorRuntime runtime = requireActors();
        return runtime.getOrCreateLocalActor(actorId, ZLinkActor.class)
            .thenCompose(actor -> actor
                .map(value -> {
                    // Entry Spot membership is framework infrastructure state;
                    // it must not publish a durable user-Spot join.
                    runtime.markJoinedEntrySpot(
                        value, request.targetActor(), entryNodeRid);
                    return joinedCallback.apply(value);
                })
                .orElseGet(() -> CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "Entry Spot actor is not available: " + actorId))))
            .whenComplete((ignored, error) -> completeEntryJoin(actorId, error));
    }

    void completeEntryJoin(String actorId, Throwable error) {
        CompletableFuture<Void> pending = pendingEntryJoins.remove(actorId);
        if (pending == null) {
            return;
        }
        if (error == null) {
            pending.complete(null);
        } else {
            pending.completeExceptionally(error);
        }
    }

    boolean isEntryJoinPending(String actorId) {
        return pendingEntryJoins.containsKey(actorId);
    }

    void completeLeave(String actorId, Throwable error) {
        CompletableFuture<Void> pending = pendingLeaves.remove(actorId);
        if (pending == null) {
            return;
        }
        if (error == null) {
            pending.complete(null);
        } else {
            pending.completeExceptionally(error);
        }
    }

    boolean isLeavePending(String actorId) {
        return pendingLeaves.containsKey(actorId);
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitSpotActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        ZLinkSpot<?> spotSurface,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        return admitNativeActor(request, spotId, spotSurface, callback, joinedCallback);
    }

    private CompletionStage<ZLinkSpotActorJoinResult> admitNativeActor(
        ZLinkBackendActorJoinRequest request,
        String spotId,
        ZLinkSpot<?> spotSurface,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback) {
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        String actorId = request.targetActor().actorId();
        return invokeAdmissionCallback(callback, actorId)
            .thenCompose(response -> {
                ZLinkSpotActorJoinResult effective = effectiveResponse(response);
                if (!effective.accepted()) {
                    return CompletableFuture.completedFuture(effective);
                }
                LocalJoin pending = new LocalJoin(
                    request.targetActor(), spotId, spotSurface, joinedCallback);
                LocalJoin previous = pendingLocalJoins.putIfAbsent(actorId, pending);
                if (previous != null) {
                    return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                        "local actor Spot join is already pending: " + actorId));
                }
                return CompletableFuture.completedFuture(effective);
            });
    }

    CompletionStage<Void> completeLocalJoinFromCaller(ZLinkActor actor) {
        LocalJoin pending = pendingLocalJoins.remove(actor.context().actorId());
        if (pending == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "local actor Spot admission is missing: " + actor.context().actorId()));
        }
        ZLinkActorRuntime runtime = requireActors();
        ZLinkActorRuntime.LocalMoveSource source = runtime.beginLocalMove(actor);
        return runtime.commitJoinedLocation(actor, pending.actorRef(), pending.spotId())
            .thenCompose(ignored -> runtime.markJoined(
                actor, pending.actorRef(), pending.spotId(), pending.spot()))
            .thenCompose(ignored -> pending.joinedCallback().apply(actor))
            .thenRun(() -> {
                runtime.notifySourceForLocalMove(actor, source);
                runtime.completeRemoteMove(actor);
            })
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    runtime.failRemoteMove(actor, error);
                }
            });
    }

    void cancelLocalJoin(ZLinkActor actor) {
        if (actor != null) {
            pendingLocalJoins.remove(actor.context().actorId());
        }
    }

    CompletionStage<ZLinkSpotActorJoinResult> prepareRoutedActor(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        String routeChannelName,
        RoutingId sourcePeerRid,
        String targetSpotId,
        Object targetSpot,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback,
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback) {
        if (!request.admission()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor transfer admission request has the wrong phase"));
        }
        if (draining.getAsBoolean()) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }
        requireActors().traceActorTransferMarker(
            "target_admission_received", request.actorId(), request.transferId());
        return invokeAdmissionCallback(callback, request.actorId())
            .thenApply(ZLinkActorSpotAdmission::effectiveResponse)
            .thenApply(response -> {
                if (!response.accepted()) {
                    return response;
                }
                var operationId = ZLinkActorJoinAdmissionProfileCodec.decode(
                    request.adapterKey());
                if (operationId.isPresent()) {
                    requireActors().admitActorJoin(
                        new ZLinkActorJoinRelocationPort.Admission(
                            UUID.fromString(request.transferId()),
                            operationId.orElseThrow(),
                            request.actorId(),
                            targetSpotId,
                            targetSpot,
                            joinedCallback,
                            response.reply() == null
                                ? ZLinkMessage.empty()
                                : response.reply(),
                            Duration.ofMillis(Math.max(
                                1L, request.timeoutMillis()))));
                } else {
                    pendingTransfers.add(
                        request, routeChannelName, sourcePeerRid);
                }
                requireActors().traceActorTransferMarker(
                    "target_admission_accepted", request.actorId(), request.transferId());
                return response;
            });
    }

    CompletionStage<RoutedJoin> commitRoutedActor(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkMessage transferState,
        ZLinkInternalSpotNode primaryNode,
        String spotId,
        Object spotSurface,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback,
        Function<ZLinkBackendActorRef, CompletionStage<List<Message>>> backlogReplay) {
        return commitRegistry.execute(
            request,
            () -> commitRoutedActorOnce(
                request,
                transferState,
                primaryNode,
                spotId,
                spotSurface,
                joinedCallback,
                backlogReplay));
    }

    private CompletionStage<RoutedJoin> commitRoutedActorOnce(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkMessage transferState,
        ZLinkInternalSpotNode primaryNode,
        String spotId,
        Object spotSurface,
        Function<ZLinkActor, CompletionStage<Void>> joinedCallback,
        Function<ZLinkBackendActorRef, CompletionStage<List<Message>>> backlogReplay) {
        if (!request.commit()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor transfer commit request has the wrong phase"));
        }
        ZLinkPendingActorTransfers.Admission pending;
        try {
            pending = pendingTransfers.take(request);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }

        ZLinkActorRuntime runtime = requireActors();
        runtime.traceActorTransferMarker(
            "target_commit_received", request.actorId(), request.transferId());
        PrepareActorTransferResult corePrepared = null;
        if (request.coreTransfer()) {
            runtime.traceActorTransferMarker(
                "core_target_prepare_start",
                request.actorId(),
                Long.toUnsignedString(request.coreTransferIdHigh())
                    + ":" + Long.toUnsignedString(request.coreTransferIdLow())
                    + ":" + Long.toUnsignedString(request.coreFinalSequence()));
            corePrepared = primaryNode.prepareActorTransfer(
                new ActorTransferPrepare(
                    ActorTransferRole.TARGET,
                    new ActorTransferId(
                        request.coreTransferIdHigh(),
                        request.coreTransferIdLow()),
                    new ActorRef(
                        request.actorNodeRid(),
                        request.actorId(),
                        request.actorGeneration()),
                    request.coreMembershipEpoch(),
                    request.actorNodeRid(),
                    request.coreFinalSequence(),
                    request.coreReserveMessageCount(),
                    request.coreReserveByteCount()),
                Duration.ofMillis(Math.max(1L, request.timeoutMillis())));
            runtime.traceActorTransferMarker(
                "core_target_prepared",
                request.actorId(),
                Long.toUnsignedString(corePrepared.result().transferId().high())
                    + ":" + Long.toUnsignedString(corePrepared.result().transferId().low())
                    + ":" + Long.toUnsignedString(corePrepared.result().finalSequence()));
        }
        PrepareActorTransferResult preparedFence = corePrepared;
        AtomicReference<ZLinkActorRuntime.PreparedTransferredActor> staged =
            new AtomicReference<>();
        AtomicBoolean aggregateCommitted = new AtomicBoolean();
        CompletionStage<RoutedJoin> attempt =
            runtime.loadDeferredJoinRelocation(request)
            .thenCompose(root -> runtime.prepareDeferredJoinTarget(
                request,
                transferState,
                root))
            .thenCompose(prepared -> {
                staged.set(prepared);
                if (preparedFence != null) {
                    primaryNode.commitActorTransfer(
                        preparedFence.token(),
                        request.coreMembershipEpoch() + 1);
                }
                return runtime.commitDeferredJoinRelocation(request)
                    .thenApply(tenure -> {
                        aggregateCommitted.set(true);
                        runtime.traceActorTransferMarker(
                            "location_committed",
                            request.actorId(),
                            request.transferId());
                        primaryNode.registerTransferredActor(
                            prepared.actorRef(),
                            spotId,
                            request.coreMembershipEpoch() + 1);
                        primaryNode.rememberActorAuthority(
                            tenure.actor(),
                            tenure.authorityOwnerGeneration(),
                            tenure.ownerLeaseGeneration());
                        return new CommittedJoin(
                            prepared,
                            tenure);
                    });
            })
            .thenCompose(committed -> {
                ZLinkActorRuntime.PreparedTransferredActor prepared =
                    committed.prepared();
                ZLinkActor actor =
                    runtime.publishPreparedTransferredActor(prepared);
                runtime.setEntrySpotNodeRid(actor, request.sourceEntrySpotNodeRid());
                runtime.setEntrySpotId(actor, request.sourceEntrySpotId());
                runtime.setEntryRouterChannelId(actor, request.sourceEntryRouterChannelId());
                runtime.traceActorTransferMarker(
                    "target_materialized", actor.context().actorId(), request.transferId());
                ZLinkBackendActorRef actorRef = runtime.actorRef(actor);
                bindRoutedTransfer(
                    runtime,
                    actor,
                    actorRef,
                    request,
                    pending,
                    primaryNode);
                runtime.traceActorTransferMarker(
                    "target_session_bound", actor.context().actorId(), request.transferId());
                BoundSessionRouteUpdate sessionRoute =
                    prepareBoundSessionRouteUpdate(
                    request,
                    primaryNode,
                    committed.tenure());
                if (sessionRoute != null) {
                    primaryNode.installRelocatingActorBoundSession(
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            sessionRoute.targetActor(),
                            committed.tenure().targetNodeGeneration(),
                            committed.tenure().authorityOwnerGeneration(),
                            committed.tenure().ownerLeaseGeneration()),
                        sessionRoute.command().session());
                }
                // Command 44 is one-way and is submitted by the target after
                // target authority and the local route are installed.
                startBoundSessionRouteUpdate(
                    request,
                    primaryNode,
                    sessionRoute);
                return ZLinkAsyncSerialQueue
                    .yieldCurrent(CompletableFuture.completedFuture(null))
                    .thenCompose(ignored -> {
                        boolean entryTarget = spotSurface instanceof ZLinkEntrySpot<?>;
                        if (!entryTarget) {
                            runtime.markJoined(
                                actor,
                                actorRef,
                                spotId,
                                (ZLinkSpot<?>) spotSurface);
                        } else {
                            runtime.markJoinedEntrySpot(
                                actor,
                                actorRef,
                                primaryNode.routingId());
                        }
                        if (preparedFence != null) {
                            // A joined lifecycle callback may send through the bound
                            // session. Activate the committed transfer before calling
                            // user code so that this outbound path does not wait on
                            // the callback that is required to complete the transfer.
                            primaryNode.activateActorTransfer(preparedFence.token());
                        }
                        return completeTargetBeforeReady(
                                () -> joinedCallback.apply(actor)
                                    .thenRun(() ->
                                        runtime.traceActorTransferMarker(
                                            "target_joined_callback",
                                            actor.context().actorId(),
                                            request.transferId())),
                                () -> runtime.deliverDeferredJoinAccepted(
                                    request,
                                    actorRef),
                                () -> backlogReplay.apply(actorRef)
                                    .thenApply(replies -> {
                                        runtime.traceActorTransferMarker(
                                            "target_backlog_replayed",
                                            actor.context().actorId(),
                                            request.transferId());
                                        return replies;
                                    }),
                                () -> {
                                    completeRemoteMove(runtime, prepared);
                                    runtime.traceActorTransferMarker(
                                        "target_dispatch_switched",
                                        actor.context().actorId(),
                                        request.transferId());
                                },
                                () -> runtime.publishDeferredJoinTargetReady(
                                    request, actorRef))
                            .thenApply(replies -> {
                                runtime.traceActorTransferMarker(
                                    "target_ready", actor.context().actorId(), request.transferId());
                                return replies;
                            })
                            .thenApply(replies -> new RoutedJoin(
                                actorRef,
                                ZLinkSpotActorJoinResult.accept(), replies));
                    });
            });
        return attempt.exceptionallyCompose(error -> {
            if (aggregateCommitted.get()) {
                // Location authority is already terminal. The target Actor
                // remains published with admission sealed so durable recovery
                // can resume callback, replay, and ACK without recreating it.
                return CompletableFuture.failedFuture(error);
            }
            if (preparedFence != null) {
                try {
                    primaryNode.abortActorTransfer(preparedFence.token());
                } catch (RuntimeException ignoredAbort) {
                    // The precommit authority abort below remains decisive.
                }
            }
            ZLinkActorRuntime.PreparedTransferredActor prepared = staged.get();
            CompletionStage<Void> discard = prepared == null
                ? CompletableFuture.completedFuture(null)
                : runtime.discardPreparedTransferredActor(prepared);
            return discard
                .exceptionally(ignored -> null)
                .thenCompose(ignored ->
                    runtime.abortDeferredJoinRelocation(request))
                .handle((ignored, abortError) -> {
                    if (abortError != null) {
                        error.addSuppressed(abortError);
                    }
                    throw new CompletionException(error);
                });
            });
    }

    private BoundSessionRouteUpdate prepareBoundSessionRouteUpdate(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkInternalSpotNode primaryNode,
        systems.zlink.framework.runtime.internal.locations
            .ZLinkDirectJoinRelocationAuthority.CommittedActorTenure tenure) {
        byte[] command44 = request.sessionRouteCommand44();
        if (command44.length == 0) {
            return null;
        }
        if (request.relocationManifest() == null) {
            throw new ZLinkConfigurationException(
                "StateIncompatible: bound-Session direct Join has no durable "
                    + "source-cleanup completion marker");
        }
        if (sessionRoutes == null) {
            throw new ZLinkConfigurationException(
                "bound Session relocation route runtime is unavailable");
        }
        var codec = new ZLinkServiceM6BWireCodec();
        var command = codec.decodeSessionRelocationRoute(command44);
        UUID relocationId =
            UUID.fromString(request.transferId());
        if (command.relocation().high()
                != relocationId.getMostSignificantBits()
            || command.relocation().low()
                != relocationId.getLeastSignificantBits()
            || !command.actor().actorId().equals(request.actorId())
            || command.actor().generation()
                != request.actorGeneration()
            || !command.targetNodeRid().equals(primaryNode.routingId())) {
            throw new ZLinkConfigurationException(
                "bound Session route command does not match direct Join");
        }
        ZLinkBackendActorRef targetActor = new ZLinkBackendActorRef(
            primaryNode.routingId(),
            request.actorId(),
            request.actorGeneration());
        if (!tenure.actor().equals(targetActor)
            || tenure.targetNodeGeneration()
                != command.targetNodeGeneration()
            || tenure.authorityOwnerGeneration()
                != command.currentAuthorityOwnerGeneration()) {
            throw new ZLinkConfigurationException(
                "committed Actor tenure differs from the Session route target");
        }
        return new BoundSessionRouteUpdate(command, targetActor);
    }

    private void startBoundSessionRouteUpdate(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkInternalSpotNode primaryNode,
        BoundSessionRouteUpdate update) {
        if (update == null) {
            return;
        }
        if (draining.getAsBoolean()
            || routeSwitchSuperseded(primaryNode, update.targetActor())) {
            reportBoundSessionRouteUpdateFailure(
                request,
                new ZLinkConfigurationException(
                    "direct-Join Session route handoff was superseded"));
            return;
        }
        switchBoundSessionRoute(request, primaryNode, update);
    }

    private void reportBoundSessionRouteUpdateFailure(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        Throwable failure) {
        requireActors().traceActorTransferMarker(
            "session_route_update_failed",
            request.actorId(),
            request.transferId());
        Logger.getLogger(ZLinkActorSpotAdmission.class.getName())
            .warning("Bound Session route update failed after Actor relocation commit: "
                + failure);
    }

    private void switchBoundSessionRoute(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkInternalSpotNode primaryNode,
        BoundSessionRouteUpdate update) {
        requireActors().traceActorTransferMarker(
            "session_route_update_started",
            request.actorId(),
            request.transferId());
        sessionRoutes.sendRoute(update.command())
            .whenComplete((ignored, failure) -> {
                if (failure != null) {
                    reportBoundSessionRouteUpdateFailure(
                        request, failure);
                    return;
                }
                requireActors().traceActorTransferMarker(
                    "session_route_switched",
                    request.actorId(),
                    request.transferId());
            });
    }

    /**
     * Spec 20 §5.1: only the running target runtime continues the location
     * update retransmission. Once this node no longer holds that exact Actor
     * incarnation the relocation it was announcing is over, so the retry is
     * terminal.
     */
    private static boolean routeSwitchSuperseded(
        ZLinkInternalSpotNode primaryNode,
        ZLinkBackendActorRef targetActor) {
        ZLinkBackendActorRef current;
        try {
            current = primaryNode.actorLookup(targetActor.actorId());
        } catch (RuntimeException unavailable) {
            return false;
        }
        return current == null || !current.equals(targetActor);
    }

    private static ZLinkBackendActorRef completeRemoteMove(
        ZLinkActorRuntime runtime,
        ZLinkActorRuntime.PreparedTransferredActor prepared) {
        runtime.completePreparedTransferredActor(prepared);
        return prepared.actorRef();
    }

    private static long bindRoutedTransfer(
        ZLinkActorRuntime runtime,
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkPendingActorTransfers.Admission pending,
        ZLinkInternalSpotNode primaryNode) {
        if (request.hasSourceSessionRoute()) {
            BoundSessionFence fence = boundSessionFence(request);
            return runtime.bindNativeSession(
                actor,
                primaryNode,
                actorRef,
                request.sourceNodeRid(),
                request.sourceSessionRid(),
                fence.bindingGeneration(),
                0);
        }
        if (pending.routeChannelName() != null || pending.sourcePeerRid() != null) {
            return runtime.bindRoutedSession(
                actor,
                pending.routeChannelName(),
                pending.sourcePeerRid() == null
                    ? request.actorRef().nodeRid()
                    : pending.sourcePeerRid(),
                request.sourceEntrySpotId(),
                request.actorRef());
        }
        return runtime.bindNativeSession(actor, primaryNode, actorRef);
    }

    private static BoundSessionFence boundSessionFence(
        ZLinkActorSpotRoutePackets.TransferRequest request) {
        byte[] command44 = request.sessionRouteCommand44();
        if (command44.length == 0) {
            return new BoundSessionFence(0);
        }
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command =
            new ZLinkServiceM6BWireCodec()
                .decodeSessionRelocationRoute(command44);
        return new BoundSessionFence(
            command.session().bindingGeneration());
    }

    private static CompletionStage<ZLinkSpotActorJoinResult> invokeAdmissionCallback(
        Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback,
        String actorId) {
        try {
            return callback.apply(actorId);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private static void clearBinding(
        ZLinkActorRuntime runtime,
        ZLinkActor actor,
        long bindingToken) {
        if (bindingToken >= 0) {
            runtime.clearSessionBinding(actor, bindingToken);
        }
    }

    private static ZLinkSpotActorJoinResult effectiveResponse(
        ZLinkSpotActorJoinResult response) {
        return response == null ? ZLinkSpotActorJoinResult.reject() : response;
    }

    private ZLinkActorRuntime requireActors() {
        if (actors == null) {
            throw new ZLinkConfigurationException(
                "actor runtime is required for Spot actor admission");
        }
        return actors;
    }

    ZLinkActorRuntime runtime() {
        return requireActors();
    }
}
