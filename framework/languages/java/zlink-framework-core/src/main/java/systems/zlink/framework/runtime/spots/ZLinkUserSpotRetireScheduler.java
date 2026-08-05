package systems.zlink.framework.runtime.spots;

import java.util.List;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;

/**
 * Owns the committed User Spot relocation completion order. Authority commit
 * is the rollback boundary; target admission remains closed through journal
 * replay, source cleanup, Completed publication and every binding-route ACK.
 */
final class ZLinkUserSpotRetireScheduler {
    private final Backend backend;

    ZLinkUserSpotRetireScheduler(
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkUserSpotAggregateStagingOwner target) {
        backend = new ProductionBackend(
            Objects.requireNonNull(coordinator, "coordinator"),
            Objects.requireNonNull(target, "target"));
    }

    ZLinkUserSpotRetireScheduler(Backend backend) {
        this.backend = Objects.requireNonNull(backend, "backend");
    }

    CompletionStage<Result> execute(
        Request request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        return backend.commit(request.prepared(), cancellation)
            .handle((published, failure) -> new CommitAttempt(
                published,
                failure == null ? null : unwrap(failure)))
            .thenCompose(attempt -> attempt.failure() == null
                ? finishCommitted(request, attempt.published(), cancellation)
                : abortPrecommit(request, attempt.failure()));
    }

    CompletionStage<Result> executeRemote(
        RemoteRequest request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        var sourceCommitted = new AtomicBoolean();
        ZLinkSpotRetireControl.StageRequest stage =
            request.source().stageRequest();
        CompletionStage<Result> operation = request.client().stage(
                stage.targetNodeRid(),
                stage,
                request.timeout())
            .thenCompose(ignored -> request.source()
                .freezeAndPrepareFinal(cancellation))
            .thenCompose(prepared -> request.source().requireReplaySupport()
                .thenApply(ignored -> prepared))
            .thenCompose(prepared -> request.source()
                .commitAuthority(cancellation))
            .thenCompose(activated -> {
                sourceCommitted.set(true);
                request.source().commitSourceBarrier(
                    activated.targetOwnerGenerations());
                return CompletableFuture.completedFuture(activated);
            })
            .thenCompose(activated -> request.client().publish(
                    stage.targetNodeRid(),
                    stage.fence(),
                    request.timeout())
                .thenCompose(ignored -> request.sourceCleanup().cleanup())
                .thenCompose(ignored -> request.source().completeSourceCleanup(
                    activated,
                    request.completedRoot(),
                    cancellation))
                .thenCompose(completed -> request.client()
                    .finalizeAfterCompletion(
                        stage.targetNodeRid(),
                        stage.fence(),
                        request.timeout())
                    .thenCompose(ignored -> request.source()
                        .discardInitialAfterCommit())
                    .thenApply(ignored -> {
                        request.source().releasePermitAfterCompletion();
                        return new Result(activated, completed);
                    })));
        return operation.exceptionallyCompose(failure -> {
            Throwable original = unwrap(failure);
            if (sourceCommitted.get()) {
                return CompletableFuture.failedFuture(original);
            }
            return request.client().abort(
                    stage.targetNodeRid(),
                    stage.fence(),
                    request.timeout())
                .handle((ignored, abortFailure) -> abortFailure)
                .thenCompose(abortFailure -> request.source()
                    .abortPrecommit()
                    .handle((ignored, sourceFailure) -> {
                        if (abortFailure != null) {
                            original.addSuppressed(unwrap(abortFailure));
                        }
                        if (sourceFailure != null) {
                            original.addSuppressed(unwrap(sourceFailure));
                        }
                        throw new CompletionException(original);
                    }));
        });
    }

    private CompletionStage<Result> finishCommitted(
        Request request,
        ZLinkAggregateRelocationCoordinator.Published published,
        ZLinkStoreCancellation cancellation) {
        return backend.publishAndReplay(request.staged(), request.replayer())
            .thenCompose(ignored -> request.sourceCleanup().cleanup())
            .thenCompose(ignored -> backend.completeSourceCleanup(
                published,
                request.completedRoot(),
                cancellation))
            .thenCompose(completed -> request.completionFinalizer()
                .finalizeAfterCompletion()
                .thenApply(ignored -> new Result(published, completed)));
    }

    private static CompletionStage<Void> switchBindingRoutes(
        List<BindingRouteSwitch> routes) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (BindingRouteSwitch route : routes) {
            chain = chain.thenCompose(ignored -> route.switchAndAwaitAck());
        }
        return chain;
    }

    private CompletionStage<Result> abortPrecommit(
        Request request,
        Throwable original) {
        return backend.abort(request.prepared())
            .thenCompose(ignored -> backend.discard(request.staged()))
            .thenCompose(ignored -> request.sourceSeal().resume())
            .handle((ignored, cleanupFailure) -> {
                if (cleanupFailure != null) {
                    original.addSuppressed(unwrap(cleanupFailure));
                }
                throw new CompletionException(original);
            });
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    record Request(
        ZLinkAggregateRelocationCoordinator.Prepared prepared,
        ZLinkUserSpotAggregateStagingOwner.Staged staged,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
        SourceCleanup sourceCleanup,
        byte[] completedRoot,
        List<BindingRouteSwitch> bindingRoutes,
        SteadyNormalizer normalizer,
        Admission admission,
        SourceSeal sourceSeal,
        CompletionFinalizer completionFinalizer) {
        Request(
            ZLinkAggregateRelocationCoordinator.Prepared prepared,
            ZLinkUserSpotAggregateStagingOwner.Staged staged,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer,
            SourceCleanup sourceCleanup,
            byte[] completedRoot,
            List<BindingRouteSwitch> bindingRoutes,
            SteadyNormalizer normalizer,
            Admission admission,
            SourceSeal sourceSeal) {
            this(
                prepared,
                staged,
                replayer,
                sourceCleanup,
                completedRoot,
                bindingRoutes,
                normalizer,
                admission,
                sourceSeal,
                () -> switchBindingRoutes(bindingRoutes)
                    .thenCompose(ignored -> normalizer.normalize())
                    .thenRun(admission::open));
        }

        Request {
            Objects.requireNonNull(prepared, "prepared");
            Objects.requireNonNull(staged, "staged");
            Objects.requireNonNull(replayer, "replayer");
            Objects.requireNonNull(sourceCleanup, "sourceCleanup");
            completedRoot = Objects.requireNonNull(
                completedRoot,
                "completedRoot").clone();
            bindingRoutes = List.copyOf(Objects.requireNonNull(
                bindingRoutes,
                "bindingRoutes"));
            Objects.requireNonNull(normalizer, "normalizer");
            Objects.requireNonNull(admission, "admission");
            Objects.requireNonNull(sourceSeal, "sourceSeal");
            Objects.requireNonNull(
                completionFinalizer, "completionFinalizer");
        }

        @Override public byte[] completedRoot() {
            return completedRoot.clone();
        }
    }

    record Result(
        ZLinkAggregateRelocationCoordinator.Published activated,
        ZLinkAggregateRelocationCoordinator.Published completed) {
    }

    record RemoteRequest(
        ZLinkUserSpotRetireSourceBuilder.PreparedSource source,
        ZLinkRelocationTransitionClient client,
        Duration timeout,
        SourceCleanup sourceCleanup,
        byte[] completedRoot) {
        RemoteRequest {
            Objects.requireNonNull(source, "source");
            Objects.requireNonNull(client, "client");
            Objects.requireNonNull(timeout, "timeout");
            Objects.requireNonNull(sourceCleanup, "sourceCleanup");
            completedRoot = Objects.requireNonNull(
                completedRoot, "completedRoot").clone();
            if (timeout.isZero() || timeout.isNegative()) {
                throw new IllegalArgumentException(
                    "remote Retire timeout must be positive");
            }
        }

        @Override public byte[] completedRoot() {
            return completedRoot.clone();
        }
    }

    @FunctionalInterface
    interface SourceCleanup {
        CompletionStage<Void> cleanup();
    }

    @FunctionalInterface
    interface BindingRouteSwitch {
        CompletionStage<Void> switchAndAwaitAck();
    }

    @FunctionalInterface
    interface SteadyNormalizer {
        CompletionStage<Void> normalize();
    }

    @FunctionalInterface
    interface SourceSeal {
        CompletionStage<Void> resume();
    }

    @FunctionalInterface
    interface Admission {
        void open();
    }

    @FunctionalInterface
    interface CompletionFinalizer {
        CompletionStage<Void> finalizeAfterCompletion();
    }

    static CompletionFinalizer remoteFinalizer(
        ZLinkRelocationTransitionClient client,
        ZLinkSpotRetireControl.StageRequest request,
        Duration timeout) {
        Objects.requireNonNull(client, "client");
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(timeout, "timeout");
        return () -> client.finalizeAfterCompletion(
            request.targetNodeRid(),
            request.fence(),
            timeout);
    }

    interface Backend {
        CompletionStage<ZLinkAggregateRelocationCoordinator.Published> commit(
            ZLinkAggregateRelocationCoordinator.Prepared prepared,
            ZLinkStoreCancellation cancellation);

        CompletionStage<Void> publishAndReplay(
            ZLinkUserSpotAggregateStagingOwner.Staged staged,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer);

        CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
            completeSourceCleanup(
                ZLinkAggregateRelocationCoordinator.Published published,
                byte[] completedRoot,
                ZLinkStoreCancellation cancellation);

        CompletionStage<Void> abort(
            ZLinkAggregateRelocationCoordinator.Prepared prepared);

        CompletionStage<Void> discard(
            ZLinkUserSpotAggregateStagingOwner.Staged staged);
    }

    private record ProductionBackend(
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkUserSpotAggregateStagingOwner target) implements Backend {
        @Override
        public CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
            commit(
                ZLinkAggregateRelocationCoordinator.Prepared prepared,
                ZLinkStoreCancellation cancellation) {
            return coordinator.commit(prepared, cancellation);
        }

        @Override
        public CompletionStage<Void> publishAndReplay(
            ZLinkUserSpotAggregateStagingOwner.Staged staged,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
            return target.publishAndReplayHidden(staged, replayer);
        }

        @Override
        public CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
            completeSourceCleanup(
                ZLinkAggregateRelocationCoordinator.Published published,
                byte[] completedRoot,
                ZLinkStoreCancellation cancellation) {
            return coordinator.completeSourceCleanup(
                published,
                completedRoot,
                cancellation);
        }

        @Override
        public CompletionStage<Void> abort(
            ZLinkAggregateRelocationCoordinator.Prepared prepared) {
            return coordinator.abort(prepared);
        }

        @Override
        public CompletionStage<Void> discard(
            ZLinkUserSpotAggregateStagingOwner.Staged staged) {
            return target.discard(staged);
        }
    }

    private record CommitAttempt(
        ZLinkAggregateRelocationCoordinator.Published published,
        Throwable failure) {
    }
}
