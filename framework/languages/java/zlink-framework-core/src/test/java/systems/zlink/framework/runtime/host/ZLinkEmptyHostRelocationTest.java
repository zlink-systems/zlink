package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

/**
 * Host relocation with nothing to move (spec 05-host-relocation-flow §5.1, #1947): the host
 * finishes {@code Relocated/None} without target discovery, while a host that owns a unit still
 * needs a target.
 */
final class ZLinkEmptyHostRelocationTest {
    private static final String MESH = "empty-host";
    private static final Duration WAIT = Duration.ofSeconds(15);

    @Test
    void hostWithOnlyMeshListenersRelocatesWithoutATarget() throws Exception {
        try (var metrics =
                        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe
                                .install();
                var runtime = ZLinkFrameworkRuntimeTestAccess.start(options("solo", false))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());

            // No other node exists in the Mesh, so any target requirement can only end Blocked.
            // The deadline is the full wait: Relocated must not depend on it expiring.
            var result =
                    runtime.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            WAIT))
                            .toCompletableFuture()
                            .get(WAIT.toSeconds() * 2, TimeUnit.SECONDS);

            assertEquals(
                    ZLinkFrameworkRelocationOutcome.RELOCATED, result.outcome(), result::toString);
            assertEquals(ZLinkFrameworkRelocationReason.NONE, result.reason(), result::toString);
            assertEquals(ZLinkFrameworkRuntimeState.RELOCATED, runtime.status().state());
            assertEquals(1, metrics.count("zlink.host.relocation.duration"));
            assertEquals(
                    "relocated",
                    metrics.only("zlink.host.relocation.duration").tags().get("outcome"));
            runtime.shutdown(WAIT).toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertEquals(1, metrics.count("zlink.host.shutdown.duration"));
            assertEquals(
                    "stopped", metrics.only("zlink.host.shutdown.duration").tags().get("outcome"));
            assertEquals(
                    java.util.List.of(
                            "preparing",
                            "serving",
                            "relocating",
                            "relocated",
                            "draining",
                            "stopped"),
                    metrics.hostStates.stream().distinct().toList());
            metrics.assertContractLabels();
        }
    }

    @Test
    void hostWithAUnitAndNoOtherServingNodeStaysBlocked() throws Exception {
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options("owner", true))) {
            runtime.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);

            var result =
                    runtime.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            Duration.ofMillis(300)))
                            .toCompletableFuture()
                            .get(WAIT.toSeconds(), TimeUnit.SECONDS);

            assertEquals(
                    ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome(), result::toString);
            assertTrue(
                    result.reason() == ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE
                            || result.reason() == ZLinkFrameworkRelocationReason.DEADLINE_EXCEEDED,
                    result::toString);
            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
        }
    }

    private static DefaultZLinkFrameworkOptions options(String rid, boolean withSpots) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new InMemoryRelocationStore());
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (withSpots) {
            node.objects()
                    .server()
                    .addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
        }
        return options;
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {}
    }
}
