package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSuspendedHandlerWorkerTest {
    private static final AtomicInteger arrived = new AtomicInteger();
    private static CompletableFuture<Void> both;

    @Test
    void twoSuspendedSpotHandlersReturnWorkersForAnotherSpot() throws Exception {
        arrived.set(0);
        both = new CompletableFuture<>();
        try (var workers = Executors.newFixedThreadPool(2)) {
            var options = new DefaultZLinkFrameworkOptions();
            options.useHandlerExecutor(workers);
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            options.addRouteMesh("suspended-workers")
                    .listen("tcp://127.0.0.1:0")
                    .objects()
                    .server()
                    .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
            try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
                for (var id : new String[] {"b", "a0", "a1"}) {
                    runtime.spotManager()
                            .getOrCreate(id, "room")
                            .inMesh("suspended-workers")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
                var first =
                        runtime.route()
                                .requestToSpot("a0", new Outer(0))
                                .timeout(Duration.ofSeconds(2))
                                .submit(Reply.class)
                                .toCompletableFuture();
                var second =
                        runtime.route()
                                .requestToSpot("a1", new Outer(1))
                                .timeout(Duration.ofSeconds(2))
                                .submit(Reply.class)
                                .toCompletableFuture();
                CompletableFuture.allOf(first, second).get(2, TimeUnit.SECONDS);
                assertEquals(new Reply(0, true), first.get());
                assertEquals(new Reply(1, true), second.get());
                assertEquals(2, arrived.get());
            }
        }
    }

    public record Outer(int id) {}

    public record Inner(int id) {}

    public record Reply(int id, boolean ok) {}

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(OuterHandler.class);
            context.handlers().addHandler(InnerHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class OuterHandler implements ZLinkSpotRequestHandler<Room, Outer, Reply> {
        @Override
        public CompletionStage<Reply> handle(Room spot, Outer request) {
            if (arrived.incrementAndGet() == 2) both.complete(null);
            return both.thenCompose(
                    ignored ->
                            spot.context()
                                    .outbound()
                                    .requestToSpot("b", new Inner(request.id()))
                                    .timeout(Duration.ofSeconds(2))
                                    .submit(Reply.class));
        }
    }

    public static final class InnerHandler implements ZLinkSpotRequestHandler<Room, Inner, Reply> {
        @Override
        public CompletionStage<Reply> handle(Room spot, Inner request) {
            return CompletableFuture.completedFuture(new Reply(request.id(), true));
        }
    }
}
