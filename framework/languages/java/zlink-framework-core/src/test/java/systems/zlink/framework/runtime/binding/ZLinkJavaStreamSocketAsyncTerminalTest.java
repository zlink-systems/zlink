package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.Arrays;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkJavaStreamSocketAsyncTerminalTest {
    @Test
    void connectionReadyIsObservedBeforeFirstInboundFrame() throws Exception {
        var ready = new CompletableFuture<RoutingId>();
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
            stream.onTransportError(
                    (rid, event, nativeCode, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) {
                            ready.complete(rid);
                        }
                    });
            stream.bind("tcp://127.0.0.1:0");
            var endpoint = java.net.URI.create(stream.lastEndpoint());
            try (var client = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                assertTrue(ready.get(5, TimeUnit.SECONDS).size() > 0);
            }
        }
    }

    @Test
    void countSnapshotDoesNotPublishConnectionButReadyEdgeDoes() throws Exception {
        var events =
                new java.util.ArrayDeque<
                        systems.zlink.framework.runtime.internal.backend
                                .ZLinkBackendSocketMonitorEvent>();
        var rid = RoutingId.from("edge-peer");
        events.add(
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent(
                        MonitorEventType.CONNECTION_READY.name(), Optional.of(rid), "", "", 0));
        events.add(
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent(
                        MonitorEventType.CONNECTION_READY.name(),
                        Optional.of(rid),
                        "",
                        "",
                        systems.zlink.contracts.eventing.MonitorEventFlags.CONNECTION_READY_EDGE
                                .mask()));
        events.add(
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent(
                        MonitorEventType.DISCONNECTED.name(), Optional.of(rid), "", "", 0));
        var delivered = new java.util.ArrayList<MonitorEventType>();
        var monitorType =
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor.class;
        var eventMonitor =
                java.lang.reflect.Proxy.newProxyInstance(
                        monitorType.getClassLoader(),
                        new Class<?>[] {monitorType},
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "isClosed" -> events.isEmpty();
                                    case "waitForReadable" -> true;
                                    case "recvDontWait" -> events.remove();
                                    default -> throw new AssertionError(method.getName());
                                });
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
            var receiver =
                    ZLinkJavaStreamSocket.class.getDeclaredMethod(
                            "receiveMonitorEvents",
                            monitorType,
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendStreamErrorHandler.class);
            receiver.setAccessible(true);
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler
                    callback = (peer, event, nativeCode, message) -> delivered.add(event);
            receiver.invoke(stream, eventMonitor, callback);
            assertEquals(
                    List.of(MonitorEventType.CONNECTION_READY, MonitorEventType.DISCONNECTED),
                    delivered);
        }
    }

    @Test
    void unexpectedMonitorReceiveFailureIsLogged() throws Exception {
        var failure = new CompletableFuture<Throwable>();
        var logger = java.util.logging.Logger.getLogger(ZLinkJavaStreamSocket.class.getName());
        var logHandler =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        if (record.getThrown() != null) {
                            failure.complete(record.getThrown());
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(logHandler);
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
            var injected = new IllegalStateException("injected monitor wait failure");
            var monitorType =
                    systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor
                            .class;
            var eventMonitor =
                    java.lang.reflect.Proxy.newProxyInstance(
                            monitorType.getClassLoader(),
                            new Class<?>[] {monitorType},
                            (proxy, method, args) -> {
                                if (method.getName().equals("isClosed")) {
                                    return false;
                                }
                                throw injected;
                            });
            var receiver =
                    ZLinkJavaStreamSocket.class.getDeclaredMethod(
                            "receiveMonitorEvents",
                            monitorType,
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendStreamErrorHandler.class);
            receiver.setAccessible(true);
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler
                    callback = (rid, event, nativeCode, message) -> {};
            receiver.invoke(stream, eventMonitor, callback);
            assertEquals(injected, failure.get(5, TimeUnit.SECONDS));
        } finally {
            logger.removeHandler(logHandler);
        }
    }

    @Test
    void failedConnectionCallbackDisconnectsOnlyThatPeerAndMonitorContinues() throws Exception {
        var firstReady = new CompletableFuture<RoutingId>();
        var secondReady = new CompletableFuture<RoutingId>();
        var readyCount = new java.util.concurrent.atomic.AtomicInteger();
        var secondDisconnected = new CompletableFuture<RoutingId>();
        var callbackFailure = new IllegalStateException("injected Session creation failure");
        var recordedFailure = new CompletableFuture<Throwable>();
        var receiveFailure = new AtomicReference<String>();
        var logger = java.util.logging.Logger.getLogger(ZLinkJavaStreamSocket.class.getName());
        var logHandler =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        if (record.getThrown()
                                instanceof
                                systems.zlink.contracts.errors.ZlinkRecvException recvFailure) {
                            receiveFailure.set(
                                    recvFailure.getResult()
                                            + "/errno="
                                            + recvFailure.getNativeErrno());
                        }
                        if (record.getThrown() == callbackFailure) {
                            recordedFailure.complete(record.getThrown());
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(logHandler);
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
            stream.onTransportError(
                    (rid, event, nativeCode, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) {
                            readyCount.incrementAndGet();
                            if (firstReady.complete(rid)) {
                                throw callbackFailure;
                            }
                            if (!rid.equals(firstReady.getNow(null))) {
                                secondReady.complete(rid);
                            }
                        } else if (event == MonitorEventType.DISCONNECTED
                                && secondReady.isDone()
                                && rid.equals(secondReady.getNow(null))) {
                            secondDisconnected.complete(rid);
                        }
                    });
            stream.bind("tcp://127.0.0.1:0");
            var endpoint = java.net.URI.create(stream.lastEndpoint());
            try (var first = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                firstReady.get(5, TimeUnit.SECONDS);
                first.setSoTimeout((int) Duration.ofSeconds(5).toMillis());
                assertEquals(-1, first.getInputStream().read());
                assertEquals(callbackFailure, recordedFailure.get(5, TimeUnit.SECONDS));
                try (var second = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                    secondReady.get(5, TimeUnit.SECONDS);
                }
                try {
                    assertEquals(secondReady.get(), secondDisconnected.get(5, TimeUnit.SECONDS));
                } catch (TimeoutException failure) {
                    throw new AssertionError(
                            "monitor receiver failure: " + receiveFailure.get(), failure);
                }
                assertEquals(2, readyCount.get());
            }
        } finally {
            logger.removeHandler(logHandler);
        }
    }

    @Test
    void asyncSendReturnsBeforeTheSocketStateLaneCanStartAdmission() throws Exception {
        CountDownLatch laneEntered = new CountDownLatch(1);
        CountDownLatch releaseLane = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node);
                Message payload = Message.from(new byte[0])) {
            Field stateLaneField = ZLinkJavaStreamSocket.class.getDeclaredField("stateLane");
            stateLaneField.setAccessible(true);
            ZLinkStateLane stateLane = (ZLinkStateLane) stateLaneField.get(stream);
            CompletionStage<Void> laneBlocker =
                    stateLane.runAsync(
                            () -> {
                                laneEntered.countDown();
                                try {
                                    if (!releaseLane.await(5, TimeUnit.SECONDS)) {
                                        throw new AssertionError(
                                                "socket state lane was not released");
                                    }
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                    throw new AssertionError(
                                            "socket state lane wait was interrupted", interrupted);
                                }
                            });
            assertTrue(laneEntered.await(1, TimeUnit.SECONDS));

            ZLinkStreamHeader header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.CONTROL,
                            ZLinkStreamCodec.RAW,
                            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                            Optional.empty(),
                            "$zlink.heartbeat.pong",
                            Map.of(),
                            Optional.empty());
            AtomicReference<Thread> invocationThread = new AtomicReference<>();
            CompletableFuture<CompletionStage<Void>> invocation =
                    CompletableFuture.supplyAsync(
                            () -> {
                                invocationThread.set(Thread.currentThread());
                                return stream.sendAsync(
                                        RoutingId.from("pending-lane-peer"),
                                        header,
                                        List.of(payload));
                            });

            CompletionStage<Void> submission = null;
            boolean returnedBeforeRelease = true;
            String blockedAt = "";
            try {
                submission = invocation.get(1, TimeUnit.SECONDS);
            } catch (TimeoutException blocked) {
                returnedBeforeRelease = false;
                blockedAt = Arrays.toString(invocationThread.get().getStackTrace());
            } finally {
                releaseLane.countDown();
                if (submission == null) {
                    submission = invocation.get(1, TimeUnit.SECONDS);
                }
                laneBlocker.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }

            assertTrue(
                    returnedBeforeRelease,
                    "async STREAM send waited for the socket state lane: " + blockedAt);
        }
    }

    @Test
    void asyncBoundSessionPushReturnsBeforeTheSocketStateLaneCanStartAdmission() throws Exception {
        CountDownLatch laneEntered = new CountDownLatch(1);
        CountDownLatch releaseLane = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node);
                Message frame = Message.from("bound-session-frame")) {
            Field stateLaneField = ZLinkJavaStreamSocket.class.getDeclaredField("stateLane");
            stateLaneField.setAccessible(true);
            ZLinkStateLane stateLane = (ZLinkStateLane) stateLaneField.get(stream);
            CompletionStage<Void> laneBlocker =
                    stateLane.runAsync(
                            () -> {
                                laneEntered.countDown();
                                try {
                                    if (!releaseLane.await(5, TimeUnit.SECONDS)) {
                                        throw new AssertionError(
                                                "socket state lane was not released");
                                    }
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                    throw new AssertionError(
                                            "socket state lane wait was interrupted", interrupted);
                                }
                            });
            assertTrue(laneEntered.await(1, TimeUnit.SECONDS));

            AtomicReference<Thread> invocationThread = new AtomicReference<>();
            CompletableFuture<CompletionStage<Void>> invocation =
                    CompletableFuture.supplyAsync(
                            () -> {
                                invocationThread.set(Thread.currentThread());
                                return stream.sendBoundSessionPushAsync(
                                        RoutingId.from("pending-bound-session-peer"),
                                        List.of(frame));
                            });

            CompletionStage<Void> submission = null;
            boolean returnedBeforeRelease = true;
            String blockedAt = "";
            try {
                submission = invocation.get(1, TimeUnit.SECONDS);
            } catch (TimeoutException blocked) {
                returnedBeforeRelease = false;
                blockedAt = Arrays.toString(invocationThread.get().getStackTrace());
            } finally {
                releaseLane.countDown();
                if (submission == null) {
                    try {
                        submission = invocation.get(1, TimeUnit.SECONDS);
                    } catch (ExecutionException synchronousSendFailure) {
                        // The pre-fix path reaches native send inline after the
                        // lane is released. The assertion below owns the
                        // synchronous-wait failure this test is detecting.
                    }
                }
                laneBlocker.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }

            assertTrue(
                    returnedBeforeRelease,
                    "async bound Session push waited for the socket state lane: " + blockedAt);
        }
    }

    @Test
    void asyncBoundActorRelayPreservesTheStreamHeaderFrame() throws Exception {
        RoutingId nodeRid = RoutingId.from("async-stream-node");
        RoutingId sessionRid = RoutingId.from("async-stream-session");
        CompletableFuture<List<ZLinkBackendActorReceived>> delivered = new CompletableFuture<>();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node)) {
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(
                    info -> {
                        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                            delivered.complete(List.copyOf(info.actorMessages()));
                        }
                    });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("async-stream-actor", create);
            }
            stream.startSessionService();
            stream.bindActor(sessionRid, actor, 1)
                    .submit(Duration.ofSeconds(1))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            stream.publishBoundActor(sessionRid, actor.actorId());
            ZLinkStreamHeader header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.SEND,
                            ZLinkStreamCodec.JSON,
                            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                            Optional.empty(),
                            "BoundSessionBind",
                            Map.of("trace", "async"));

            try (Message body = Message.from("payload")) {
                stream.relayBoundActorAsync(sessionRid, actor.actorId(), header, List.of(body))
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS);
            }

            List<ZLinkBackendActorReceived> frames = delivered.get(1, TimeUnit.SECONDS);
            try {
                assertEquals(2, frames.size());
                ZLinkStreamHeader receivedHeader =
                        ZLinkStreamHeaderCodec.decodeOrPlain(
                                frames.getFirst().message().toByteArray());
                assertEquals(header, receivedHeader);
                assertEquals("payload", frames.getLast().message().toUtf8String());
            } finally {
                frames.forEach(ZLinkBackendActorReceived::close);
            }
        }
    }

    @Test
    void asyncBoundActorRelayPreservesAnAlreadyAcceptedSessionSequence() throws Exception {
        RoutingId nodeRid = RoutingId.from("explicit-sequence-node");
        RoutingId sessionRid = RoutingId.from("explicit-sequence-session");
        CompletableFuture<List<ZLinkBackendActorReceived>> delivered = new CompletableFuture<>();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node)) {
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(
                    info -> {
                        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                            delivered.complete(List.copyOf(info.actorMessages()));
                        }
                    });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("explicit-sequence-actor", create);
            }
            stream.startSessionService();
            stream.bindActor(sessionRid, actor, 1)
                    .submit(Duration.ofSeconds(1))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            stream.publishBoundActor(sessionRid, actor.actorId());
            ZLinkStreamHeader header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.SEND,
                            ZLinkStreamCodec.JSON,
                            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                            Optional.empty(),
                            "ExplicitSequence",
                            Map.of());

            try (Message body = Message.from("payload")) {
                stream.relayBoundActorAsync(sessionRid, actor.actorId(), 73, header, List.of(body))
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS);
            }

            List<ZLinkBackendActorReceived> frames = delivered.get(1, TimeUnit.SECONDS);
            try {
                assertEquals(2, frames.size());
                assertEquals(
                        header,
                        ZLinkStreamHeaderCodec.decodeOrPlain(
                                frames.getFirst().message().toByteArray()));
                assertEquals("payload", frames.getLast().message().toUtf8String());
            } finally {
                frames.forEach(ZLinkBackendActorReceived::close);
            }

            try (Message duplicate = Message.from("duplicate")) {
                ExecutionException failure =
                        assertThrows(
                                ExecutionException.class,
                                () ->
                                        stream.relayBoundActorAsync(
                                                        sessionRid,
                                                        actor.actorId(),
                                                        73,
                                                        header,
                                                        List.of(duplicate))
                                                .toCompletableFuture()
                                                .get(1, TimeUnit.SECONDS));
                ZlinkSubmitException rejected = (ZlinkSubmitException) failure.getCause();
                assertEquals(SubmitResult.NOT_ADMITTED, rejected.getResult());
            }
        }
    }
}
