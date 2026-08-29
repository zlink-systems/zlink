package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

final class ZLinkChannelReceiveLoopsApplicationJobQueueTest {
    @Test
    void shutdownJoinsTheRouterReceiveOwnerBeforeSocketClose() throws Exception {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        AtomicBoolean running = new AtomicBoolean(true);
        BlockingRouter router = new BlockingRouter();
        ZLinkChannelReceiveLoops loops = new ZLinkChannelReceiveLoops(running::get, queue);
        ExecutorService lifecycle = Executors.newSingleThreadExecutor();
        CountDownLatch closeStarted = new CountDownLatch(1);
        try {
            loops.startRequest(router, ignored -> { }, error -> {
                throw new AssertionError(error);
            });
            assertTrue(router.receiveEntered.await(1, TimeUnit.SECONDS));

            var close = lifecycle.submit(() -> {
                closeStarted.countDown();
                running.set(false);
                loops.close();
                loops.awaitTermination();
                router.close();
            });

            assertTrue(closeStarted.await(1, TimeUnit.SECONDS));
            assertFalse(router.closeEntered.await(1500, TimeUnit.MILLISECONDS),
                "socket close entered before the receive owner exited");
            router.releaseReceive.countDown();
            close.get(2, TimeUnit.SECONDS);
            assertTrue(router.receiveExited.get());
        } finally {
            router.releaseReceive.countDown();
            running.set(false);
            loops.close();
            loops.awaitTermination();
            queue.close();
            lifecycle.shutdownNow();
        }
    }

    @Test
    void ordinaryReceiveReservesBeforeRecvAndDoesNotBuildAHiddenSerialBacklog()
        throws Exception {
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        AtomicBoolean running = new AtomicBoolean(true);
        FakeRouter router = new FakeRouter();
        router.inbound.add(received("one"));
        router.inbound.add(received("two"));
        var handlerExecutor = Executors.newSingleThreadExecutor();
        ZLinkSerialExecutionQueue serial = new ZLinkSerialExecutionQueue(
            handlerExecutor, ZLinkExecutionLanePolicy.generic());
        CountDownLatch allowFirstInstruction = new CountDownLatch(1);
        CountDownLatch bothDispatched = new CountDownLatch(2);
        AtomicInteger job = new AtomicInteger();
        ZLinkChannelReceiveLoops loops = new ZLinkChannelReceiveLoops(running::get, queue);

        try {
            loops.startRequest(router, ignored -> {
                int index = job.incrementAndGet();
                serial.enqueue(() -> {
                    if (index == 1) {
                        await(allowFirstInstruction);
                    }
                    ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                    bothDispatched.countDown();
                    return CompletableFuture.completedFuture(null);
                });
            }, error -> { throw new AssertionError(error); });

            awaitCondition(() -> queue.snapshot().capacityWaiters() == 1);
            assertEquals(1, router.receiveCount.get());
            assertEquals(1, queue.snapshot().queuedApplicationJobs());
            assertEquals(1, queue.snapshot().capacityWaiters());

            allowFirstInstruction.countDown();
            assertTrue(bothDispatched.await(5, TimeUnit.SECONDS));
            assertEquals(2, router.receiveCount.get());
            assertEquals(1, router.receiveThreads.size());
            assertEquals(0, queue.snapshot().permitsInUse());
        } finally {
            running.set(false);
            loops.close();
            queue.close();
            handlerExecutor.shutdownNow();
            loops.awaitTermination();
        }
    }

    private static ZLinkBackendReceived received(String value) {
        return new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("job-queue-peer")),
            Optional.empty(),
            Optional.of(1L),
            List.of(Message.from(value)));
    }

    private static void await(CountDownLatch latch) {
        try {
            if (!latch.await(5, TimeUnit.SECONDS)) {
                throw new AssertionError("timed out");
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError(interrupted);
        }
    }

    private static void awaitCondition(java.util.function.BooleanSupplier condition)
        throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class FakeRouter implements ZLinkBackendRouterSocket {
        private final ArrayDeque<ZLinkBackendReceived> inbound = new ArrayDeque<>();
        private final AtomicInteger receiveCount = new AtomicInteger();
        private final Set<Thread> receiveThreads = ConcurrentHashMap.newKeySet();

        @Override public void setReceiveFlowState(
            systems.zlink.contracts.sockets.ReceiveFlowState state) { }
        @Override public void setChannelName(String value) { }
        @Override public void setRoutingId(RoutingId value) { }
        @Override public void setConnectRoutingId(RoutingId value) { }
        @Override public void setProbe(boolean value) { }
        @Override public long maxMessageSize() { return 0; }
        @Override public void setMaxMessageSize(long value) { }
        @Override public int peerWeight() { return 100; }
        @Override public void setPeerWeight(int value) { }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { }
        @Override public void disconnect(String endpoint) { }
        @Override public boolean waitForReadable(Duration timeout) {
            return !inbound.isEmpty();
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
            receiveThreads.add(Thread.currentThread());
            ZLinkBackendReceived result = inbound.poll();
            if (result != null) {
                receiveCount.incrementAndGet();
            }
            return result;
        }
        @Override public CompletionStage<Void> send(
            RoutingId routingId, List<Message> parts) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> request(
            RoutingId routingId, List<Message> parts, Duration timeout) {
            return CompletableFuture.failedFuture(new UnsupportedOperationException());
        }
        @Override public void reply(
            RoutingId routingId, long requestSeq, List<Message> parts) { }
        @Override public String name() { return "job-queue-router"; }
        @Override public void close() { }
    }

    private static final class BlockingRouter implements ZLinkBackendRouterSocket {
        private final CountDownLatch receiveEntered = new CountDownLatch(1);
        private final CountDownLatch releaseReceive = new CountDownLatch(1);
        private final CountDownLatch closeEntered = new CountDownLatch(1);
        private final AtomicBoolean receiveExited = new AtomicBoolean();

        @Override public void setReceiveFlowState(
            systems.zlink.contracts.sockets.ReceiveFlowState state) { }
        @Override public void setChannelName(String value) { }
        @Override public void setRoutingId(RoutingId value) { }
        @Override public void setConnectRoutingId(RoutingId value) { }
        @Override public void setProbe(boolean value) { }
        @Override public long maxMessageSize() { return 0; }
        @Override public void setMaxMessageSize(long value) { }
        @Override public int peerWeight() { return 100; }
        @Override public void setPeerWeight(int value) { }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { }
        @Override public void disconnect(String endpoint) { }
        @Override public boolean waitForReadable(Duration timeout) { return true; }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
            receiveEntered.countDown();
            boolean interrupted = false;
            while (true) {
                try {
                    releaseReceive.await();
                    break;
                } catch (InterruptedException ignored) {
                    interrupted = true;
                }
            }
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
            receiveExited.set(true);
            return null;
        }
        @Override public CompletionStage<Void> send(
            RoutingId routingId, List<Message> parts) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> request(
            RoutingId routingId, List<Message> parts, Duration timeout) {
            return CompletableFuture.failedFuture(new UnsupportedOperationException());
        }
        @Override public void reply(
            RoutingId routingId, long requestSeq, List<Message> parts) { }
        @Override public String name() { return "blocking-router"; }
        @Override public void close() { closeEntered.countDown(); }
    }
}
