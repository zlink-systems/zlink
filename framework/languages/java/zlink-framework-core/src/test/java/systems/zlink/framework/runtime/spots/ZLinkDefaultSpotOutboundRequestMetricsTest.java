package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRequestMetricProbe;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkDefaultSpotOutboundRequestMetricsTest {
    @Test
    void cancellationRecordsSourceMeshSpotRequestOnce() throws Exception {
        CompletableFuture<Optional<SpotTransportAddress>> resolution = new CompletableFuture<>();
        SpotTransportAddressResolver resolver = ignored -> resolution;
        DefaultSpotOutbound outbound = new DefaultSpotOutbound(
            null,
            "source-mesh",
            null,
            new ZLinkJsonMessageSerializer(),
            ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
            null,
            null,
            null,
            null,
            false,
            Duration.ofSeconds(1),
            () -> resolver,
            null);

        try (ZLinkRequestMetricProbe metrics = ZLinkRequestMetricProbe.install()) {
            var result = outbound.requestToSpot("missing", new Request("hello"))
                .submit(Reply.class).toCompletableFuture();
            assertEquals(1L, metrics.inflight("source-mesh", "spot"));
            assertTrue(result.cancel(false));
            assertEquals(0L, metrics.inflight("source-mesh", "spot"));
            assertEquals(1L, metrics.durationCount(
                "source-mesh", "spot", "failed"));

            resolution.complete(Optional.empty());

            assertEquals(0L, metrics.inflight("source-mesh", "spot"));
            assertEquals(1L, metrics.durationCount(
                "source-mesh", "spot", "failed"));
        }
    }

    private record Request(String value) { }

    private record Reply(String value) { }
}
