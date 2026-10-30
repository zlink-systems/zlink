package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaDelayedTcpAdmissionTest {
    @Test
    void firstNodeRequestWaitsForDelayedTcpHandshakeAndArrivesOnce() throws Exception {
        var incoming = new LinkedBlockingQueue<ZLinkMeshDispatchRecord>();
        var calls = new AtomicInteger();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "delayed-tcp");
                var target = new ZLinkJavaRawMeshNode(context, "delayed-tcp")) {
            source.setRoutingId(RoutingId.from("delayed-source"));
            source.setBind("tcp://127.0.0.1:0");
            target.setRoutingId(RoutingId.from("delayed-target"));
            target.setBind("tcp://127.0.0.1:0");
            target.setAdvertiseHost("127.0.0.2");
            target.setApplicationReceiver(
                    new ZLinkMeshApplicationReceiver() {
                        @Override
                        public void accept(ZLinkMeshDispatchRecord record) {
                            calls.incrementAndGet();
                            incoming.add(record);
                        }

                        @Override
                        public CompletionStage<Integer> submitLocalNodeSend(
                                RoutingId sourceRid, byte[] metadata, List<Message> parts) {
                            throw new AssertionError("remote request expected");
                        }
                    });
            source.start();
            target.start();
            try (var proxy = new DelayedProxy(URI.create(target.advertisedEndpoint()).getPort());
                    var packet = Message.from("delayed.packet");
                    var payload = Message.from("request")) {
                source.connectPeer(target.advertisedEndpoint(), target.routingId());
                proxy.accepted.get(3, TimeUnit.SECONDS);
                assertFalse(source.observePeerRoute(target.routingId()).ready());
                var request =
                        source.requestNode(
                                        target.routingId(),
                                        null,
                                        List.of(packet, payload),
                                        Duration.ofSeconds(5),
                                        null,
                                        null,
                                        null)
                                .toCompletableFuture();
                assertFalse(request.isDone(), "request must wait for the withheld Core handshake");
                assertEquals(0, calls.get());
                proxy.release.countDown();
                var record = incoming.poll(5, TimeUnit.SECONDS);
                assertNotNull(record, "the original request must reach the target after release");
                try (record;
                        var reply = Message.from("reply")) {
                    record.reply(List.of(reply));
                }
                try (var received = request.get(5, TimeUnit.SECONDS)) {
                    assertEquals("reply", received.parts().getFirst().toUtf8String());
                }
                assertEquals(1, calls.get());
                assertTrue(incoming.isEmpty());
                assertTrue(source.observePeerRoute(target.routingId()).ready());
            }
        }
    }

    /** Holds both TCP directions before the Core handshake without interpreting frames. */
    private static final class DelayedProxy implements AutoCloseable {
        final ServerSocket listener = new ServerSocket();
        final CountDownLatch release = new CountDownLatch(1);
        final CompletableFuture<Void> accepted = new CompletableFuture<>();
        final List<Socket> sockets = new CopyOnWriteArrayList<>();
        volatile boolean closed;

        DelayedProxy(int port) throws IOException {
            listener.bind(new InetSocketAddress("127.0.0.2", port));
            Thread.ofVirtual()
                    .start(
                            () -> {
                                try {
                                    while (!closed) {
                                        var client = listener.accept();
                                        sockets.add(client);
                                        Thread.ofVirtual().start(() -> forward(client, port));
                                    }
                                } catch (IOException failure) {
                                    if (!closed) accepted.completeExceptionally(failure);
                                }
                            });
        }

        private void forward(Socket client, int port) {
            try {
                var upstream = new Socket("127.0.0.1", port);
                sockets.add(upstream);
                accepted.complete(null);
                release.await();
                Thread.ofVirtual().start(() -> copy(upstream, client));
                copy(client, upstream);
            } catch (IOException failure) {
                if (!closed) accepted.completeExceptionally(failure);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                accepted.completeExceptionally(failure);
            }
        }

        private void copy(Socket source, Socket target) {
            try {
                source.getInputStream().transferTo(target.getOutputStream());
            } catch (IOException failure) {
                if (!closed) accepted.completeExceptionally(failure);
            }
        }

        @Override
        public void close() throws IOException {
            closed = true;
            release.countDown();
            listener.close();
            for (var socket : sockets) socket.close();
        }
    }
}
