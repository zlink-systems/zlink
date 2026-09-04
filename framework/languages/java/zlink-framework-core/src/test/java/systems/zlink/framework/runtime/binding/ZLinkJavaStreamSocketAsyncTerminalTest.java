package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

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
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
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

final class ZLinkJavaStreamSocketAsyncTerminalTest {
    @Test
    void asyncSendReturnsBeforeTheSocketStateLaneCanStartAdmission()
        throws Exception {
        CountDownLatch laneEntered = new CountDownLatch(1);
        CountDownLatch releaseLane = new CountDownLatch(1);
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node);
             Message payload = Message.from(new byte[0])) {
            Field stateLaneField = ZLinkJavaStreamSocket.class
                .getDeclaredField("stateLane");
            stateLaneField.setAccessible(true);
            ZLinkStateLane stateLane =
                (ZLinkStateLane) stateLaneField.get(stream);
            CompletionStage<Void> laneBlocker = stateLane.runAsync(() -> {
                laneEntered.countDown();
                try {
                    if (!releaseLane.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("socket state lane was not released");
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(
                        "socket state lane wait was interrupted", interrupted);
                }
            });
            assertTrue(laneEntered.await(1, TimeUnit.SECONDS));

            ZLinkStreamHeader header = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.CONTROL,
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                "$zlink.heartbeat.pong",
                Map.of(),
                Optional.empty());
            AtomicReference<Thread> invocationThread = new AtomicReference<>();
            CompletableFuture<CompletionStage<Void>> invocation =
                CompletableFuture.supplyAsync(() -> {
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
                blockedAt = Arrays.toString(
                    invocationThread.get().getStackTrace());
            } finally {
                releaseLane.countDown();
                if (submission == null) {
                    submission = invocation.get(1, TimeUnit.SECONDS);
                }
                laneBlocker.toCompletableFuture().get(1, TimeUnit.SECONDS);
            }

            assertTrue(returnedBeforeRelease,
                "async STREAM send waited for the socket state lane: "
                    + blockedAt);
        }
    }

    @Test
    void asyncBoundActorRelayPreservesTheStreamHeaderFrame()
        throws Exception {
        RoutingId nodeRid = RoutingId.from("async-stream-node");
        RoutingId sessionRid = RoutingId.from("async-stream-session");
        CompletableFuture<List<ZLinkBackendActorReceived>> delivered =
            new CompletableFuture<>();
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node)) {
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    delivered.complete(List.copyOf(info.actorMessages()));
                }
            });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("async-stream-actor", create);
            }
            stream.startSessionService();
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            ZLinkStreamHeader header = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.SEND,
                ZLinkStreamCodec.JSON,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                "BoundSessionBind",
                Map.of("trace", "async"));

            try (Message body = Message.from("payload")) {
                stream.relayBoundActorAsync(
                        sessionRid,
                        actor.actorId(),
                        header,
                        List.of(body))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            }

            List<ZLinkBackendActorReceived> frames =
                delivered.get(1, TimeUnit.SECONDS);
            try {
                assertEquals(2, frames.size());
                ZLinkStreamHeader receivedHeader =
                    ZLinkStreamHeaderCodec.decodeOrPlain(
                        frames.getFirst().message().toByteArray());
                assertEquals(header, receivedHeader);
                assertEquals("payload",
                    frames.getLast().message().toUtf8String());
            } finally {
                frames.forEach(ZLinkBackendActorReceived::close);
            }
        }
    }

    @Test
    void asyncBoundActorRelayPreservesAnAlreadyAcceptedSessionSequence()
        throws Exception {
        RoutingId nodeRid = RoutingId.from("explicit-sequence-node");
        RoutingId sessionRid = RoutingId.from("explicit-sequence-session");
        CompletableFuture<List<ZLinkBackendActorReceived>> delivered =
            new CompletableFuture<>();
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node)) {
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    delivered.complete(List.copyOf(info.actorMessages()));
                }
            });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor(
                    "explicit-sequence-actor", create);
            }
            stream.startSessionService();
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            ZLinkStreamHeader header = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.SEND,
                ZLinkStreamCodec.JSON,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                "ExplicitSequence",
                Map.of());

            try (Message body = Message.from("payload")) {
                stream.relayBoundActorAsync(
                        sessionRid,
                        actor.actorId(),
                        73,
                        header,
                        List.of(body))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            }

            List<ZLinkBackendActorReceived> frames =
                delivered.get(1, TimeUnit.SECONDS);
            try {
                assertEquals(2, frames.size());
                assertEquals(header, ZLinkStreamHeaderCodec.decodeOrPlain(
                    frames.getFirst().message().toByteArray()));
                assertEquals("payload",
                    frames.getLast().message().toUtf8String());
            } finally {
                frames.forEach(ZLinkBackendActorReceived::close);
            }

            try (Message duplicate = Message.from("duplicate")) {
                ExecutionException failure = assertThrows(
                    ExecutionException.class,
                    () -> stream.relayBoundActorAsync(
                            sessionRid,
                            actor.actorId(),
                            73,
                            header,
                            List.of(duplicate))
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS));
                ZlinkSubmitException rejected =
                    (ZlinkSubmitException) failure.getCause();
                assertEquals(SubmitResult.NOT_ADMITTED,
                    rejected.getResult());
            }
        }
    }
}
