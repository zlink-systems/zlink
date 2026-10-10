package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedDeque;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

final class ZLinkSameTargetActivationTest {
    @Test
    void coldFactoryAndHookRunOutsideIngressLane() throws Exception {
        assertActivationRunsOutsideIngressLane(false);
    }

    @Test
    void recoveryFactoryAndHookRunOutsideIngressLane() throws Exception {
        assertActivationRunsOutsideIngressLane(true);
    }

    private void assertActivationRunsOutsideIngressLane(boolean recovery) throws Exception {
        var ingressThread = Thread.currentThread();
        var factoryThread = new CompletableFuture<Thread>();
        var hookThread = new CompletableFuture<Thread>();
        var restoreThread = new CompletableFuture<Thread>();
        var lane = new ZLinkStateLane(Runnable::run);
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "activation-thread")) {
            var registry = new ZLinkJavaInstanceSpotRegistry(lane, owner::executeApplication);
            registry.register(
                    "room",
                    (id, generation) -> {
                        factoryThread.complete(Thread.currentThread());
                        return backendSpot(generation);
                    },
                    (type, id, generation, spot) -> {
                        hookThread.complete(Thread.currentThread());
                        return CompletableFuture.completedFuture(null);
                    });
            CompletionStage<ZLinkJavaInstanceSpotRegistry.Activation> activation =
                    recovery
                            ? registry.activate(
                                    "spot",
                                    "room",
                                    1,
                                    Long.MAX_VALUE,
                                    spot -> restoreThread.complete(Thread.currentThread()))
                            : registry.activate(
                                    "spot",
                                    "room",
                                    Long.MAX_VALUE,
                                    () -> CompletableFuture.completedFuture(1L),
                                    spot -> restoreThread.complete(Thread.currentThread()));
            activation.toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertNotSame(ingressThread, factoryThread.join(), "factory ran on the ingress thread");
            assertNotSame(ingressThread, hookThread.join(), "hook ran on the ingress thread");
            assertNotSame(ingressThread, restoreThread.join(), "restore ran on the ingress thread");
            registry.closeAll();
        }
    }

    @Test
    void ingressArrivalOrderSurvivesOutOfOrderActivationAdmission() throws Exception {
        var ready = new CompletableFuture<Void>();
        var scheduled = new ConcurrentLinkedDeque<Runnable>();
        var firstScheduled = new CompletableFuture<Void>();
        var admitted = new CopyOnWriteArrayList<Long>();
        var failures = new CopyOnWriteArrayList<Throwable>();
        var receivedAll = new CountDownLatch(3);
        var targetRid = RoutingId.from("fifo-target");
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "fifo")) {
            owner.setRoutingId(targetRid);
            var target = (ZLinkJavaRawSpotNode) owner.spotNode();
            var observedAuthority =
                    new systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing(
                            java.time.Instant.now());
            target.setInstanceSpotActivationValidator(
                    envelope -> CompletableFuture.completedFuture(observedAuthority));
            target.registerInstanceSpotType(
                    "room",
                    new ZLinkInternalMeshNode.InstanceSpotActivationHandler() {
                        @Override
                        public <T> CompletionStage<T> admit(Supplier<CompletionStage<T>> work) {
                            var result = new CompletableFuture<T>();
                            scheduled.addLast(
                                    () ->
                                            work.get()
                                                    .whenComplete(
                                                            (value, failure) -> {
                                                                if (failure == null)
                                                                    result.complete(value);
                                                                else
                                                                    result.completeExceptionally(
                                                                            failure);
                                                            }));
                            firstScheduled.complete(null);
                            return result;
                        }

                        @Override
                        public CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                                ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                                ZLinkAuthorityReadResult authority) {
                            assertSame(observedAuthority, authority);
                            return CompletableFuture.completedFuture(
                                    new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                                            targetRid, 1, "spot", 1, "owner", 1, 1, "version"));
                        }

                        @Override
                        public CompletionStage<Void> activate(
                                String type,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence route,
                                ZLinkBackendSpot spot) {
                            spot.onDispatchEvent(
                                    new ZLinkInternalAsyncSpotDispatchHandler() {
                                        @Override
                                        public CompletionStage<Void> handleAsync(
                                                ZLinkBackendSpotDispatchInfo info) {
                                            return CompletableFuture.completedFuture(null);
                                        }

                                        @Override
                                        public CompletionStage<Void> handleRoute(
                                                ZLinkBackendReceived received,
                                                CompletableFuture<Void> admission) {
                                            admitted.add(
                                                    received.activationMessage()
                                                            .orElseThrow()
                                                            .operationLow());
                                            admission.complete(null);
                                            received.close();
                                            receivedAll.countDown();
                                            return CompletableFuture.completedFuture(null);
                                        }
                                    });
                            return ready;
                        }
                    });
            for (long operation = 1; operation <= 3; operation++) {
                var header =
                        new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                                0,
                                new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                                        targetRid, 1, "spot", "fifo", "room", "1", Long.MAX_VALUE),
                                true,
                                1,
                                targetRid,
                                null,
                                false,
                                1571,
                                operation,
                                null);
                assertTrue(
                        target.enqueueRemoteInstanceSpot(
                                targetRid,
                                header,
                                new byte[0],
                                List.of(Message.from(new byte[] {1})),
                                null,
                                reply -> reply.forEach(Message::close),
                                failures::add));
            }
            firstScheduled.get(2, TimeUnit.SECONDS);
            scheduled.removeFirst().run();
            Runnable next;
            while ((next = scheduled.pollLast()) != null) next.run();
            assertTrue(admitted.isEmpty());
            ready.complete(null);
            assertTrue(receivedAll.await(2, TimeUnit.SECONDS));
            assertEquals(List.of(), failures);
            assertEquals(List.of(1L, 2L, 3L), admitted);
        }
    }

    @Test
    void pendingOperationsEnterTheQueueInTargetArrivalOrder() {
        var ready = new CompletableFuture<Void>();
        var created = new AtomicInteger();
        var admitted = new ArrayList<Integer>();
        var registry =
                new ZLinkJavaInstanceSpotRegistry(new ZLinkStateLane(Runnable::run), Runnable::run);
        registry.register(
                "room",
                (id, generation) -> {
                    created.incrementAndGet();
                    return backendSpot(generation);
                },
                (type, id, generation, spot) -> ready);
        var event = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(1));
        var request = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(2));
        var later = registry.activate("spot", "room", 1, Long.MAX_VALUE, spot -> admitted.add(3));
        assertTrue(admitted.isEmpty());
        ready.complete(null);
        assertSame(
                event.toCompletableFuture().join().spot(),
                request.toCompletableFuture().join().spot());
        assertSame(
                event.toCompletableFuture().join().spot(),
                later.toCompletableFuture().join().spot());
        assertEquals(1, created.get());
        assertEquals(List.of(1, 2, 3), admitted);
        registry.closeAll();
    }

    private static ZLinkBackendSpot backendSpot(long generation) {
        return (ZLinkBackendSpot)
                Proxy.newProxyInstance(
                        ZLinkBackendSpot.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendSpot.class},
                        (proxy, method, arguments) ->
                                switch (method.getName()) {
                                    case "lifecycleGeneration" -> generation;
                                    case "close" -> null;
                                    default -> throw new AssertionError(method.getName());
                                });
    }
}
