package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import io.micrometer.core.instrument.simple.SimpleMeterRegistry;
import java.time.Duration;
import java.util.Map;
import org.junit.jupiter.api.Test;

final class ZLinkMicrometerMetricSinkTest {
    @Test
    void recordsCatalogKindsAndRejectsHighCardinalityTags() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        sink.increment("zlink.stream.connections.opened", Map.of());
        sink.add("zlink.stream.connections.active", 1, Map.of());
        sink.record("zlink.mesh_node.request.duration", Duration.ofMillis(50), Map.of());

        assertEquals(1.0, registry.get("zlink.stream.connections.opened").counter().count());
        assertEquals(1.0, registry.get("zlink.stream.connections.active").gauge().value());
        assertEquals(1, registry.get("zlink.mesh_node.request.duration").timer().count());
        assertThrows(IllegalArgumentException.class,
            () -> sink.increment("zlink.channel.messages.dropped", Map.of("flow_id", "x")));
    }
}
