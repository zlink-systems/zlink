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
        var readyPeers = new LinkedBlockingQueue<RoutingId>();
        try (var context = Zlink.createContext();
                var stream =
                        new ZLinkJavaStreamSocket(
                                context.createStreamSocket(),
                                new ZLinkStateLane(Runnable::run),
                                (socket, rid, parts, timeout) -> {
                                    if (firstSubmit.complete(null)) return admitted;
                                    return CompletableFuture.completedFuture(null);
                                });
                var payload = Message.from("pending")) {
            stream.onTransportError(
                    (rid, event, code, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) readyPeers.add(rid);
                    });
            stream.bind("tcp://127.0.0.1:0");
            var endpoint = URI.create(stream.lastEndpoint());
            try (var clientA = new Socket(endpoint.getHost(), endpoint.getPort());
                    var clientB = new Socket(endpoint.getHost(), endpoint.getPort())) {
                var peerA = readyPeers.poll(5, TimeUnit.SECONDS);
                var peerB = readyPeers.poll(5, TimeUnit.SECONDS);
                assertNotNull(peerA);
                assertNotNull(peerB);
                assertNotEquals(peerA, peerB);

                var pending = stream.sendBoundSessionPushAsync(peerA, List.of(payload));
                firstSubmit.get(5, TimeUnit.SECONDS);
                var closeA =
                        expireCallerWait
                                ? stream.disconnectBoundSessionPeerAsync(peerA, 0)
                                        .toCompletableFuture()
                                : null;
                if (closeA != null) {
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () ->
                                            closeA.copy()
                                                    .orTimeout(1, TimeUnit.MILLISECONDS)
                                                    .get(5, TimeUnit.SECONDS));
                    assertInstanceOf(TimeoutException.class, failure.getCause());
                }

                var bind =
                        CompletableFuture.runAsync(
                                () -> stream.bind("tcp://127.0.0.1:0"), Thread::startVirtualThread);
                var close = stream.disconnectBoundSessionPeerAsync(peerB, 0);
                try {
                    bind.get(250, TimeUnit.MILLISECONDS);
                    close.toCompletableFuture().get(250, TimeUnit.MILLISECONDS);
                    assertFalse(pending.toCompletableFuture().isDone());
                } finally {
                    admitted.complete(null);
                    if (closeA != null) closeA.get(5, TimeUnit.SECONDS);
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
        try (var context = Zlink.createContext();
                var stream =
                        new ZLinkJavaStreamSocket(
                                observeDisconnect(context.createStreamSocket(), observed),
                                new ZLinkStateLane(Runnable::run),
                                (socket, rid, parts, timeout) -> {
                                    observed.add("submit");
                                    return admitted;
                                });
                var payload = Message.from("reply")) {
            var rid = RoutingId.from("peer");
            var submitted =
                    stream.replyAsync(
                            rid,
                            new ZLinkStreamHeader("Reply", Map.of(), Optional.empty()),
                            List.of(payload));
            try {
                var close = stream.disconnectBoundSessionPeerAsync(rid, 0).toCompletableFuture();
                assertFalse(close.isDone());
                assertEquals(List.of("submit"), observed);
                observed.add("admitted");
                admitted.complete(null);
                close.get(5, TimeUnit.SECONDS);
                submitted.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(List.of("submit", "admitted", "close"), observed);
            } finally {
                admitted.complete(null);
            }
        }
    }

    @RepeatedTest(40)
    void expiredQueuedCloseDoesNotDisconnectAfterAdmissionCompletes() throws Exception {
        var admitted = new CompletableFuture<Void>();
        var callerWait = new CompletableFuture<Void>();
        var observed = new CopyOnWriteArrayList<String>();
        try (var context = Zlink.createContext();
                var stream =
                        new ZLinkJavaStreamSocket(
                                observeDisconnect(context.createStreamSocket(), observed),
                                new ZLinkStateLane(Runnable::run),
                                (socket, rid, parts, timeout) -> {
                                    observed.add("submit");
                                    return admitted;
                                });
                var payload = Message.from("pending")) {
            try {
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

    private static StreamSocket observeDisconnect(
            StreamSocket nativeSocket, List<String> observed) {
        return (StreamSocket)
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
                stream.disconnectBoundSessionPeerAsync(rid, 0)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                submitted.get(5, TimeUnit.SECONDS);
                assertArrayEquals(expected, client.getInputStream().readAllBytes());
            }
        }
    }
}
