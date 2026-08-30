package systems.zlink.framework.runtime.spots;
import java.util.concurrent.CopyOnWriteArrayList;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkSpotRelocationCoordinationMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkDefaultSpotContextTest {
    @Test
    void lifecycleYieldIsLimitedToSharedSpotExecutions() {
        assertTrue(lifecycleYieldAllowed(
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            false));
        assertFalse(lifecycleYieldAllowed(
            ZLinkUserSpotExecutionMode.PER_ACTOR,
            false));
        assertTrue(lifecycleYieldAllowed(
            ZLinkUserSpotExecutionMode.PER_ACTOR,
            true));
    }

    @Test
    void lifecycleRelocationReadyFollowsSpotFactoryPolicy() {
        assertTrue(DefaultSpotContext.relocationReadyAllowed(
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED,
            false));
        assertFalse(DefaultSpotContext.relocationReadyAllowed(
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            ZLinkSpotRelocationCoordinationMode.FRAMEWORK_MANAGED,
            false));
        assertFalse(DefaultSpotContext.relocationReadyAllowed(
            ZLinkUserSpotExecutionMode.PER_ACTOR,
            ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED,
            false));
        assertFalse(DefaultSpotContext.relocationReadyAllowed(
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED,
            true));
    }

    @Test
    void perActorApplicationPayloadKeepsSameActorFifo() {
        TestHost host = new TestHost();
        DefaultSpotContext context = host.userContext(
            ZLinkUserSpotExecutionMode.PER_ACTOR);
        CompletableFuture<Void> firstRelease = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first = context.enqueueActorDispatch(
            "actor-a",
            17,
            () -> {
                var execution = ZLinkSuspendInvocationContext
                    .currentApplicationExecution();
                assertEquals("actor-a", execution.actorId());
                assertFalse(execution.sharedSpotGate());
                assertFalse(execution.yieldAllowed());
                firstStarted.complete(null);
                return firstRelease;
            });
        firstStarted.join();
        CompletionStage<Void> second = context.enqueueActorDispatch(
            "actor-a",
            19,
            () -> {
                secondStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

        assertFalse(secondStarted.isDone());
        firstRelease.complete(null);
        CompletableFuture.allOf(
            first.toCompletableFuture(),
            second.toCompletableFuture()).join();
        assertEquals(2, host.actorDispatchSubmissions.get());
    }

    @Test
    void spotWideApplicationPayloadUsesActorQueueThenSharedGate() {
        TestHost host = new TestHost();
        DefaultSpotContext context = host.userContext(
            ZLinkUserSpotExecutionMode.SPOT_WIDE);
        CompletableFuture<Void> firstRelease = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first = context.enqueueActorDispatch(
                "actor-a",
                23,
                () -> {
                assertEquals(
                    "actor-a",
                    ZLinkSuspendInvocationContext.currentActorDispatch());
                var execution = ZLinkSuspendInvocationContext
                    .currentApplicationExecution();
                assertTrue(execution.sharedSpotGate());
                assertTrue(execution.yieldAllowed());
                firstStarted.complete(null);
                return firstRelease;
            });
        firstStarted.join();
        CompletionStage<Void> second = context.enqueueActorDispatch(
            "actor-b",
            29,
            () -> {
                secondStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

        assertFalse(secondStarted.isDone());
        firstRelease.complete(null);
        CompletableFuture.allOf(
            first.toCompletableFuture(),
            second.toCompletableFuture()).join();
        assertEquals(2, host.actorDispatchSubmissions.get());
    }

    @Test
    void spotWideActorPayloadReusesSharedGateOwnedByCurrentSpotTurn() {
        TestHost host = new TestHost();
        DefaultSpotContext context = host.userContext(
            ZLinkUserSpotExecutionMode.SPOT_WIDE);
        CompletableFuture<Void> actorStarted = new CompletableFuture<>();

        CompletionStage<Void> dispatch = context.enqueueDispatch(() ->
            context.enqueueActorDispatch(
                "actor-a",
                31,
                () -> {
                    var execution = ZLinkSuspendInvocationContext
                        .currentApplicationExecution();
                    assertTrue(execution.sharedSpotGate());
                    assertTrue(execution.yieldAllowed());
                    actorStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                }));

        dispatch.toCompletableFuture().join();
        assertTrue(actorStarted.isDone());
    }

    @Test
    void spotWideDifferentActorCallbacksDoNotOverlap() throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> firstRelease = new CompletableFuture<>();
            CompletableFuture<Void> firstStarted = new CompletableFuture<>();
            CompletableFuture<Void> secondStarted = new CompletableFuture<>();
            AtomicInteger activeCallbacks = new AtomicInteger();
            AtomicInteger maximumActiveCallbacks = new AtomicInteger();

            CompletionStage<Void> first = context.enqueueActorDispatch(
                "actor-a",
                () -> {
                    int active = activeCallbacks.incrementAndGet();
                    maximumActiveCallbacks.accumulateAndGet(
                        active, Math::max);
                    firstStarted.complete(null);
                    return firstRelease.whenComplete((ignored, error) ->
                        activeCallbacks.decrementAndGet());
                });
            firstStarted.get(2, TimeUnit.SECONDS);

            CompletionStage<Void> second = context.enqueueActorDispatch(
                "actor-b",
                () -> {
                    int active = activeCallbacks.incrementAndGet();
                    maximumActiveCallbacks.accumulateAndGet(
                        active, Math::max);
                    secondStarted.complete(null);
                    activeCallbacks.decrementAndGet();
                    return CompletableFuture.completedFuture(null);
                });

            assertEquals(2, host.actorDispatchSubmissions.get());
            assertFalse(secondStarted.isDone());
            firstRelease.complete(null);
            CompletableFuture.allOf(
                first.toCompletableFuture(),
                second.toCompletableFuture()).join();
            assertEquals(1, maximumActiveCallbacks.get());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void spotWideLifecycleWaitsForTheSharedSpotGate() throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> applicationRelease =
                new CompletableFuture<>();
            CompletableFuture<Void> applicationStarted =
                new CompletableFuture<>();
            CompletableFuture<Void> lifecycleStarted =
                new CompletableFuture<>();

            CompletionStage<Void> application = context.enqueueDispatch(() -> {
                applicationStarted.complete(null);
                return applicationRelease;
            });
            applicationStarted.get(2, TimeUnit.SECONDS);

            CompletionStage<Void> lifecycle = context.enqueueLifecycle(() -> {
                lifecycleStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

            assertFalse(lifecycleStarted.isDone());
            applicationRelease.complete(null);
            CompletableFuture.allOf(
                application.toCompletableFuture(),
                lifecycle.toCompletableFuture()).join();
            assertTrue(lifecycleStarted.isDone());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void spotWideLifecycleCanBeEnqueuedFromTheCurrentDispatchTurn()
        throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> lifecycleStarted =
                new CompletableFuture<>();

            CompletionStage<Void> dispatch = context.enqueueDispatch(() ->
                context.enqueueLifecycle(() -> {
                    lifecycleStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                }));

            dispatch.toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTrue(lifecycleStarted.isDone());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void spotWideLifecycleYieldsWhenOnlyTheRestoredSerialTurnIsPresent()
        throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> dispatchRelease = new CompletableFuture<>();
            CompletableFuture<Void> lifecycleStarted = new CompletableFuture<>();

            CompletionStage<Void> dispatch = context.enqueueDispatch(() -> {
                Object serialTurn = ZLinkSuspendInvocationContext
                    .currentSerialExecutionTurn();
                var application = ZLinkSuspendInvocationContext
                    .currentApplicationExecution();
                executor.execute(() -> {
                    try (var serial = ZLinkSuspendInvocationContext
                             .enterSerialExecutionTurn(serialTurn);
                         var execution = ZLinkSuspendInvocationContext
                             .enterApplicationExecution(application)) {
                        CompletionStage<Void> lifecycle = context.enqueueLifecycle(
                            () -> {
                                lifecycleStarted.complete(null);
                                return CompletableFuture.completedFuture(null);
                            });
                        lifecycle.whenComplete((ignored, error) -> {
                            if (error == null) {
                                dispatchRelease.complete(null);
                            } else {
                                dispatchRelease.completeExceptionally(error);
                            }
                        });
                    } catch (Throwable failure) {
                        dispatchRelease.completeExceptionally(failure);
                    }
                });
                return dispatchRelease;
            });

            dispatch.toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTrue(lifecycleStarted.isDone());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void lifecycleLaneRunsBeforeAQueuedApplicationTurn() throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> firstRelease = new CompletableFuture<>();
            CompletableFuture<Void> firstStarted = new CompletableFuture<>();
            List<String> order = new CopyOnWriteArrayList<>();

            CompletionStage<Void> first = context.enqueueDispatch(() -> {
                order.add("first");
                firstStarted.complete(null);
                return firstRelease;
            });
            firstStarted.get(2, TimeUnit.SECONDS);
            CompletionStage<Void> lifecycle = context.enqueueLifecycle(() -> {
                order.add("lifecycle");
                return CompletableFuture.completedFuture(null);
            });
            CompletionStage<Void> second = context.enqueueDispatch(() -> {
                order.add("second");
                return CompletableFuture.completedFuture(null);
            });

            firstRelease.complete(null);
            CompletableFuture.allOf(
                first.toCompletableFuture(),
                lifecycle.toCompletableFuture(),
                second.toCompletableFuture()).join();
            assertEquals(List.of("first", "lifecycle", "second"), order);
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void infrastructureControlProgressesWhileApplicationLaneIsDeferred()
        throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE,
                ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED);
            CompletableFuture<Void> applicationRelease =
                new CompletableFuture<>();
            CompletableFuture<Void> applicationStarted =
                new CompletableFuture<>();
            CompletableFuture<Void> infrastructureStarted =
                new CompletableFuture<>();

            CompletionStage<Void> application = context.enqueueDispatch(() -> {
                context.relocationReady().defer();
                applicationStarted.complete(null);
                return applicationRelease;
            });
            applicationStarted.get(2, TimeUnit.SECONDS);

            CompletionStage<Void> infrastructure =
                context.enqueueInfrastructureDispatch(() -> {
                    infrastructureStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                });

            infrastructureStarted.get(2, TimeUnit.SECONDS);
            assertFalse(application.toCompletableFuture().isDone());
            infrastructure.toCompletableFuture().join();

            applicationRelease.complete(null);
            application.toCompletableFuture().join();
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void instanceClosingWaitsForAcceptedApplicationAndInfrastructureTurns()
        throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try (ZLinkWorkerPool workerPool = new ZLinkWorkerPool(
                0, 1, java.time.Duration.ofSeconds(1), 1)) {
            TestHost host = new TestHost(executor);
            DefaultInstanceSpotContext context =
                host.instanceContext(workerPool);
            CompletableFuture<Void> applicationRelease =
                new CompletableFuture<>();
            CompletableFuture<Void> infrastructureRelease =
                new CompletableFuture<>();
            CompletableFuture<Void> applicationStarted =
                new CompletableFuture<>();
            CompletableFuture<Void> infrastructureStarted =
                new CompletableFuture<>();
            CompletableFuture<Void> closingStarted =
                new CompletableFuture<>();

            context.enqueueDispatch(() -> {
                applicationStarted.complete(null);
                return applicationRelease;
            });
            context.enqueueInfrastructureDispatch(() -> {
                infrastructureStarted.complete(null);
                return infrastructureRelease;
            });
            applicationStarted.get(2, TimeUnit.SECONDS);
            infrastructureStarted.get(2, TimeUnit.SECONDS);

            CompletionStage<Void> closing = context.runClosing(() -> {
                closingStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

            assertFalse(closingStarted.isDone());
            applicationRelease.complete(null);
            assertFalse(closingStarted.isDone());
            infrastructureRelease.complete(null);
            closing.toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTrue(closingStarted.isDone());
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void spotWideYieldReleasesSharedGateButRetainsActorQueueClaim()
        throws Exception {
        ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> firstRelease = new CompletableFuture<>();
            CompletableFuture<Void> firstStarted = new CompletableFuture<>();
            CompletableFuture<Void> secondStarted = new CompletableFuture<>();

            CompletionStage<Void> first = context.enqueueDispatch(() ->
                context.enqueueActorDispatch(
                    "actor-a",
                    37,
                    () -> {
                        var execution = ZLinkSuspendInvocationContext
                            .currentApplicationExecution();
                        assertTrue(execution.sharedSpotGate());
                        assertTrue(execution.yieldAllowed());
                        firstStarted.complete(null);
                        return ZLinkSerialExecutionQueue.yieldCurrent(firstRelease);
                    }));
            firstStarted.get(2, TimeUnit.SECONDS);

            CompletionStage<Void> second = context.enqueueActorDispatch(
                "actor-b",
                41,
                () -> {
                    secondStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                });

            secondStarted.get(2, TimeUnit.SECONDS);
            firstRelease.complete(null);
            CompletableFuture.allOf(
                first.toCompletableFuture(),
                second.toCompletableFuture()).join();
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void relocatingActorWaitYieldsSharedSpotGateBeforeReenqueue()
        throws Exception {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.SPOT_WIDE);
            CompletableFuture<Void> moveCompleted = new CompletableFuture<>();
            CompletableFuture<Void> moveWaitStarted = new CompletableFuture<>();
            CompletableFuture<Void> actorStarted = new CompletableFuture<>();

            CompletionStage<Void> dispatch = context.enqueueDispatch(() -> {
                moveWaitStarted.complete(null);
                return ZLinkSpotRuntime.yieldSharedSpotTurnForActorRelocation(
                    context,
                    moveCompleted)
                    .thenCompose(ignored -> context.enqueueActorDispatch(
                        "relocated-actor",
                        () -> {
                            actorStarted.complete(null);
                            return CompletableFuture.completedFuture(null);
                        }));
            });

            moveWaitStarted.get(2, TimeUnit.SECONDS);
            executor.submit(() -> moveCompleted.complete(null));
            actorStarted.get(2, TimeUnit.SECONDS);
            dispatch.toCompletableFuture().get(2, TimeUnit.SECONDS);
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void acceptedActorPacketBehindDeferredTurnCanBeCapturedForRelocation()
        throws Exception {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try {
            TestHost host = new TestHost(executor);
            DefaultSpotContext context = host.userContext(
                ZLinkUserSpotExecutionMode.PER_ACTOR);
            CompletableFuture<Void> activeRelease = new CompletableFuture<>();
            CompletableFuture<Void> activeStarted = new CompletableFuture<>();
            AtomicReference<ZLinkSerialExecutionQueue.ActiveTurnSealHandle> handle =
                new AtomicReference<>();
            byte[] acceptedRecord = new byte[] {7, 8, 9};

            CompletionStage<Void> active = context.enqueueActorDispatch(
                "actor-a",
                () -> {
                    handle.set(context.actorRelocationLane("actor-a")
                        .captureActiveTurnSealHandle()
                        .orElseThrow());
                    activeStarted.complete(null);
                    return activeRelease;
                });
            activeStarted.get(2, TimeUnit.SECONDS);
            CompletionStage<Void> pending = context.enqueueActorDispatch(
                "actor-a",
                () -> acceptedRecord,
                acceptedRecord.length,
                () -> CompletableFuture.completedFuture(null),
                () -> { });

            ZLinkSerialExecutionQueue queue = context.actorRelocationLane("actor-a");
            ZLinkSerialExecutionQueue.RelocationSeal seal = queue
                .trySealRelocation(handle.get())
                .orElseThrow();
            assertEquals(1, seal.captured().size());
            assertArrayEquals(
                acceptedRecord,
                seal.captured().getFirst().payload());

            assertTrue(queue.abortRelocation(seal));
            activeRelease.complete(null);
            CompletableFuture.allOf(
                active.toCompletableFuture(),
                pending.toCompletableFuture()).get(2, TimeUnit.SECONDS);
        } finally {
            executor.shutdownNow();
        }
    }

    @Test
    void entrySpotApplicationPayloadUsesActorQueue() {
        TestHost host = new TestHost();
        DefaultEntrySpotContext context = host.entryContext();
        CompletableFuture<Void> firstRelease = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first = context.enqueueActorDispatch(
            "actor-entry",
            11,
            () -> {
                var execution = ZLinkSuspendInvocationContext
                    .currentApplicationExecution();
                assertFalse(execution.sharedSpotGate());
                assertFalse(execution.yieldAllowed());
                firstStarted.complete(null);
                return firstRelease;
            });
        firstStarted.join();
        CompletionStage<Void> second = context.enqueueActorDispatch(
            "actor-entry",
            13,
            () -> {
                secondStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

        assertFalse(secondStarted.isDone());
        firstRelease.complete(null);
        CompletableFuture.allOf(
            first.toCompletableFuture(),
            second.toCompletableFuture()).join();
        assertEquals(2, host.actorDispatchSubmissions.get());
    }

    private static final class TestHost extends ZLinkSpotContextHost {
        private final Executor executor;
        private final AtomicInteger actorDispatchSubmissions =
            new AtomicInteger();
        private final ZLinkBackendSpot backendSpot = backendSpot();

        TestHost() {
            this(Runnable::run);
        }

        TestHost(Executor executor) {
            this.executor = executor;
        }

        DefaultSpotContext userContext(
            ZLinkUserSpotExecutionMode executionMode) {
            return userContext(
                executionMode,
                ZLinkSpotRelocationCoordinationMode.FRAMEWORK_MANAGED);
        }

        DefaultSpotContext userContext(
            ZLinkUserSpotExecutionMode executionMode,
            ZLinkSpotRelocationCoordinationMode relocationCoordinationMode) {
            return new DefaultSpotContext(
                this,
                null,
                null,
                RoutingId.from("node-a"),
                backendSpot,
                new ZLinkSerialExecutionQueue(
                    executor, ZLinkExecutionLanePolicy.spot()),
                executionMode,
                false,
                null,
                relocationCoordinationMode);
        }

        DefaultEntrySpotContext entryContext() {
            return new DefaultEntrySpotContext(
                this,
                null,
                null,
                RoutingId.from("node-a"),
                backendSpot);
        }

        DefaultInstanceSpotContext instanceContext(
            ZLinkWorkerPool workerPool) {
            ZLinkScannedHandlerCatalog scannedHandlers =
                new ZLinkScannedHandlerCatalog(List.of());
            return new DefaultInstanceSpotContext(
                this,
                workerPool,
                new ZLinkSpotHandlerLoader(
                    scannedHandlers,
                    new ZLinkSpotActorHandlerCatalog(
                        scannedHandlers, null)),
                "instance-mesh",
                RoutingId.from("node-a"),
                backendSpot);
        }

        @Override
        Executor serialExecutor() {
            return executor;
        }

        @Override
        Executor infrastructureExecutor() {
            return executor;
        }

        @Override
        DefaultSpotOutbound createContextOutbound(
            ZLinkBackendSpot backendSpot,
            RoutingId nodeRid) {
            return null;
        }

        @Override
        ZLinkSpotTimerRegistry createTimerRegistry(
            String spotId,
            ZLinkHandlerInstanceOwner handlers,
            ZLinkSpotTimerRegistry.Dispatch dispatch) {
            return new ZLinkSpotTimerRegistry(
                spotId,
                null,
                handlers,
                List.of(),
                null,
                "test",
                dispatch);
        }

        @Override
        ZLinkHandlerInstanceOwner createHandlerInstances() {
            return new ZLinkHandlerInstanceOwner(
                ZLinkHandlerActivator.reflection());
        }

        @Override
        CompletionStage<Void> destroyActorFromEntry(
            RoutingId nodeRid,
            ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        CompletionStage<Void> leaveActor(
            RoutingId nodeRid,
            ZLinkSpot<?> spot,
            ZLinkActor actor,
            String fallbackSpotId) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        CompletionStage<Boolean> closeSpot(String spotId) {
            return CompletableFuture.completedFuture(true);
        }

        @Override
        CompletionStage<Boolean> closeInstanceSpot(
            String spotId,
            long objectGeneration) {
            return CompletableFuture.completedFuture(true);
        }

        @Override
        CompletionStage<Boolean> completeInstanceSpotClose(
            ZLinkInstanceSpotActivation activation) {
            return CompletableFuture.completedFuture(true);
        }

        @Override
        boolean isActorMember(String spotId, String actorId) {
            return true;
        }

        @Override
        boolean isActorAtSpot(String actorId, String spotId) {
            return true;
        }

        @Override
        Object deferredActorJoinRuntimeScope() {
            return new Object();
        }

        @Override
        <T> CompletionStage<T> runWithOutbound(
            DefaultSpotOutbound outbound,
            Supplier<CompletionStage<T>> operation) {
            return operation.get();
        }

        @Override
        CompletionStage<Void> runEntryDispatch(
            Object entryContext,
            Supplier<CompletionStage<Void>> operation) {
            return operation.get();
        }

        @Override
        CompletionStage<Void> runActorTimerDispatch(
            String actorId,
            Supplier<CompletionStage<Void>> operation) {
            return operation.get();
        }

        @Override
        CompletionStage<Void> enqueueActorDispatch(
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation) {
            throw new AssertionError("actor dispatch must use its coordinator target");
        }

        @Override
        CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation) {
            actorDispatchSubmissions.incrementAndGet();
            return target.executeActor(actorId, payloadBytes, operation);
        }

        @Override
        CompletionStage<Void> enqueueActorDispatch(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
            throw new AssertionError("actor dispatch must use its coordinator target");
        }

        @Override
        CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease) {
            actorDispatchSubmissions.incrementAndGet();
            return target.executeActorLazyRecord(
                actorId,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                operation,
                relocationRelease);
        }

        private static ZLinkBackendSpot backendSpot() {
            return (ZLinkBackendSpot) Proxy.newProxyInstance(
                ZLinkBackendSpot.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendSpot.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "spotId" -> "spot-a";
                    case "lifecycleGeneration" -> 1L;
                    case "name" -> "spot-a";
                    case "toString" -> "test-backend-spot";
                    case "hashCode" -> 1;
                    case "equals" -> proxy == arguments[0];
                    default -> defaultValue(method.getReturnType());
                });
        }

        private static Object defaultValue(Class<?> type) {
            if (!type.isPrimitive()) {
                return null;
            }
            if (type == boolean.class) return false;
            if (type == byte.class) return (byte) 0;
            if (type == short.class) return (short) 0;
            if (type == int.class) return 0;
            if (type == long.class) return 0L;
            if (type == float.class) return 0F;
            if (type == double.class) return 0D;
            if (type == char.class) return '\0';
            return null;
        }
    }

    private static boolean lifecycleYieldAllowed(
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot) {
        return DefaultSpotContext.lifecycleYieldAllowed(
            executionMode,
            instanceSpot);
    }
}
