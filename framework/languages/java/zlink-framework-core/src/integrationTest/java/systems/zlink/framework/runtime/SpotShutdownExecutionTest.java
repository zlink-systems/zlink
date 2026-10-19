package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.concurrent.*;

final class SpotShutdownExecutionTest {
    private static Probe probe;

    @Test
    void completedRequestDrainsNormally() throws Exception {
        exercise(true);
    }

    @Test
    void heldRequestDoesNotKeepShutdownWaiting() throws Exception {
        exercise(false);
    }

    private void exercise(boolean complete) throws Exception {
        probe = new Probe();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("shutdown-execution")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("shutdown-node"))
                .objects()
                .server()
                .addSpotFactory("target", Target.class, factory -> factory.disableRelocation())
                .addSpotFactory("source", Source.class, factory -> factory.disableRelocation());
        var runtime =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
        CompletableFuture<ZLinkFrameworkTerminationResult> shutdown = null;
        ZLinkFrameworkTerminationResult result;
        boolean returned = false;
        try {
            probe.target =
                    runtime.spotManager()
                            .create("target")
                            .inMesh("shutdown-execution")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .spot()
                            .spotId();
            runtime.spotManager()
                    .create("source")
                    .inMesh("shutdown-execution")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            probe.entered.get(5, TimeUnit.SECONDS);
            if (complete) {
                probe.release.complete("reply");
                probe.completed.get(5, TimeUnit.SECONDS);
            }
            shutdown =
                    (complete ? runtime.shutdown() : runtime.shutdown(Duration.ofMillis(50)))
                            .toCompletableFuture();
            try {
                shutdown.get(5, TimeUnit.SECONDS);
                returned = true;
            } catch (TimeoutException expected) {
                System.out.println("shutdown did not return while target response was held");
            }
            if (returned) {
                if (!complete) {
                    assertEquals(java.util.List.of("target-started"), probe.events);
                    probe.release.complete("late reply");
                    probe.release.complete("duplicate reply");
                }
                probe.disposed.get(5, TimeUnit.SECONDS);
                probe.timerCancelled.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(
                        java.util.List.of(
                                "target-started",
                                "target-ended",
                                "target-closing",
                                "target-disposed"),
                        probe.events);
                assertEquals(1, probe.terminals.get());
            }
        } finally {
            probe.release.complete("late reply");
            if (shutdown == null) runtime.close();
            else shutdown.get(10, TimeUnit.SECONDS);
        }
        result = shutdown.get();
        assertTrue(returned, "shutdown must return before the application releases its response");
        assertEquals(
                complete
                        ? ZLinkFrameworkTerminationOutcome.STOPPED
                        : ZLinkFrameworkTerminationOutcome.FORCE_STOPPED,
                result.outcome());
        assertEquals(
                complete
                        ? ZLinkFrameworkTerminationReason.NONE
                        : ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED,
                result.reason());
    }

    static final class Probe {
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final CompletableFuture<String> release = new CompletableFuture<>();
        final CompletableFuture<Void> completed = new CompletableFuture<>();
        final CompletableFuture<Void> disposed = new CompletableFuture<>();
        final java.util.List<String> events = new CopyOnWriteArrayList<>();
        final java.util.concurrent.atomic.AtomicInteger terminals =
                new java.util.concurrent.atomic.AtomicInteger();
        String target;
        CompletionStage<Void> timerCancelled;
    }

    public record Hold(String value) {}

    public static final class Target implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Target(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(HoldHandler.class);
        }

        public CompletionStage<Void> onClosing() {
            assertEquals("target-ended", probe.events.getLast());
            probe.events.add("target-closing");
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Source implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;
        private ZLinkTimer timer;

        public Source(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onInitialize() {
            return context.addTimer("request", Duration.ofMillis(10), RequestTimer.class, null)
                    .thenAccept(created -> timer = created);
        }
    }

    public static final class HoldHandler
            implements ZLinkSpotRequestHandler<Target, Hold, String>, AutoCloseable {
        public CompletionStage<String> handle(Target spot, Hold request) {
            probe.events.add("target-started");
            probe.entered.complete(null);
            return probe.release.thenApply(
                    reply -> {
                        probe.events.add("target-ended");
                        return reply;
                    });
        }

        public void close() {
            assertEquals("target-closing", probe.events.getLast());
            probe.events.add("target-disposed");
            probe.disposed.complete(null);
        }
    }

    public static final class RequestTimer implements ZLinkSpotTimerHandler<Source> {
        public CompletionStage<Void> handle(Source spot, ZLinkTimerTick tick) {
            probe.timerCancelled = spot.timer.cancel();
            return spot.context()
                    .outbound()
                    .requestToSpot(probe.target, new Hold("request"))
                    .submit(String.class)
                    .handle(
                            (reply, error) -> {
                                probe.terminals.incrementAndGet();
                                probe.completed.complete(null);
                                return null;
                            });
        }
    }
}
