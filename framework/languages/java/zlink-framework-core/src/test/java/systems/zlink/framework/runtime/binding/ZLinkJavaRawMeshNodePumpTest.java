package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawMeshNodePumpTest {
    @Test
    void serialRequestsKeepTheReceiveTaskUntilShutdown() throws Exception {
        runSerialRequests(false);
    }

    @Test
    void pendingPermitYieldsToItsGrantOnTheSameExecutor() throws Exception {
        runSerialRequests(true);
    }

    private static void runSerialRequests(boolean exhaustPermit) throws Exception {
        CountingExecutor sourcePump;
        CountingExecutor targetPump;
        try (var queue =
                        new ZLinkApplicationJobQueue(
                                ZLinkApplicationJobQueueProfile.BALANCED,
                                OptionalLong.of(1),
                                new ZLinkApplicationJobQueue.ProcessorCandidates(
                                        1, null, null, null),
                                100,
                                0);
                var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "pump");
                var target = new ZLinkJavaRawMeshNode(context, "pump")) {
            RoutingId sourceRid = RoutingId.from("pump-source");
            RoutingId targetRid = RoutingId.from("pump-target");
            String targetEndpoint = "inproc://pump-target-" + UUID.randomUUID();
            var incoming = new LinkedBlockingQueue<ZLinkMeshDispatchRecord>();
            target.setApplicationReceiver(
                    new systems.zlink.framework.runtime.internal.backend
                            .ZLinkMeshApplicationReceiver() {
                        @Override
                        public void accept(ZLinkMeshDispatchRecord record) {
                            incoming.add(record);
                        }

                        @Override
                        public java.util.concurrent.CompletionStage<Integer> submitLocalNodeSend(
                                RoutingId sourceNodeRid, byte[] metadata, List<Message> parts) {
                            throw new AssertionError("remote request fixture");
                        }
                    });
            source.setRoutingId(sourceRid);
            target.setRoutingId(targetRid);
            source.setBind("inproc://pump-source-" + UUID.randomUUID());
            target.setBind(targetEndpoint);
            source.start();
            target.start();
            sourcePump = countPump(source);
            targetPump = countPump(target);
            var ready = new CompletableFuture<Void>();
            try (var observation =
                    source.onPeerStateChanged(
                            targetRid,
                            () -> {
                                if (source.observePeerRoute(targetRid).ready())
                                    ready.complete(null);
                            },
                            () -> {})) {
                source.connectPeer(targetEndpoint, targetRid);
                ready.get(5, TimeUnit.SECONDS);
            }
            // Install after startup to isolate permit callbacks from Core PAUSED flow control.
            // This white-box case exercises the receive executor's grant queue directly.
            ZLinkApplicationJobQueue.Permit held = null;
            if (exhaustPermit) {
                target.setApplicationJobQueue(queue);
                held =
                        queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join();
            }
            try {
                for (int i = 0; i < 3; i++) {
                    try (var packet = Message.from("pump.packet");
                            var payload = Message.from("request")) {
                        var reply =
                                source.requestNode(
                                        targetRid,
                                        null,
                                        List.of(packet, payload),
                                        Duration.ofSeconds(5),
                                        null,
                                        null,
                                        null);
                        if (held != null) {
                            Field field =
                                    ZLinkJavaRawMeshNode.class.getDeclaredField(
                                            "pendingReceiveAcquire");
                            field.setAccessible(true);
                            var pending =
                                    (java.util.concurrent.atomic.AtomicReference<?>)
                                            field.get(target);
                            var yielded = new CompletableFuture<Boolean>();
                            targetPump.execute(() -> yielded.complete(pending.get() != null));
                            assertTrue(yielded.get(5, TimeUnit.SECONDS));
                            assertTrue(incoming.isEmpty());
                            assertFalse(reply.toCompletableFuture().isDone());
                            held.close();
                            held = null;
                        }
                        var record = incoming.poll(5, TimeUnit.SECONDS);
                        assertNotNull(record);
                        try (record;
                                var response = Message.from("reply")) {
                            record.reply(List.of(response));
                        }
                        try (var received = reply.toCompletableFuture().get(5, TimeUnit.SECONDS)) {
                            assertEquals("reply", received.parts().getFirst().toUtf8String());
                        }
                    }
                }
            } finally {
                if (held != null) held.close();
            }
        }
        assertTrue(sourcePump.isTerminated());
        assertTrue(targetPump.isTerminated());
        assertEquals(0, sourcePump.posts.get(), "source pump reposted a receive rotation");
        if (!exhaustPermit)
            assertEquals(0, targetPump.posts.get(), "target pump reposted a receive rotation");
        else
            // The probe above is one post; the grant and the resumed rotation must add more.
            assertTrue(
                    targetPump.posts.get() >= 2, "pending grant must run on the receive executor");
    }

    private static CountingExecutor countPump(ZLinkJavaRawMeshNode node) throws Exception {
        Field field = ZLinkJavaRawMeshNode.class.getDeclaredField("pump");
        field.setAccessible(true);
        var counting = new CountingExecutor((ExecutorService) field.get(node));
        field.set(node, counting);
        return counting;
    }

    private static final class CountingExecutor extends AbstractExecutorService {
        private final ExecutorService delegate;
        private final AtomicInteger posts = new AtomicInteger();

        private CountingExecutor(ExecutorService delegate) {
            this.delegate = delegate;
        }

        @Override
        public void execute(Runnable command) {
            posts.incrementAndGet();
            delegate.execute(command);
        }

        @Override
        public void shutdown() {
            delegate.shutdown();
        }

        @Override
        public List<Runnable> shutdownNow() {
            return delegate.shutdownNow();
        }

        @Override
        public boolean isShutdown() {
            return delegate.isShutdown();
        }

        @Override
        public boolean isTerminated() {
            return delegate.isTerminated();
        }

        @Override
        public boolean awaitTermination(long timeout, TimeUnit unit) throws InterruptedException {
            return delegate.awaitTermination(timeout, unit);
        }
    }
}
