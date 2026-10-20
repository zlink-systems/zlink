package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.drain.AsyncDrainProbe;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.mesh.ZLinkActivationAdmission;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;

/**
 * Owns one independent Actor target attempt. Factory and Restore finish while the Actor is absent
 * from the live registry. Publication and admission are separate so the caller can keep the target
 * closed through durable replay.
 */
final class ZLinkStandaloneActorRelocationStagingOwner {
    private static final ZLinkRelocationCancellation NOT_CANCELLED = () -> false;

    private final Backend backend;
    private final ZLinkActivationAdmission activationAdmission;

    ZLinkStandaloneActorRelocationStagingOwner(
            ZLinkInternalSpotNode targetNode,
            ZLinkActorSessionCoordinator actors,
            ZLinkRelocationAdapterRegistry adapters,
            ZLinkSpotRuntime spots,
            String meshName) {
        backend =
                new ProductionBackend(
                        Objects.requireNonNull(targetNode, "targetNode"),
                        Objects.requireNonNull(actors, "actors"),
                        Objects.requireNonNull(adapters, "adapters"),
                        Objects.requireNonNull(spots, "spots"));
        activationAdmission = spots.activationAdmission(meshName);
    }

    /** A staging owner over a test backend, bounded by {@code activationAdmission}. */
    ZLinkStandaloneActorRelocationStagingOwner(
            Backend backend, ZLinkActivationAdmission activationAdmission) {
        this.backend = Objects.requireNonNull(backend, "backend");
        this.activationAdmission =
                Objects.requireNonNull(activationAdmission, "activationAdmission");
    }

    /** A staging owner over a test backend whose MeshNode limit is never reached. */
    ZLinkStandaloneActorRelocationStagingOwner(Backend backend) {
        this(backend, new ZLinkActivationAdmission(Integer.MAX_VALUE));
    }

    /**
     * Restores the Actor as this MeshNode's relocation target. The Restore holds one activation
     * admission until the Actor is published (target commit) or discarded (MeshNode §5.1).
     */
    CompletionStage<Staged> stage(Request request, byte[] root) {
        Objects.requireNonNull(request, "request");
        var decoded =
                ZLinkCanonicalActorRelocationEnvelope.decode(
                        root, request.relocationId(), request.actorId(), request.restoreSnapshot());
        if (decoded.objectGeneration() != request.objectGeneration()
                || decoded.expectedAuthorityOwnerGeneration()
                        != request.sourceAuthorityOwnerGeneration()) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "Actor relocation root differs from the authority fence"));
        }
        ZLinkActivationAdmission.Permit permit;
        try {
            permit =
                    activationAdmission.acquire(
                            "Actor relocation target '" + request.actorId() + "'");
        } catch (ZLinkFrameworkException full) {
            return CompletableFuture.failedFuture(full);
        }
        return ZLinkHandlerStages.fromStageSupplier(
                        () -> backend.prepare(request, decoded.state(), NOT_CANCELLED))
                .whenComplete(
                        (prepared, failure) -> {
                            if (failure != null) permit.close();
                        })
                .thenCompose(
                        prepared -> {
                            var staged = new Staged(this, request, decoded, prepared, permit);
                            return ZLinkHandlerStages.fromStageSupplier(
                                            () ->
                                                    backend.stageTimers(
                                                            request, decoded.timerEnvelope()))
                                    .thenApply(ignored -> staged)
                                    .exceptionallyCompose(
                                            failure ->
                                                    SpotActivationBase.finishCleanup(
                                                                    failure,
                                                                    ZLinkHandlerStages
                                                                            .fromStageSupplier(
                                                                                    () ->
                                                                                            discard(
                                                                                                    staged)))
                                                            .thenApply(ignored -> null));
                        });
    }

    DurableBacklog closeDurableBacklog(
            Staged staged,
            byte[] finalRoot,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
        requireActive(staged);
        Objects.requireNonNull(replayer, "replayer");
        var decoded =
                ZLinkCanonicalActorRelocationEnvelope.decode(
                        finalRoot,
                        staged.request().relocationId(),
                        staged.request().actorId(),
                        staged.request().restoreSnapshot());
        requireStagingPrefix(staged, decoded);
        return inStateLane(
                staged,
                () -> {
                    if (staged.ingressClosed) {
                        throw new IllegalStateException(
                                "standalone Actor staging ingress was already closed");
                    }
                    staged.ingressClosed = true;
                    staged.replaySealed = true;
                    List<PendingIngress> relayed = List.copyOf(staged.relayedIngress);
                    List<PendingIngress> temporary = List.copyOf(staged.pendingIngress);
                    staged.relayedIngress.clear();
                    staged.pendingIngress.clear();
                    DurableBacklog backlog =
                            new DurableBacklog(
                                    staged, decoded.journal(), relayed, temporary, replayer);
                    staged.durableBacklog = backlog;
                    return backlog;
                });
    }

    void publishHidden(DurableBacklog backlog, long targetOwnerGeneration) {
        Objects.requireNonNull(backlog, "backlog");
        Staged staged = backlog.staged;
        requireActive(staged);
        if (staged.durableBacklog != backlog || !staged.replaySealed || staged.published) {
            throw new IllegalStateException("Actor durable backlog publication fence is invalid");
        }
        if (targetOwnerGeneration < 0) {
            throw new IllegalArgumentException("target owner generation must not be negative");
        }
        if (targetOwnerGeneration == 0) {
            publish(staged);
        } else {
            publish(staged, targetOwnerGeneration);
        }
    }

    CompletionStage<Optional<byte[]>> replayActor(
            Staged staged, ZLinkActorAcceptedJournal.Record record) {
        // The Actor FIFO can execute an accepted record after the temporary stage has ended.
        Objects.requireNonNull(staged, "staged");
        if (staged.owner != this) {
            throw new IllegalStateException(
                    "Actor relocation target attempt belongs to another owner");
        }
        Objects.requireNonNull(record, "record");
        if (!record.actorId().equals(staged.request().actorId())) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("accepted journal references another Actor"));
        }
        return backend.replay(staged.actor(), staged.request(), record);
    }

    boolean acceptIngress(
            Staged staged,
            byte[] acceptedJournalRecord,
            Consumer<List<Message>> reply,
            Consumer<Throwable> failure) {
        Objects.requireNonNull(staged, "staged");
        return inStateLane(
                staged,
                () -> {
                    if (staged.owner != this || staged.terminal) {
                        if (staged.owner == this
                                && staged.terminal
                                && staged.published
                                && staged.lifecycleOpen) {
                            return false;
                        }
                        throw new IllegalStateException(
                                "Actor relocation target attempt fence is not active");
                    }
                    if (staged.ingressClosed && staged.replayed) return false;
                    assert staged.debugProbe.expect(
                                    "temporary:actor:"
                                            + staged.request().actorId()
                                            + ":"
                                            + (staged.pendingIngress.size()
                                                    + (staged.directBacklog != null
                                                            ? staged.directBacklog.temporary.size()
                                                            : staged.durableBacklog != null
                                                                    ? staged.durableBacklog
                                                                            .temporary.size()
                                                                    : 0)),
                                    "standalone Actor staging owner")
                            != null;
                    staged.pendingIngress.add(
                            new PendingIngress(acceptedJournalRecord, reply, failure));
                    return true;
                });
    }

    CompletionStage<Void> stageRelayedRecord(Staged staged, byte[] frozenRecord) {
        requireActive(staged);
        IllegalStateException closed =
                inStateLane(
                        staged,
                        () -> {
                            if (staged.ingressClosed) {
                                return new IllegalStateException(
                                        "standalone Actor staging ingress is closed");
                            }
                            assert staged.debugProbe.expect(
                                            "relayed:actor:"
                                                    + staged.request().actorId()
                                                    + ":"
                                                    + staged.relayedIngress.size(),
                                            "standalone Actor staging owner")
                                    != null;
                            staged.relayedIngress.add(new PendingIngress(frozenRecord, null, null));
                            return null;
                        });
        if (closed != null) {
            return CompletableFuture.failedFuture(closed);
        }
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<CompletionStage<Void>> drainDurableBacklog(DurableBacklog backlog) {
        Objects.requireNonNull(backlog, "backlog");
        Staged staged = backlog.staged;
        requireActive(staged);
        IllegalStateException consumed =
                inStateLane(
                        staged,
                        () -> {
                            if (backlog.consumed) {
                                return new IllegalStateException(
                                        "standalone Actor durable backlog was already consumed");
                            }
                            backlog.consumed = true;
                            return null;
                        });
        if (consumed != null) {
            return CompletableFuture.failedFuture(consumed);
        }
        return replayBacklog(
                        staged,
                        backlog.saved,
                        backlog.relayed,
                        backlog.temporary,
                        backlog.replayer,
                        backlog.debugProbe,
                        0,
                        0)
                .thenApply(
                        terminal -> {
                            inStateLane(
                                    staged,
                                    () -> {
                                        staged.decoded = null;
                                        return null;
                                    });
                            return terminal.whenComplete(
                                    (ignored, failure) -> {
                                        assert backlog.debugProbe.assertDrainedResult();
                                    });
                        })
                .thenApply(
                        terminal -> {
                            backlog.saved = List.of();
                            backlog.relayed = List.of();
                            backlog.temporary = List.of();
                            return terminal;
                        });
    }

    private CompletionStage<CompletionStage<Void>> replayBacklog(
            Staged staged,
            List<ZLinkSerialExecutionQueue.QueuedRecord> saved,
            List<PendingIngress> relayed,
            List<PendingIngress> temporary,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
            AsyncDrainProbe probe,
            int temporaryOffset,
            int pendingOffset) {
        List<ZLinkSerialExecutionQueue.QueuedRecord> records =
                new ArrayList<>(saved.size() + relayed.size() + temporary.size());
        String lane = "actor:" + staged.request().actorId();
        for (var queued : saved) {
            records.add(
                    new ZLinkSerialExecutionQueue.QueuedRecord(
                            records.size() + 1L, queued.payload()));
        }
        for (PendingIngress ingress : relayed) {
            records.add(
                    new ZLinkSerialExecutionQueue.QueuedRecord(
                            records.size() + 1L, ingress.record()));
        }
        for (PendingIngress ingress : temporary) {
            records.add(
                    new ZLinkSerialExecutionQueue.QueuedRecord(
                            records.size() + 1L, ingress.record()));
        }
        List<CompletionStage<Void>> replayObserved = new ArrayList<>(records.size());
        return backend.admitBacklogPrefix(
                        staged.actor(),
                        staged.request(),
                        records,
                        (record, terminal) -> {
                            int index = Math.toIntExact(record.sequence() - 1);
                            if (index < saved.size())
                                return replayer.prepareReplay(
                                        lane, saved.get(index), terminal, replayObserved::add);
                            if (index < saved.size() + relayed.size())
                                return replayer.prepareReplay(
                                        lane,
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1, relayed.get(index - saved.size()).record()),
                                        terminal,
                                        replayObserved::add);
                            replayObserved.add(terminal);
                            var ingress = temporary.get(index - saved.size() - relayed.size());
                            return () -> replayIngress(staged, ingress);
                        })
                .thenCompose(
                        accepted -> {
                            List<CompletableFuture<Void>> observed =
                                    new ArrayList<>(accepted.size());
                            for (int index = 0; index < accepted.size(); index++) {
                                CompletionStage<Void> execution = replayObserved.get(index);
                                if (index >= saved.size() + relayed.size()) {
                                    var ingress =
                                            temporary.get(index - saved.size() - relayed.size());
                                    execution =
                                            execution.whenComplete(
                                                    (ignored, failure) -> {
                                                        if (failure != null
                                                                && ingress.failure() != null)
                                                            ingress.failure()
                                                                    .accept(unwrap(failure));
                                                    });
                                }
                                assert (execution =
                                                probe.completeOn(
                                                        execution,
                                                        index < saved.size()
                                                                ? "journal:"
                                                                        + lane
                                                                        + ":"
                                                                        + saved.get(index)
                                                                                .sequence()
                                                                : index
                                                                                < saved.size()
                                                                                        + relayed
                                                                                                .size()
                                                                        ? "relayed:"
                                                                                + lane
                                                                                + ":"
                                                                                + (index
                                                                                        - saved
                                                                                                .size())
                                                                        : "temporary:"
                                                                                + lane
                                                                                + ":"
                                                                                + (temporaryOffset
                                                                                        + index
                                                                                        - saved
                                                                                                .size()
                                                                                        - relayed
                                                                                                .size())))
                                        != null;
                                observed.add(execution.toCompletableFuture());
                            }
                            // Retain pending payloads until all batches have reached the prepared
                            // FIFO.
                            // Arrivals during an asynchronous admission stay in this same owner
                            // list.
                            List<PendingIngress> late =
                                    inStateLane(
                                            staged,
                                            () -> {
                                                if (staged.pendingIngress.size() == pendingOffset) {
                                                    staged.pendingIngress.clear();
                                                    completeReplay(staged);
                                                    return List.of();
                                                }
                                                return List.copyOf(
                                                        staged.pendingIngress.subList(
                                                                pendingOffset,
                                                                staged.pendingIngress.size()));
                                            });
                            CompletionStage<CompletionStage<Void>> suffix =
                                    late.isEmpty()
                                            ? CompletableFuture.completedFuture(
                                                    CompletableFuture.completedFuture(null))
                                            : replayBacklog(
                                                    staged,
                                                    List.of(),
                                                    List.of(),
                                                    late,
                                                    replayer,
                                                    probe,
                                                    temporaryOffset + temporary.size(),
                                                    pendingOffset + late.size());
                            return suffix.thenApply(
                                    lateTerminal -> {
                                        observed.add(lateTerminal.toCompletableFuture());
                                        var terminal =
                                                CompletableFuture.allOf(
                                                        observed.toArray(CompletableFuture[]::new));
                                        terminal.whenComplete(
                                                (ignored, failure) -> {
                                                    if (terminal.isCancelled()) {
                                                        accepted.forEach(
                                                                entry -> entry.cancel(false));
                                                        lateTerminal
                                                                .toCompletableFuture()
                                                                .cancel(false);
                                                    }
                                                });
                                        return terminal;
                                    });
                        });
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException && failure.getCause() != null
                ? failure.getCause()
                : failure;
    }

    private CompletionStage<Void> replayIngress(Staged staged, PendingIngress ingress) {
        return replayActor(staged, ZLinkActorAcceptedJournal.decode(ingress.record()))
                .handle(
                        (reply, failure) -> {
                            if (failure != null) {
                                throw new CompletionException(unwrap(failure));
                            }
                            if (reply.isPresent() && ingress.reply() != null) {
                                List<Message> parts = List.of(Message.from(reply.orElseThrow()));
                                try {
                                    ingress.reply().accept(parts);
                                } catch (RuntimeException callbackFailure) {
                                    parts.forEach(Message::close);
                                    throw callbackFailure;
                                }
                            }
                            return null;
                        });
    }

    DirectJoinReplay closeDirectJoinIngress(Staged staged, byte[] finalRoot) {
        requireActive(staged);
        var decoded =
                ZLinkCanonicalActorRelocationEnvelope.decode(
                        finalRoot,
                        staged.request().relocationId(),
                        staged.request().actorId(),
                        staged.request().restoreSnapshot());
        requireStagingPrefix(staged, decoded);
        return inStateLane(
                staged,
                () -> {
                    if (staged.ingressClosed) {
                        throw new IllegalStateException(
                                "standalone Actor staging ingress was already closed");
                    }
                    staged.ingressClosed = true;
                    staged.directReplaySealed = true;
                    List<PendingIngress> relayed = List.copyOf(staged.relayedIngress);
                    List<PendingIngress> temporary = List.copyOf(staged.pendingIngress);
                    staged.relayedIngress.clear();
                    staged.pendingIngress.clear();
                    DirectJoinReplay replay =
                            new DirectJoinReplay(staged, decoded.journal(), relayed, temporary);
                    staged.directBacklog = replay;
                    return replay;
                });
    }

    void publishDirectJoinHidden(DirectJoinReplay replay, long targetOwnerGeneration) {
        Objects.requireNonNull(replay, "replay");
        Staged staged = replay.staged;
        requireActive(staged);
        if (!staged.directReplaySealed || staged.directBacklog != replay || staged.published) {
            throw new IllegalStateException(
                    "direct Join durable backlog publication fence is invalid");
        }
        backend.publish(staged.actor(), staged.request(), targetOwnerGeneration);
        staged.markPublished();
    }

    CompletionStage<Void> prepareDirectJoinBoundSession(
            DirectJoinReplay replay,
            ZLinkSpotRetireControl.StageRequest request,
            long targetOwnerGeneration) {
        Objects.requireNonNull(replay, "replay");
        Objects.requireNonNull(request, "request");
        if (request.sessionRoutes().isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        Staged staged = replay.staged();
        requireActive(staged);
        if (!staged.published || staged.directBacklog != replay) {
            throw new IllegalStateException(
                    "direct Join bound Session requires hidden publication");
        }
        ZLinkSpotRetireControl.SessionRouteFence route = request.sessionRoutes().getFirst();
        if (!route.actorId().equals(staged.request().actorId())
                || route.actorObjectGeneration() != staged.request().objectGeneration()) {
            throw new IllegalStateException(
                    "direct Join bound Session differs from the staged Actor");
        }
        return backend.prepareBoundSession(
                staged.actor(),
                staged.request(),
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new ZLinkBackendActorRef(
                                request.targetNodeRid(),
                                route.actorId(),
                                route.actorObjectGeneration()),
                        request.targetNodeGeneration(),
                        targetOwnerGeneration,
                        request.targetOwnerLeaseGeneration()),
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        route.sessionOwnerNodeRid(),
                        route.sessionOwnerNodeGeneration(),
                        route.sessionOwnerId(),
                        route.sessionOwnerLeaseGeneration(),
                        route.sessionRid(),
                        route.bindingGeneration()));
    }

    CompletionStage<CompletionStage<Void>> replayDirectJoin(
            DirectJoinReplay replay, ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
        Objects.requireNonNull(replay, "replay");
        Objects.requireNonNull(replayer, "replayer");
        Staged staged = replay.staged();
        requireActive(staged);
        IllegalStateException alreadyReplayed =
                inStateLane(
                        staged,
                        () -> {
                            if (replay.replayed) {
                                return new IllegalStateException(
                                        "direct Join replay was already consumed");
                            }
                            replay.replayed = true;
                            return null;
                        });
        if (alreadyReplayed != null) {
            return CompletableFuture.failedFuture(alreadyReplayed);
        }
        return replayBacklog(
                        staged,
                        replay.saved(),
                        replay.relayed(),
                        replay.temporary(),
                        replayer,
                        replay.debugProbe,
                        0,
                        0)
                .thenApply(
                        terminal -> {
                            inStateLane(
                                    staged,
                                    () -> {
                                        staged.decoded = null;
                                        return null;
                                    });
                            return terminal.whenComplete(
                                    (ignored, failure) -> {
                                        assert replay.debugProbe.assertDrainedResult();
                                    });
                        })
                .thenApply(
                        terminal -> {
                            replay.saved = List.of();
                            replay.relayed = List.of();
                            replay.temporary = List.of();
                            return terminal;
                        });
    }

    private static void requireStagingPrefix(
            Staged staged, ZLinkCanonicalActorRelocationEnvelope.Decoded decoded) {
        var initial = staged.decoded();
        if (decoded.objectGeneration() != initial.objectGeneration()
                || decoded.expectedAuthorityOwnerGeneration()
                        != initial.expectedAuthorityOwnerGeneration()
                || !Arrays.equals(decoded.state(), initial.state())) {
            throw new IllegalArgumentException(
                    "authority-selected Actor root differs from staged state");
        }
        if (decoded.journal().size() < initial.journal().size()) {
            throw new IllegalArgumentException(
                    "authority-selected Actor journal removed staged records");
        }
        for (int index = 0; index < initial.journal().size(); index++) {
            var left = initial.journal().get(index);
            var right = decoded.journal().get(index);
            if (left.sequence() != right.sequence()
                    || !Arrays.equals(left.payload(), right.payload())) {
                throw new IllegalArgumentException(
                        "authority-selected Actor journal changed its prefix");
            }
        }
    }

    private void publish(Staged staged) {
        requireActive(staged);
        backend.publish(staged.actor(), staged.request());
        staged.markPublished();
    }

    private void publish(Staged staged, long targetOwnerGeneration) {
        requireActive(staged);
        backend.publish(staged.actor(), staged.request(), targetOwnerGeneration);
        staged.markPublished();
    }

    void openAdmission(Staged staged) {
        requireActive(staged);
        if (!staged.published) {
            throw new IllegalStateException("Actor relocation is not published");
        }
        if (staged.lifecycleOpen) {
            throw new IllegalStateException("Actor relocation admission is already open");
        }
        backend.publishTimers(staged.request());
        backend.openAdmission(staged.actor());
        inStateLane(
                staged,
                () -> {
                    staged.lifecycleOpen = true;
                    settleTerminal(staged);
                    return null;
                });
    }

    private static void completeReplay(Staged staged) {
        staged.replayed = true;
        settleTerminal(staged);
    }

    private static void settleTerminal(Staged staged) {
        staged.terminal = staged.lifecycleOpen && staged.replayed;
        if (staged.terminal) {
            staged.durableBacklog = null;
            staged.directBacklog = null;
        }
    }

    CompletionStage<Void> discard(Staged staged) {
        Objects.requireNonNull(staged, "staged");
        if (staged.owner != this) {
            throw new IllegalStateException(
                    "Actor relocation target attempt belongs to another owner");
        }
        List<String> debugDiscardNames = new ArrayList<>();
        List<PendingIngress> pending =
                inStateLane(
                        staged,
                        () -> {
                            if (staged.published) {
                                throw new IllegalStateException(
                                        "published Actor relocation cannot be discarded");
                            }
                            if (staged.terminal) {
                                return null;
                            }
                            assert captureDiscardedIngress(staged, debugDiscardNames);
                            staged.ingressClosed = true;
                            staged.terminal = true;
                            List<PendingIngress> captured =
                                    new ArrayList<>(
                                            staged.relayedIngress.size()
                                                    + staged.pendingIngress.size());
                            captured.addAll(staged.relayedIngress);
                            captured.addAll(staged.pendingIngress);
                            if (staged.durableBacklog != null) {
                                captured.addAll(staged.durableBacklog.relayed);
                                captured.addAll(staged.durableBacklog.temporary);
                                staged.durableBacklog = null;
                            }
                            if (staged.directBacklog != null) {
                                captured.addAll(staged.directBacklog.relayed);
                                captured.addAll(staged.directBacklog.temporary);
                                staged.directBacklog = null;
                            }
                            staged.relayedIngress.clear();
                            staged.pendingIngress.clear();
                            return captured;
                        });
        if (pending == null) {
            return CompletableFuture.completedFuture(null);
        }
        IllegalStateException aborted =
                new IllegalStateException("Actor relocation target staging was aborted");
        for (PendingIngress ingress : pending) {
            if (ingress.failure() != null) {
                ingress.failure().accept(aborted);
            }
        }
        return ZLinkHandlerStages.fromStageSupplier(
                        () -> backend.discard(staged.actor(), staged.request()))
                .whenComplete(
                        (ignored, failure) -> {
                            staged.restorePermit.close();
                            if (failure == null) {
                                for (String name : debugDiscardNames) {
                                    assert staged.debugProbe.complete(name);
                                }
                                assert staged.debugProbe.assertDrainedResult();
                            }
                        });
    }

    private static boolean captureDiscardedIngress(Staged staged, List<String> names) {
        String actor = staged.request().actorId();
        int relayed = 0;
        int temporary = 0;
        List<PendingIngress> relayedItems = staged.relayedIngress;
        List<PendingIngress> temporaryItems = staged.pendingIngress;
        if (staged.durableBacklog != null) {
            relayedItems = staged.durableBacklog.relayed;
            temporaryItems = staged.durableBacklog.temporary;
            for (var record : staged.durableBacklog.saved) {
                names.add("journal:actor:" + actor + ":" + record.sequence());
            }
        }
        if (staged.directBacklog != null) {
            relayedItems = staged.directBacklog.relayed;
            temporaryItems = staged.directBacklog.temporary;
            for (var record : staged.directBacklog.saved) {
                names.add("journal:actor:" + actor + ":" + record.sequence());
            }
        }
        for (PendingIngress ignored : relayedItems) {
            names.add("relayed:actor:" + actor + ":" + relayed++);
        }
        for (PendingIngress ignored : temporaryItems) {
            names.add("temporary:actor:" + actor + ":" + temporary++);
        }
        return true;
    }

    private void requireActive(Staged staged) {
        Objects.requireNonNull(staged, "staged");
        if (staged.owner != this || staged.terminal) {
            throw new IllegalStateException("Actor relocation target attempt fence is not active");
        }
    }

    private static <T> T inStateLane(Staged staged, Supplier<T> work) {
        try {
            return staged.stateLane.runAsync(work).toCompletableFuture().join();
        } catch (java.util.concurrent.CompletionException failure) {
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

    record Request(
            UUID relocationId,
            String actorId,
            String stableType,
            long objectGeneration,
            long sourceAuthorityOwnerGeneration,
            boolean restoreSnapshot,
            String targetSpotId) {
        Request {
            Objects.requireNonNull(relocationId, "relocationId");
            requireText(actorId, "actorId");
            requireText(stableType, "stableType");
            requireText(targetSpotId, "targetSpotId");
            if (relocationId.equals(new UUID(0, 0))
                    || objectGeneration <= 0
                    || sourceAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                        "Actor relocation identity and generations must be positive");
            }
        }
    }

    static final class Staged {
        private final ZLinkStandaloneActorRelocationStagingOwner owner;
        private final Request request;
        private ZLinkCanonicalActorRelocationEnvelope.Decoded decoded;
        private final Object actor;
        private final ZLinkActivationAdmission.Permit restorePermit;
        private List<PendingIngress> relayedIngress = new ArrayList<>();
        private final List<PendingIngress> pendingIngress = new ArrayList<>();
        private AsyncDrainProbe debugProbe;
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private DurableBacklog durableBacklog;
        private DirectJoinReplay directBacklog;
        private boolean replayed;
        private boolean published;
        private boolean ingressClosed;
        private boolean replaySealed;
        private boolean directReplaySealed;
        private boolean lifecycleOpen;
        private boolean terminal;

        private Staged(
                ZLinkStandaloneActorRelocationStagingOwner owner,
                Request request,
                ZLinkCanonicalActorRelocationEnvelope.Decoded decoded,
                Object actor,
                ZLinkActivationAdmission.Permit restorePermit) {
            this.owner = owner;
            this.request = request;
            this.decoded = decoded;
            this.actor = actor;
            this.restorePermit = restorePermit;
            assert (debugProbe = new AsyncDrainProbe()) != null;
        }

        /** The target commit ends the Restore and returns its activation admission. */
        private void markPublished() {
            published = true;
            restorePermit.close();
        }

        Request request() {
            return request;
        }

        ZLinkCanonicalActorRelocationEnvelope.Decoded decoded() {
            return decoded;
        }

        Object actor() {
            return actor;
        }
    }

    static final class DurableBacklog {
        private final Staged staged;
        private List<ZLinkSerialExecutionQueue.QueuedRecord> saved;
        private List<PendingIngress> relayed;
        private List<PendingIngress> temporary;
        private final ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer;
        private AsyncDrainProbe debugProbe;
        private boolean consumed;

        private DurableBacklog(
                Staged staged,
                List<ZLinkSerialExecutionQueue.QueuedRecord> saved,
                List<PendingIngress> relayed,
                List<PendingIngress> temporary,
                ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
            this.staged = staged;
            this.saved = List.copyOf(saved);
            this.relayed = List.copyOf(relayed);
            this.temporary = List.copyOf(temporary);
            this.replayer = replayer;
            assert (debugProbe = staged.debugProbe) != null;
            assert registerObligations(staged, this.saved, debugProbe);
        }
    }

    static final class DirectJoinReplay {
        private final Staged staged;
        private List<ZLinkSerialExecutionQueue.QueuedRecord> saved;
        private List<PendingIngress> relayed;
        private List<PendingIngress> temporary;
        private AsyncDrainProbe debugProbe;
        private boolean replayed;

        private DirectJoinReplay(
                Staged staged,
                List<ZLinkSerialExecutionQueue.QueuedRecord> saved,
                List<PendingIngress> relayed,
                List<PendingIngress> temporary) {
            this.staged = staged;
            this.saved = List.copyOf(saved);
            this.relayed = List.copyOf(relayed);
            this.temporary = List.copyOf(temporary);
            assert (debugProbe = staged.debugProbe) != null;
            assert registerObligations(staged, this.saved, debugProbe);
        }

        private Staged staged() {
            return staged;
        }

        private List<ZLinkSerialExecutionQueue.QueuedRecord> saved() {
            return saved;
        }

        private List<PendingIngress> relayed() {
            return relayed;
        }

        private List<PendingIngress> temporary() {
            return temporary;
        }
    }

    private static boolean registerObligations(
            Staged staged,
            List<ZLinkSerialExecutionQueue.QueuedRecord> saved,
            AsyncDrainProbe probe) {
        for (var record : saved) {
            probe.expect(
                    "journal:actor:" + staged.request().actorId() + ":" + record.sequence(),
                    "standalone Actor staging owner");
        }
        return true;
    }

    private record PendingIngress(
            byte[] record, Consumer<List<Message>> reply, Consumer<Throwable> failure) {
        private PendingIngress {
            record = Objects.requireNonNull(record, "record").clone();
        }

        @Override
        public byte[] record() {
            return record.clone();
        }
    }

    interface Backend {
        CompletionStage<Object> prepare(
                Request request, byte[] state, ZLinkRelocationCancellation cancellation);

        CompletionStage<Optional<byte[]>> replay(
                Object actor, Request request, ZLinkActorAcceptedJournal.Record record);

        CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                Object actor,
                Request request,
                List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                java.util.function.BiFunction<
                                ZLinkSerialExecutionQueue.QueuedRecord,
                                CompletableFuture<Void>,
                                Supplier<CompletionStage<Void>>>
                        replay);

        default CompletionStage<Void> stageTimers(Request request, byte[] timerEnvelope) {
            if (ZLinkSpotTimerRelocationEnvelope.canonicalize(timerEnvelope).isEmpty()) {
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(
                    new IllegalStateException("standalone Actor timer staging is unavailable"));
        }

        default void publish(Object actor, Request request) {
            throw new IllegalStateException("Actor publication requires a target owner generation");
        }

        default void publish(Object actor, Request request, long targetOwnerGeneration) {
            if (targetOwnerGeneration <= 0) {
                throw new IllegalArgumentException("target owner generation must be positive");
            }
            publish(actor, request);
        }

        default CompletionStage<Void> prepareBoundSession(
                Object actor,
                Request request,
                ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
                ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
            return CompletableFuture.completedFuture(null);
        }

        void openAdmission(Object actor);

        default void publishTimers(Request request) {}

        CompletionStage<Void> discard(Object actor, Request request);
    }

    private static final class ProductionBackend implements Backend {
        private final ZLinkInternalSpotNode targetNode;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkRelocationAdapterRegistry adapters;
        private final ZLinkSpotRuntime spots;

        private ProductionBackend(
                ZLinkInternalSpotNode targetNode,
                ZLinkActorSessionCoordinator actors,
                ZLinkRelocationAdapterRegistry adapters,
                ZLinkSpotRuntime spots) {
            this.targetNode = targetNode;
            this.actors = actors;
            this.adapters = adapters;
            this.spots = spots;
        }

        @Override
        public CompletionStage<Object> prepare(
                Request request, byte[] state, ZLinkRelocationCancellation cancellation) {
            return actors.prepareRelocatedActor(
                            request.actorId(),
                            request.stableType(),
                            state,
                            request.restoreSnapshot(),
                            adapters,
                            cancellation,
                            new ZLinkBackendActorRef(
                                    targetNode.routingId(),
                                    request.actorId(),
                                    request.objectGeneration()))
                    .thenApply(value -> value);
        }

        @Override
        public CompletionStage<Optional<byte[]>> replay(
                Object actor, Request request, ZLinkActorAcceptedJournal.Record record) {
            return spots.replayPreparedActorAtSpot(
                    request.targetSpotId(),
                    (ZLinkActorRuntime.PreparedTransferredActor) actor,
                    record);
        }

        @Override
        public CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                Object actor,
                Request request,
                List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                java.util.function.BiFunction<
                                ZLinkSerialExecutionQueue.QueuedRecord,
                                CompletableFuture<Void>,
                                java.util.function.Supplier<CompletionStage<Void>>>
                        replay) {
            return spots.admitActorRelocationPrefix(
                    actors,
                    (ZLinkActorRuntime.PreparedTransferredActor) actor,
                    request.targetSpotId(),
                    records,
                    replay);
        }

        @Override
        public CompletionStage<Void> stageTimers(Request request, byte[] timerEnvelope) {
            spots.stageEntryActorTimerRelocationEnvelope(
                    request.targetSpotId(), request.actorId(), timerEnvelope);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void publish(Object actor, Request request) {
            throw new IllegalStateException(
                    "standalone Actor publication requires the committed owner generation");
        }

        @Override
        public void publish(Object actor, Request request, long targetOwnerGeneration) {
            if (targetOwnerGeneration <= 0) {
                throw new IllegalArgumentException("target owner generation must be positive");
            }
            actors.publishRelocatedActor(
                    (ZLinkActorRuntime.PreparedTransferredActor) actor,
                    request.targetSpotId(),
                    targetOwnerGeneration);
        }

        @Override
        public CompletionStage<Void> prepareBoundSession(
                Object actor,
                Request request,
                ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
                ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
            // The node's remote binding owner binds the Actor context in the same transition.
            return targetNode.installRelocatingActorBoundSession(actorRoute, session);
        }

        @Override
        public void openAdmission(Object actor) {
            actors.openRelocatedActorAdmission((ZLinkActorRuntime.PreparedTransferredActor) actor);
        }

        @Override
        public void publishTimers(Request request) {
            spots.publishEntryActorTimerRelocation(request.targetSpotId(), request.actorId());
        }

        @Override
        public CompletionStage<Void> discard(Object actor, Request request) {
            var prepared = (ZLinkActorRuntime.PreparedTransferredActor) actor;
            String actorId = prepared.actor().context().actorId();
            return actors.discardRelocatedActor(prepared)
                    .thenRun(
                            () ->
                                    spots.discardEntryActorTimerRelocation(
                                            request.targetSpotId(), actorId));
        }
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " is invalid");
        }
        return value;
    }
}
