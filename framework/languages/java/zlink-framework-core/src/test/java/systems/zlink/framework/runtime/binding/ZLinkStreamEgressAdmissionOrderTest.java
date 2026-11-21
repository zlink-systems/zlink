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
import java.util.concurrent.ExecutionException;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkStreamEgressAdmissionOrderTest {
    @RepeatedTest(40)
    void pendingAdmissionOnOnePeerDoesNotHoldAnotherPeersBindOrClose() throws Exception {
        exerciseUnrelatedPeerProgress(false);
    }

    @RepeatedTest(40)
    void expiredCloseWaitDoesNotHoldAnotherPeersBindOrClose() throws Exception {
        exerciseUnrelatedPeerProgress(true);
    }

    private void exerciseUnrelatedPeerProgress(boolean expireCallerWait) throws Exception {
        var admitted = new CompletableFuture<Void>();
        var firstSubmit = new CompletableFuture<Void>();
        var bindCompleted = new CompletableFuture<Void>();
        var closeCompleted = new CompletableFuture<Void>();
        var readyPeers = new LinkedBlockingQueue<RoutingId>();
        try (var context = Zlink.createContext();
                var nativeSocket = context.createStreamSocket();
                var admission =
                        new ZLinkStreamAdmissionTestScope(
                                previous ->
                                        (socket, rid, parts, timeout) -> {
                                            if (firstSubmit.complete(null)) return admitted;
                                            return CompletableFuture.completedFuture(null);
                                        })) {
            try (var stream =
                            new ZLinkJavaStreamSocket(
                                    nativeSocket, null, new ZLinkStateLane(Runnable::run));
                    var payload = Message.from("pending")) {
                stream.onTransportError(
                        (rid, event, code, message) -> {
                            if (event == MonitorEventType.CONNECTION_READY) readyPeers.add(rid);
                        });
                stream.bind("tcp://127.0.0.1:0");
                var endpoint = java.net.URI.create(stream.lastEndpoint());
                try (var clientA = new java.net.Socket(endpoint.getHost(), endpoint.getPort());
                        var clientB = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                    var peerA = readyPeers.poll(5, TimeUnit.SECONDS);
                    var peerB = readyPeers.poll(5, TimeUnit.SECONDS);
                    assertNotNull(peerA);
                    assertNotNull(peerB);
                    assertNotEquals(peerA, peerB);

                    stream.sendBoundSessionPushAsync(peerA, List.of(payload));
                    firstSubmit.get(5, TimeUnit.SECONDS);
                    Thread closeA = null;
                    CompletableFuture<Void> closeACompletion = null;
                    if (expireCallerWait) {
                        closeACompletion = new CompletableFuture<>();
                        CompletableFuture<Void> completion = closeACompletion;
                        closeA =
                                Thread.ofVirtual()
                                        .start(
                                                () -> {
                                                    try {
                                                        stream.disconnectPeer(peerA);
                                                        completion.complete(null);
                                                    } catch (Throwable failure) {
                                                        completion.completeExceptionally(failure);
                                                    }
                                                });
                        try {
                            closeACompletion
                                    .thenApply(ignored -> (Void) null)
                                    .orTimeout(1, TimeUnit.MILLISECONDS)
                                    .get(5, TimeUnit.SECONDS);
                            fail("the held close wait should reach its existing caller deadline");
                        } catch (ExecutionException expected) {
                            assertInstanceOf(TimeoutException.class, expected.getCause());
                        }
                    }

                    var bind =
                            Thread.ofVirtual()
                                    .start(
                                            () -> {
                                                try {
                                                    stream.bind("tcp://127.0.0.1:0");
                                                    bindCompleted.complete(null);
                                                } catch (Throwable failure) {
                                                    bindCompleted.completeExceptionally(failure);
                                                }
                                            });
                    var close =
                            Thread.ofVirtual()
                                    .start(
                                            () -> {
                                                try {
                                                    stream.disconnectPeer(peerB);
                                                    closeCompleted.complete(null);
                                                } catch (Throwable failure) {
                                                    closeCompleted.completeExceptionally(failure);
                                                }
                                            });
                    try {
                        bindCompleted.get(250, TimeUnit.MILLISECONDS);
                        closeCompleted.get(250, TimeUnit.MILLISECONDS);
                        assertTrue(firstSubmit.isDone());
                    } finally {
                        admitted.complete(null);
                        if (closeA != null) {
                            closeA.join(java.time.Duration.ofSeconds(5));
                            assertFalse(closeA.isAlive());
                            closeACompletion.get(5, TimeUnit.SECONDS);
                        }
                        bind.join(java.time.Duration.ofSeconds(5));
                        close.join(java.time.Duration.ofSeconds(5));
                    }
                    assertFalse(bind.isAlive());
                    assertFalse(close.isAlive());
                } finally {
                    admitted.complete(null);
                }
            } finally {
                admitted.complete(null);
            }
        }
    }

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
    void expiredQueuedCloseDoesNotDisconnectAfterAdmissionCompletes() throws Exception {
        var admitted = new CompletableFuture<Void>();
        var callerWait = new CompletableFuture<Void>();
        var observed = new CopyOnWriteArrayList<String>();
        var firstSubmit = new AtomicBoolean();
        try (var context = Zlink.createContext();
                var nativeSocket = context.createStreamSocket();
                var admission =
                        new ZLinkStreamAdmissionTestScope(
                                previous ->
                                        (socket, rid, parts, timeout) -> {
                                            observed.add("submit");
                                            if (firstSubmit.compareAndSet(false, true))
                                                return admitted;
                                            return CompletableFuture.completedFuture(null);
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
                    var payload = Message.from("pending")) {
                var routingId = RoutingId.from("peer");
                var submitted =
                        stream.replyAsync(
                                routingId,
                                new ZLinkStreamHeader("Reply", Map.of(), Optional.empty()),
                                List.of(payload));
                var close =
                        stream.disconnectBoundSessionPeerAsync(
                                routingId, 0, () -> !callerWait.isDone());
                assertFalse(close.toCompletableFuture().isDone());

                var timedOut = callerWait.orTimeout(1, TimeUnit.MILLISECONDS);
                try {
                    timedOut.get(5, TimeUnit.SECONDS);
                    fail("the close caller wait should reach its existing deadline");
                } catch (ExecutionException expected) {
                    assertInstanceOf(TimeoutException.class, expected.getCause());
                }
                assertFalse(close.toCompletableFuture().isDone());
                assertEquals(List.of("submit"), observed);

                observed.add("admitted");
                admitted.complete(null);
                close.toCompletableFuture().get(5, TimeUnit.SECONDS);
                submitted.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(List.of("submit", "admitted"), observed);
            } finally {
                admitted.complete(null);
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
