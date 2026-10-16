package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaStreamRelocationTest {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(1);

    @Test
    void sameGenerationPublicationRejectsADifferentSession() {
        try (var fixture = new Fixture()) {
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            spots.publishStreamSession(
                                    RoutingId.from("another-session"),
                                    fixture.source,
                                    fixture.generation(),
                                    1,
                                    fixture.stream));
        }
    }

    @Test
    void sameGenerationPublicationRejectsADifferentSlot() {
        try (var fixture = new Fixture()) {
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            spots.publishStreamSession(
                                    fixture.session,
                                    fixture.source,
                                    fixture.generation(),
                                    2,
                                    fixture.stream));
        }
    }

    @Test
    void sameGenerationPublicationRejectsADifferentPhysicalStream() {
        try (var fixture = new Fixture();
                var other =
                        new ZLinkJavaStreamSocket(
                                fixture.context.createStreamSocket(), fixture.node)) {
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            spots.publishStreamSession(
                                    fixture.session,
                                    fixture.source,
                                    fixture.generation(),
                                    1,
                                    other));
        }
    }

    @Test
    void sameGenerationPublicationRejectsADifferentActorIncarnation() {
        try (var fixture = new Fixture()) {
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            var other =
                    new ZLinkBackendActorRef(
                            fixture.source.nodeRid(), "player", fixture.source.generation() + 1);
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            spots.publishStreamSession(
                                    fixture.session,
                                    other,
                                    fixture.generation(),
                                    1,
                                    fixture.stream));
        }
    }

    @Test
    void sameGenerationPublicationAcceptsARouteChange() {
        try (var fixture = new Fixture()) {
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            assertDoesNotThrow(
                    () ->
                            spots.publishStreamSession(
                                    fixture.session,
                                    fixture.target,
                                    fixture.generation(),
                                    1,
                                    fixture.stream));
        }
    }

    @Test
    void relayAfterRelocationReachesTheTargetWithoutContactingTheRetiredSource() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.nativeTarget = true;
            var delivered = new CompletableFuture<RoutingId>();
            fixture.node
                    .spotNode()
                    .entrySpot()
                    .onDispatchEvent(
                            info -> {
                                if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                                    RoutingId receivedSession =
                                            info.actorMessages().getFirst().sourceSessionRid();
                                    info.actorMessages().forEach(received -> received.close());
                                    delivered.complete(receivedSession);
                                }
                            });
            long generation = fixture.generation();
            fixture.relocate().toCompletableFuture().join();
            try (var payload = Message.from("move-after-drain")) {
                assertTrue(
                        fixture.stream.sendBoundActor(
                                fixture.session, "player", List.of(payload), SendFlags.DONT_WAIT));
            }
            assertEquals(
                    fixture.session,
                    delivered.get(REQUEST_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS));
            assertEquals(generation, fixture.generation());
            assertEquals(0, fixture.unbinds.get());
        }
    }

    @Test
    void retiredSourceDoesNotPreventTheSameBindingFromMovingToTheTarget() {
        try (var fixture = new Fixture()) {
            long generation = fixture.generation();
            fixture.relocate().toCompletableFuture().join();
            assertEquals(0, fixture.unbinds.get());
            assertEquals(2, fixture.binds.get());
            assertEquals(generation, fixture.generation());
            assertDoesNotThrow(() -> fixture.stream.publishBoundActor(fixture.session, "player"));
        }
    }

    @Test
    void rejectedTargetLeavesTheSourceBindingPublishedWithoutRebindingIt() {
        try (var fixture = new Fixture()) {
            var rejected = new IllegalStateException("target rejected preparation");
            fixture.preparation = CompletableFuture.failedFuture(rejected);
            long generation = fixture.generation();
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () -> fixture.relocate().toCompletableFuture().join());
            assertSame(rejected, failure.getCause());
            assertEquals(0, fixture.unbinds.get());
            assertEquals(2, fixture.binds.get());
            assertEquals(generation, fixture.generation());
            assertDoesNotThrow(() -> fixture.stream.publishBoundActor(fixture.session, "player"));
        }
    }

    @Test
    void latePreparationCannotReplaceANewerBinding() {
        try (var fixture = new Fixture()) {
            var release = new CompletableFuture<Void>();
            fixture.preparation = release;
            long oldGeneration = fixture.generation();
            var relocation = fixture.relocate().toCompletableFuture();
            try {
                fixture.stream
                        .bindActor(fixture.session, fixture.source, 1)
                        .submit(REQUEST_TIMEOUT)
                        .toCompletableFuture()
                        .join();
                fixture.stream.publishBoundActor(fixture.session, "player");
            } finally {
                release.complete(null);
            }
            assertThrows(CompletionException.class, relocation::join);
            assertTrue(fixture.generation() > oldGeneration);
            assertEquals(0, fixture.unbinds.get());
            var spots = (ZLinkJavaRawSpotNode) fixture.node.spotNode();
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            spots.publishStreamSession(
                                    fixture.session,
                                    fixture.target,
                                    oldGeneration,
                                    1,
                                    fixture.stream));
        }
    }

    private static final class Fixture implements AutoCloseable {
        final systems.zlink.contracts.core.Context context = Zlink.createContext();
        final ZLinkJavaRawMeshNode node = new ZLinkJavaRawMeshNode(context, "mesh");
        final RoutingId session = RoutingId.from("relocation-session");
        final ZLinkBackendActorRef source;
        final ZLinkBackendActorRef target;
        final AtomicInteger unbinds = new AtomicInteger();
        final AtomicInteger binds = new AtomicInteger();
        CompletionStage<Void> preparation = CompletableFuture.completedFuture(null);
        boolean nativeTarget;
        final ZLinkJavaStreamSocket stream;

        Fixture() {
            node.setRoutingId(RoutingId.from("relocation-session-owner"));
            try (var create = Message.from("create")) {
                target = node.spotNode().createActor("player", create);
            }
            source =
                    new ZLinkBackendActorRef(
                            RoutingId.from("relocation-source"), "player", target.generation());
            stream =
                    new ZLinkJavaStreamSocket(
                            context.createStreamSocket(),
                            node,
                            null,
                            new ZLinkJavaStreamSocket.BoundSessionLifecycle() {
                                @Override
                                public CompletionStage<Void> bind(
                                        RoutingId rid,
                                        ZLinkBackendActorRef actor,
                                        long generation,
                                        Duration timeout) {
                                    binds.incrementAndGet();
                                    if (nativeTarget && actor.equals(target)) {
                                        return ((ZLinkJavaRawSpotNode) node.spotNode())
                                                .bindStreamSession(
                                                        rid, actor, generation, 1, stream, timeout);
                                    }
                                    return actor.equals(source)
                                            ? CompletableFuture.completedFuture(null)
                                            : preparation;
                                }

                                @Override
                                public CompletionStage<Void> unbind(
                                        RoutingId rid,
                                        ZLinkBackendActorRef actor,
                                        long generation,
                                        Duration timeout) {
                                    unbinds.incrementAndGet();
                                    return CompletableFuture.failedFuture(
                                            new IllegalStateException(
                                                    "source owner already retired"));
                                }
                            });
            stream.startSessionService();
            stream.bindActor(session, source, 1)
                    .submit(REQUEST_TIMEOUT)
                    .toCompletableFuture()
                    .join();
            stream.publishBoundActor(session, "player");
        }

        long generation() {
            return stream.boundActorBindingGeneration(session, "player");
        }

        CompletionStage<Void> relocate() {
            return stream.relocateBoundActor(
                    session, "player", generation(), target, REQUEST_TIMEOUT);
        }

        @Override
        public void close() {
            stream.close();
            node.close();
            context.close();
        }
    }
}
