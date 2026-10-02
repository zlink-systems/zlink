package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.LockSupport;

final class ZLinkJavaRawSpotNodeReplyLifetimeTest {
    enum Terminal {
        NO_REPLY,
        FAILURE,
        REPLY,
        CLOSE
    }

    @org.junit.jupiter.api.Test
    void relayFailureOwnsTerminalAndReleasesPartsWithoutLocalDispatch() throws Exception {
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "mesh")) {
            owner.setRoutingId(RoutingId.from("audit-relay-owner"));
            owner.setBind("inproc://audit-relay-owner-" + System.nanoTime());
            owner.start();
            var spots = (ZLinkJavaRawSpotNode) owner.spotNode();
            ZLinkBackendActorRef actor;
            try (var create = Message.from("create")) {
                actor = spots.createActor("relay-actor", create);
            }
            spots.rememberActorAuthority(actor, 11, 3);
            var dispatches = new AtomicInteger();
            spots.entrySpot()
                    .onDispatchEvent(
                            new ZLinkInternalAsyncSpotDispatchHandler() {
                                @Override
                                public CompletionStage<Void> handleAsync(
                                        ZLinkBackendSpotDispatchInfo info) {
                                    dispatches.incrementAndGet();
                                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                    return CompletableFuture.completedStage(null);
                                }
                            });
            var rejected = new IllegalStateException("relay failed");
            spots.setMessageFollowRelayHandler(
                    (rid,
                            generation,
                            header,
                            journal,
                            parts,
                            contentType,
                            reply,
                            failure,
                            release) -> {
                        throw rejected;
                    });
            var route =
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            actor, owner.lifecycleGeneration(), 11, 3);
            var header = new ZLinkServiceM6BWireCodec.ActorMessage(false, 0, null, null, route);
            var parts = List.of(Message.from("header"), Message.from("payload"));
            var terminal = new CompletableFuture<Throwable>();
            var released = new AtomicInteger();
            assertTrue(
                    spots.enqueueRemoteActor(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                    RoutingId.from("relay-source"), 7, "source", 5),
                            header,
                            () -> new byte[] {1},
                            parts,
                            null,
                            null,
                            terminal::complete,
                            () -> {
                                released.incrementAndGet();
                                parts.forEach(Message::close);
                            }));
            assertSame(rejected, terminal.get(1, TimeUnit.SECONDS));
            assertEquals(1, released.get());
            assertEquals(0, dispatches.get());
        }
    }

    @org.junit.jupiter.api.Test
    void drainWaitsForTheActualActorRequestTerminalAndPreservesItsFailure() throws Exception {
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "mesh")) {
            owner.setRoutingId(RoutingId.from("audit-drain-owner"));
            owner.setBind("inproc://audit-drain-owner-" + System.nanoTime());
            owner.start();
            var spots = (ZLinkJavaRawSpotNode) owner.spotNode();
            ZLinkBackendActorRef actor;
            try (var create = Message.from("create")) {
                actor = spots.createActor("drain-actor", create);
            }
            var entered = new CompletableFuture<Void>();
            var handler = new CompletableFuture<Void>();
            spots.entrySpot()
                    .onDispatchEvent(
                            new ZLinkInternalAsyncSpotDispatchHandler() {
                                @Override
                                public CompletionStage<Void> handleAsync(
                                        ZLinkBackendSpotDispatchInfo info) {
                                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                    entered.complete(null);
                                    return handler;
                                }
                            });
            CompletionStage<List<Message>> request;
            try (var payload = Message.from("request")) {
                request =
                        spots.requestToActor(
                                actor,
                                List.of(payload),
                                systems.zlink.contracts.sockets.SendFlags.DONT_WAIT,
                                Duration.ofSeconds(5));
            }
            entered.get(1, TimeUnit.SECONDS);
            var drain = spots.awaitPendingActorRequests().toCompletableFuture();
            assertFalse(drain.isDone());
            var failure = new IllegalStateException("request handler failed");
            handler.completeExceptionally(failure);
            assertSame(
                    failure,
                    assertThrows(
                                    ExecutionException.class,
                                    () -> request.toCompletableFuture().get(1, TimeUnit.SECONDS))
                            .getCause());
            drain.get(1, TimeUnit.SECONDS);
            assertFalse(spots.hasPendingActorRequests());
        }
    }

    @ParameterizedTest
    @EnumSource(Terminal.class)
    void handlerTerminalReplyAndCloseReleaseTheSameReplyRoute(Terminal terminal) throws Exception {
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "mesh")) {
            owner.setRoutingId(RoutingId.from("audit-reply-owner"));
            owner.setBind("inproc://audit-reply-owner-" + System.nanoTime());
            owner.start();
            var spots = (ZLinkJavaRawSpotNode) owner.spotNode();
            ZLinkBackendActorRef actor;
            try (var create = Message.from("create")) {
                actor = spots.createActor("reply-actor", create);
            }
            spots.rememberActorAuthority(actor, 11, 3);
            var handler = new CompletableFuture<Void>();
            var ingress = new CompletableFuture<ZLinkBackendActorReceived>();
            spots.entrySpot()
                    .onDispatchEvent(
                            new ZLinkInternalAsyncSpotDispatchHandler() {
                                @Override
                                public CompletionStage<Void> handleAsync(
                                        ZLinkBackendSpotDispatchInfo info) {
                                    var first = info.actorMessages().getFirst();
                                    ingress.complete(first);
                                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                    return handler;
                                }
                            });
            var source = RoutingId.from("audit-reply-caller");
            var route =
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            actor, owner.lifecycleGeneration(), 11, 3);
            var header = new ZLinkServiceM6BWireCodec.ActorMessage(true, 0, 91L, null, route);
            var replies = new AtomicInteger();
            var failure = new CompletableFuture<Throwable>();
            assertTrue(
                    spots.enqueueRemoteActor(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(source, 7, "caller", 5),
                            header,
                            () -> new byte[] {1},
                            List.of(Message.from("header"), Message.from("payload")),
                            null,
                            parts -> {
                                replies.incrementAndGet();
                                parts.forEach(Message::close);
                            },
                            failure::complete));
            var received = ingress.get(1, TimeUnit.SECONDS);
            var field = ZLinkJavaRawSpotNode.class.getDeclaredField("actorRemoteReplies");
            field.setAccessible(true);
            var routes = (Map<?, ?>) field.get(spots);
            assertEquals(1, routes.size(), "payload release must not release the reply route");
            if (terminal == Terminal.REPLY) {
                try (var reply = Message.from("reply")) {
                    spots.replyActorNoBind(
                            actor, source, null, received.requestId(), 0, List.of(reply));
                }
                handler.complete(null);
            } else if (terminal == Terminal.FAILURE) {
                var rejected = new IllegalStateException("handler failed");
                handler.completeExceptionally(rejected);
                assertSame(rejected, failure.get(1, TimeUnit.SECONDS));
            } else if (terminal == Terminal.CLOSE) {
                spots.close();
                handler.complete(null);
            } else handler.complete(null);
            long deadline = System.nanoTime() + Duration.ofSeconds(1).toNanos();
            while (!routes.isEmpty() && System.nanoTime() < deadline)
                LockSupport.parkNanos(100_000);
            assertTrue(routes.isEmpty());
            if (terminal != Terminal.CLOSE) {
                try (var late = Message.from("late")) {
                    spots.replyActorNoBind(
                            actor, source, null, received.requestId(), 0, List.of(late));
                }
                assertEquals(terminal == Terminal.REPLY ? 1 : 0, replies.get());
            }
        }
    }
}
