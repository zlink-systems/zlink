package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.micrometer.core.instrument.simple.SimpleMeterRegistry;
import java.time.Duration;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;
import java.util.Map;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueueStatus;
import systems.zlink.framework.monitoring.ZLinkCoreHwmStatus;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;

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

    @Test
    void exportsExactHostCapacityCatalogFromTheSingleStatusProjection() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        AtomicReference<ZLinkHostCapacityStatus> source =
            new AtomicReference<>(capacity(0, 5, Duration.ofMillis(2500)));

        sink.registerHostCapacity(source::get);

        assertEquals(Set.of(
            "zlink.host.core_hwm.effective_budget",
            "zlink.host.core_hwm.applied",
            "zlink.host.core_hwm.accounted",
            "zlink.host.core_hwm.completion_accounted",
            "zlink.host.core_hwm.blocked_ratio",
            "zlink.host.application_job_queue.limit",
            "zlink.host.application_job_queue.jobs",
            "zlink.host.application_job_queue.capacity_waiters",
            "zlink.host.application_job_queue.capacity_waits",
            "zlink.host.application_job_queue.capacity_wait_duration"),
            registry.getMeters().stream()
                .map(meter -> meter.getId().getName())
                .filter(name -> name.startsWith("zlink.host."))
                .collect(java.util.stream.Collectors.toSet()));
        assertEquals(1024.0,
            registry.get("zlink.host.core_hwm.effective_budget").gauge().value());
        assertEquals(128.0,
            registry.get("zlink.host.application_job_queue.limit").gauge().value());
        assertEquals(2.0, registry.get("zlink.host.application_job_queue.jobs")
            .tag("state", "queued").gauge().value());
        assertEquals(Set.of("reserved", "queued", "in_use", "peak"),
            registry.getMeters().stream()
                .filter(meter -> meter.getId().getName()
                    .equals("zlink.host.application_job_queue.jobs"))
                .map(meter -> meter.getId().getTag("state"))
                .collect(java.util.stream.Collectors.toSet()));
        assertEquals("By",
            registry.get("zlink.host.core_hwm.effective_budget")
                .gauge().getId().getBaseUnit());
        assertEquals("{job}",
            registry.get("zlink.host.application_job_queue.limit")
                .gauge().getId().getBaseUnit());
        assertEquals("{waiter}",
            registry.get("zlink.host.application_job_queue.capacity_waiters")
                .gauge().getId().getBaseUnit());
        assertEquals(5.0,
            registry.get("zlink.host.application_job_queue.capacity_waits")
                .functionCounter().count());
        assertEquals(2.5,
            registry.get("zlink.host.application_job_queue.capacity_wait_duration")
                .functionCounter().count(), 0.000001);

        source.set(capacity(1, 0, Duration.ZERO));
        assertEquals(0.0,
            registry.get("zlink.host.application_job_queue.capacity_waits")
                .functionCounter().count());
        source.set(capacity(1, 3, Duration.ofSeconds(1)));
        assertEquals(3.0,
            registry.get("zlink.host.application_job_queue.capacity_waits")
                .functionCounter().count());
        assertEquals(1.0,
            registry.get("zlink.host.application_job_queue.capacity_wait_duration")
                .functionCounter().count(), 0.000001);
        assertTrue(registry.getMeters().stream()
            .filter(meter -> meter.getId().getName().startsWith("zlink.host."))
            .allMatch(meter -> meter.getId().getTags().stream()
                .allMatch(tag -> tag.getKey().equals("state"))));
    }

    private static ZLinkHostCapacityStatus capacity(
        long epoch,
        long waits,
        Duration waitDuration) {
        return new ZLinkHostCapacityStatus(
            epoch,
            new ZLinkCoreHwmStatus(
                Optional.empty(), Optional.empty(), ZLinkCoreHwmProfile.BALANCED,
                1024, 900, 100, 200, 300, 0, 400,
                40, 50, 1, 340, 0, 0, 0, 0, 13,
                1, 1, 1, 1, 0, 0, 0),
            new ZLinkApplicationJobQueueStatus(
                ZLinkApplicationJobQueueProfile.BALANCED,
                Optional.empty(), 1, 128, 1, 2, 3, 4, 1,
                waits, waitDuration));
    }
}
