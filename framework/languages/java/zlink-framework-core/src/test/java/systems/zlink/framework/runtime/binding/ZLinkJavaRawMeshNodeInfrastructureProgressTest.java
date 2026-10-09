package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

final class ZLinkJavaRawMeshNodeInfrastructureProgressTest {
    @Test
    void reverseControlAdmissionDoesNotHoldOtherPeerReceive() throws Exception {
        var sourceRid = RoutingId.from("infra-progress-source");
        var pausedRid = RoutingId.from("infra-progress-paused");
        var otherRid = RoutingId.from("infra-progress-other");
        String sourceEndpoint = "inproc://infra-source-" + System.nanoTime();
        String pausedEndpoint = "inproc://infra-paused-" + System.nanoTime();
        try (var context = Zlink.createContext();
                var pausedPort = new ZLinkJavaRawServicePort(context);
                var paused = pausedPort.openRouter(pausedRid);
                var source = new ZLinkJavaRawMeshNode(context, "infra-progress");
                var other = new ZLinkJavaRawMeshNode(context, "infra-progress")) {
            paused.setRoutingId(pausedRid);
            paused.options().recvHwm(1);
            paused.options().receiveFlowState(ReceiveFlowState.PAUSED);
            paused.bind(pausedEndpoint);
            source.setRoutingId(sourceRid);
            source.setBind(sourceEndpoint);
            source.start();
            var routerField = ZLinkJavaRawMeshNode.class.getDeclaredField("router");
            routerField.setAccessible(true);
            var router = (RouterSocket) routerField.get(source);
            router.options().sendHwm(1);
            source.connectPeer(pausedEndpoint, pausedRid);
            await(() -> !router.routesSnapshot().isEmpty());

            Method send =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "sendLiveness", RoutingId.class, List.class);
            send.setAccessible(true);
            Method encode =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "encodeLiveness", int.class, long.class);
            encode.setAccessible(true);
            @SuppressWarnings("unchecked")
            var frames =
                    (List<byte[]>)
                            encode.invoke(
                                    null,
                                    systems.zlink.framework.runtime.protocol.ServiceWireConstants
                                            .COMMAND_LIVENESS_PROBE,
                                    1L);
            var fill = (CompletionStage<?>) send.invoke(source, pausedRid, frames);
            var pending = (CompletionStage<?>) send.invoke(source, pausedRid, frames);
            assertFalse(pending.toCompletableFuture().isDone(), pending.toString());

            other.setRoutingId(otherRid);
            other.setBind("inproc://infra-other-" + System.nanoTime());
            other.start();
            other.connectPeer(sourceEndpoint, sourceRid);
            await(() -> source.isPeerTransportConnected(otherRid));
            assertFalse(pending.toCompletableFuture().isDone());
            paused.options().receiveFlowState(ReceiveFlowState.RUNNING);
            await(
                    () -> {
                        pausedPort
                                .receiveNow(paused)
                                .ifPresent(ZLinkJavaRawServicePort.Inbound::close);
                        return pending.toCompletableFuture().isDone()
                                && fill.toCompletableFuture().isDone();
                    });
            pending.toCompletableFuture().get(2, TimeUnit.SECONDS);
            fill.toCompletableFuture().get(2, TimeUnit.SECONDS);
        }
    }

    private static void await(BooleanSupplier ready) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!ready.getAsBoolean()) {
            if (System.nanoTime() >= deadline)
                throw new AssertionError("infrastructure did not progress");
            Thread.sleep(1);
        }
    }

    @Test
    void exhaustedOrdinaryPermitEndsReceiveTurnBeforeNextInfrastructureTurn() throws Exception {
        var queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
                        100,
                        0);
        var held = queue.acquire().toCompletableFuture().join();
        var executor = Executors.newSingleThreadExecutor();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "infra-progress")) {
            node.setApplicationJobQueue(queue);
            var pump = ZLinkJavaRawMeshNode.class.getDeclaredField("pump");
            pump.setAccessible(true);
            pump.set(node, executor);
            Method drain =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "drainIngressBatch", RouterSocket.class);
            drain.setAccessible(true);
            Method tick = ZLinkJavaRawMeshNode.class.getDeclaredMethod("tickLiveness", long.class);
            tick.setAccessible(true);
            var ticks = new AtomicInteger();
            // No socket is needed: a pre-receive permit must prevent native receive.
            var round =
                    executor.submit(
                            () -> {
                                drain.invoke(node, new Object[] {null});
                                tick.invoke(node, System.nanoTime());
                                ticks.incrementAndGet();
                                return null;
                            });
            round.get(1, TimeUnit.SECONDS);
            assertEquals(1, ticks.get());
            drain.invoke(node, new Object[] {null});
            assertEquals(1, queue.snapshot().capacityWaitCount());
        } finally {
            executor.shutdownNow();
            assertTrue(executor.awaitTermination(2, TimeUnit.SECONDS));
            held.close();
            queue.close();
        }
    }
}
