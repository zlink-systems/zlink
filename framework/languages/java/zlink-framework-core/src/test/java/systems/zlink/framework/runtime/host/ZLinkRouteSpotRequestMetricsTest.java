package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.UUID;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRequestMetricProbe;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

final class ZLinkRouteSpotRequestMetricsTest {
    @Test
    void modernRouteMeshProvidesSourceMeshForPublicSpotRequest()
        throws Exception {
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("modern-mesh")
            .listen("inproc://modern-mesh-" + UUID.randomUUID())
            .channelName("requests")
            .client();

        try (ZLinkRequestMetricProbe metrics = ZLinkRequestMetricProbe.install();
             ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
                 options,
                 new ZLinkJavaBackendAdapterFactory())) {
            assertThrows(
                CompletionException.class,
                () -> runtime.route()
                    .requestToSpot("missing", new Request("hello"))
                    .submit(Reply.class)
                    .toCompletableFuture()
                    .join());

            assertEquals(0L, metrics.inflight("modern-mesh", "spot"));
            assertEquals(1L, metrics.durationCount(
                "modern-mesh", "spot", "failed"));

            assertThrows(
                CompletionException.class,
                () -> runtime.route()
                    .requestToSpot("missing-instance", new Request("hello"))
                    .instanceSpot("room")
                    .inMesh("unknown-mesh")
                    .submit(Reply.class)
                    .toCompletableFuture()
                    .join());

            assertEquals(0L, metrics.inflight(
                "modern-mesh", "instance_spot"));
            assertEquals(1L, metrics.durationCount(
                "modern-mesh", "instance_spot", "failed"));
            assertEquals(0L, metrics.inflight(
                "unknown-mesh", "instance_spot"));
            assertEquals(0L, metrics.durationCount(
                "unknown-mesh", "instance_spot", "failed"));
        }
    }

    private record Request(String value) { }

    private record Reply(String value) { }
}
