package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;

final class ZLinkStreamEgressAdmissionOrderTest {
    @RepeatedTest(40)
    void physicalCloseWaitsForPreviouslyRegisteredCoreAdmission() throws Exception {
        var admitted = new CompletableFuture<Void>();
        var observed = new CopyOnWriteArrayList<String>();
        // Preserve the binding's registered implementation after replacing only its admission
        // boundary. No native send or close is performed by this controlled backend.
        try (var context = Zlink.createContext();
                var nativeSocket = context.createStreamSocket();
                var admission =
                        new ZLinkStreamAdmissionTestScope(
                                previous ->
                                        (socket, rid, parts, timeout) -> {
                                            observed.add("submit");
                                            return admitted;
                                        })) {
            var socket =
                    (StreamSocket)
                            Proxy.newProxyInstance(
                                    StreamSocket.class.getClassLoader(),
                                    new Class<?>[] {StreamSocket.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("disconnectRid")) {
                                            observed.add("close");
                                            return null;
                                        }
                                        try {
                                            return method.invoke(nativeSocket, arguments);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            try (var stream =
                            new ZLinkJavaStreamSocket(
                                    socket, null, new ZLinkStateLane(Runnable::run));
                    var payload = Message.from("reply")) {
                var submitted =
                        stream.replyAsync(
                                RoutingId.from("peer"),
                                new ZLinkStreamHeader("Reply", Map.of(), Optional.empty()),
                                List.of(payload));
                var close =
                        Thread.ofVirtual()
                                .unstarted(() -> stream.disconnectPeer(RoutingId.from("peer")));
                try {
                    close.start();
                    long deadline = System.nanoTime() + java.time.Duration.ofSeconds(5).toNanos();
                    // WAITING proves close is queued behind the pending lane item; TERMINATED
                    // proves it reached the backend. No elapsed delay determines their order.
                    while (close.getState() != Thread.State.WAITING
                            && close.isAlive()
                            && System.nanoTime() < deadline) Thread.onSpinWait();
                    assertTrue(close.getState() == Thread.State.WAITING || !close.isAlive());
                    assertEquals(List.of("submit"), observed);
                    observed.add("admitted");
                    admitted.complete(null);
                    close.join(java.time.Duration.ofSeconds(5));
                    assertFalse(close.isAlive());
                    submitted.toCompletableFuture().join();
                    assertEquals(List.of("submit", "admitted", "close"), observed);
                } finally {
                    admitted.complete(null);
                    close.join(java.time.Duration.ofSeconds(5));
                }
            }
        }
    }

    @RepeatedTest(40)
    void rawClientReceivesReplyBeforeEof() throws Exception {
        exercise(false);
    }

    @RepeatedTest(40)
    void rawClientReceivesPushBeforeEof() throws Exception {
        exercise(true);
    }

    private void exercise(boolean push) throws Exception {
        var ready = new CompletableFuture<RoutingId>();
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
            stream.onTransportError(
                    (rid, event, code, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) ready.complete(rid);
                    });
            stream.bind("tcp://127.0.0.1:0");
            var endpoint = URI.create(stream.lastEndpoint());
            try (var client = new Socket(endpoint.getHost(), endpoint.getPort());
                    var payload = Message.from("last-frame")) {
                client.setSoTimeout(5000);
                var rid = ready.get(5, TimeUnit.SECONDS);
                var header = new ZLinkStreamHeader("LastFrame", Map.of(), Optional.empty());
                byte[] expected = ZLinkStreamFrameCodec.encode(header, payload.toByteArray());
                CompletableFuture<Void> submitted;
                if (push) {
                    try (var frame = Message.from(expected)) {
                        submitted =
                                stream.sendBoundSessionPushAsync(rid, List.of(frame))
                                        .toCompletableFuture();
                    }
                } else {
                    submitted =
                            stream.replyAsync(rid, header, List.of(payload)).toCompletableFuture();
                }
                // Keep client reads closed until close has started; receiving first would hide
                // a loss of the last frame already accepted by Core.
                stream.disconnectPeer(rid);
                submitted.get(5, TimeUnit.SECONDS);
                assertArrayEquals(expected, client.getInputStream().readAllBytes());
            }
        }
    }
}
