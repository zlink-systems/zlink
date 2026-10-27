package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotPopulationMetricTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void entryPopulationIsOneAfterStartupIncludingLateSinkInstallation(boolean lateSink)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("spot-metrics")
                .listen("inproc://spot-metrics-" + UUID.randomUUID())
                .objects()
                .server()
                .addEntrySpot(Entry.class);
        var tags = Map.of("mesh_name", "spot-metrics", "spot_kind", "entry");
        var metrics = lateSink ? null : ZLinkRuntimeMetricProbe.install();
        try {
            try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                        .toCompletableFuture()
                        .join();
                if (lateSink) {
                    metrics = ZLinkRuntimeMetricProbe.install();
                }
                assertEquals(1, metrics.observable("zlink.spot.count", tags));
            }
            assertEquals(0, metrics.observable("zlink.spot.count", tags));
        } finally {
            if (metrics != null) {
                metrics.close();
            }
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<ZLinkActor> {
        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
