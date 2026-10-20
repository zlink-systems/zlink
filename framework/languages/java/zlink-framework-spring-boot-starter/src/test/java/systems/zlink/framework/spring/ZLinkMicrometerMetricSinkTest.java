package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.micrometer.core.instrument.simple.SimpleMeterRegistry;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkCoreHwmProfile;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueueStatus;
import systems.zlink.framework.monitoring.ZLinkCoreHwmStatus;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;
import systems.zlink.framework.runtime.internal.metrics.ZLinkApplicationJobQueuePressureMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkMicrometerMetricSinkTest {
    @Test
    void registersEverySharedInstrumentWithItsKindUnitAndLabelKeys() throws Exception {
        com.fasterxml.jackson.databind.JsonNode fixture;
        try (var input = getClass().getResourceAsStream("/runtime-metrics-v1.json")) {
            org.junit.jupiter.api.Assertions.assertNotNull(input);
            fixture = new com.fasterxml.jackson.databind.ObjectMapper().readTree(input);
        }
        var registry = new SimpleMeterRegistry();
        try {
            var sink = new ZLinkMicrometerMetricSink(registry);
            var expectedNames = new java.util.HashSet<String>();
            for (var definition : fixture.path("metrics")) {
                String name = definition.path("name").asText();
                String kind = definition.path("kind").asText();
                String unit = definition.path("unit").asText();
                expectedNames.add(name);
                var tags = new java.util.HashMap<String, String>();
                definition
                        .path("labels")
                        .forEach(
                                label -> {
                                    var values =
                                            definition.path("labelValues").path(label.asText());
                                    tags.put(
                                            label.asText(),
                                            values.isArray()
                                                    ? values.get(0).asText()
                                                    : "registered");
                                });
                switch (kind) {
                    case "counter" -> sink.increment(name, tags);
                    case "updown" -> sink.add(name, 1, tags);
                    case "histogram" -> sink.record(name, 1d, tags);
                    case "observable" ->
                            sink.registerObservable(
                                    ZLinkRuntimeMetrics.Metric.decode(name), tags, () -> 1L);
                    default -> throw new AssertionError(kind);
                }
                var meter = registry.get(name).meter();
                assertEquals(unit, meter.getId().getBaseUnit(), name);
                assertEquals(
                        tags.keySet(),
                        meter.getId().getTags().stream()
                                .map(io.micrometer.core.instrument.Tag::getKey)
                                .collect(java.util.stream.Collectors.toSet()),
                        name);
                assertEquals(
                        switch (kind) {
                            case "counter" -> io.micrometer.core.instrument.Meter.Type.COUNTER;
                            case "histogram" ->
                                    io.micrometer.core.instrument.Meter.Type.DISTRIBUTION_SUMMARY;
                            default -> io.micrometer.core.instrument.Meter.Type.GAUGE;
                        },
                        meter.getId().getType(),
                        name);
            }
            assertEquals(
                    expectedNames,
                    registry.getMeters().stream()
                            .map(meter -> meter.getId().getName())
                            .collect(java.util.stream.Collectors.toSet()));
        } finally {
            registry.close();
        }
    }

    @Test
    void hostStateReplacesThePreviousSeriesAndByteHistogramsKeepByteUnits() {
        var registry = new SimpleMeterRegistry();
        try {
            var sink = new ZLinkMicrometerMetricSink(registry);
            sink.observeHostState("serving");
            sink.observeHostState("draining");
            sink.observeHostState("stopped");
            assertEquals(1, registry.find("zlink.host.state").gauges().size());
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "stopped").gauge().value());
            sink.record(
                    "zlink.relocation.bytes",
                    128d,
                    Map.of("mesh_name", "mesh", "object_kind", "actor", "policy", "snapshot"));
            assertEquals(
                    "By", registry.get("zlink.relocation.bytes").summary().getId().getBaseUnit());
            assertEquals(128d, registry.get("zlink.relocation.bytes").summary().totalAmount());
        } finally {
            registry.close();
        }
    }

    @Test
    void retainsRequestAndMessageCounterUnits() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        sink.registerRequestInflight(Map.of("mesh_name", "mesh", "surface", "actor"), () -> 2);
        sink.increment("zlink.mesh_node.request.timeouts", Map.of());
        sink.increment("zlink.mesh_node.messages.dropped", Map.of());
        sink.increment("zlink.mesh_node.channel.selection_failures", Map.of());
        assertEquals(
                "{request}",
                registry.get("zlink.mesh_node.requests.inflight").gauge().getId().getBaseUnit());
        assertEquals(
                "{request}",
                registry.get("zlink.mesh_node.request.timeouts").counter().getId().getBaseUnit());
        assertEquals(
                "{message}",
                registry.get("zlink.mesh_node.messages.dropped").counter().getId().getBaseUnit());
        assertEquals(
                "{failure}",
                registry.get("zlink.mesh_node.channel.selection_failures")
                        .counter()
                        .getId()
                        .getBaseUnit());
    }

    @Test
    void recordsCatalogKindsAndRejectsHighCardinalityTags() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        sink.increment("zlink.stream.connections.opened", Map.of());
        sink.add("zlink.stream.connections.active", 1, Map.of());
        sink.record("zlink.mesh_node.request.duration", Duration.ofMillis(50), Map.of());
        sink.record("application.custom.summary", 0.25, Map.of());

        assertEquals(1.0, registry.get("zlink.stream.connections.opened").counter().count());
        assertEquals(1.0, registry.get("zlink.stream.connections.active").gauge().value());
        assertEquals(1, registry.get("zlink.mesh_node.request.duration").summary().count());
        assertEquals(0.25, registry.get("application.custom.summary").summary().totalAmount());
        assertThrows(
                IllegalArgumentException.class,
                () -> sink.increment("zlink.channel.messages.dropped", Map.of("flow_id", "x")));
    }

    @Test
    void recordsRequestDurationSecondsWithTheSpecifiedHistogramUnit() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);

        sink.record("zlink.mesh_node.request.duration", 0.25, Map.of());

        assertEquals(1, registry.get("zlink.mesh_node.request.duration").summary().count());
        assertEquals(
                0.25,
                registry.get("zlink.mesh_node.request.duration").summary().totalAmount(),
                0.000001);
    }

    @Test
    void exportsLiveRouteMeshTopologyAndAggregatesMatchingHosts() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        AtomicReference<List<ZLinkRuntimeMetrics.MeshTopologyMetrics>> source =
                new AtomicReference<>(
                        List.of(
                                topology("mesh", "manual_and_redis", 2, 1, 1, 3),
                                topology("mesh", "manual_and_redis", 3, 2, 2, 4)));

        sink.registerMeshTopology(source::get);

        assertEquals(
                5.0,
                registry.get("zlink.mesh_node.peers.configured")
                        .tag("mesh_name", "mesh")
                        .tag("source", "manual_and_redis")
                        .gauge()
                        .value());
        assertEquals(
                3.0,
                registry.get("zlink.mesh_node.peers.connected")
                        .tag("mesh_name", "mesh")
                        .tag("source", "manual_and_redis")
                        .gauge()
                        .value());
        assertEquals(
                3.0,
                registry.get("zlink.mesh_node.peers.ready")
                        .tag("mesh_name", "mesh")
                        .tag("source", "manual_and_redis")
                        .gauge()
                        .value());
        assertEquals(
                7.0,
                registry.get("zlink.mesh_node.channels.ready_members")
                        .tag("mesh_name", "mesh")
                        .tag("channel_name", "orders")
                        .gauge()
                        .value());
        assertEquals(
                "{peer}",
                registry.get("zlink.mesh_node.peers.configured").gauge().getId().getBaseUnit());
        assertEquals(
                "{member}",
                registry.get("zlink.mesh_node.channels.ready_members")
                        .gauge()
                        .getId()
                        .getBaseUnit());

        source.set(List.of(topology("mesh", "manual_and_redis", 1, 0, 0, 0)));
        assertEquals(
                1.0,
                registry.get("zlink.mesh_node.peers.configured")
                        .tag("mesh_name", "mesh")
                        .tag("source", "manual_and_redis")
                        .gauge()
                        .value());
        assertEquals(
                0.0,
                registry.get("zlink.mesh_node.channels.ready_members")
                        .tag("mesh_name", "mesh")
                        .tag("channel_name", "orders")
                        .gauge()
                        .value());
    }

    @Test
    void exportsExactHostCapacityCatalogFromTheSingleStatusProjection() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        AtomicReference<ZLinkHostCapacityStatus> source =
                new AtomicReference<>(capacity(0, 5, Duration.ofMillis(2500)));

        sink.registerHostCapacity(source::get);

        assertEquals(
                Set.of(
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
        assertEquals(1024.0, registry.get("zlink.host.core_hwm.effective_budget").gauge().value());
        assertEquals(128.0, registry.get("zlink.host.application_job_queue.limit").gauge().value());
        assertEquals(
                2.0,
                registry.get("zlink.host.application_job_queue.jobs")
                        .tag("state", "queued")
                        .gauge()
                        .value());
        assertEquals(
                Set.of("reserved", "queued", "in_use", "peak"),
                registry.getMeters().stream()
                        .filter(
                                meter ->
                                        meter.getId()
                                                .getName()
                                                .equals("zlink.host.application_job_queue.jobs"))
                        .map(meter -> meter.getId().getTag("state"))
                        .collect(java.util.stream.Collectors.toSet()));
        assertEquals(
                "By",
                registry.get("zlink.host.core_hwm.effective_budget").gauge().getId().getBaseUnit());
        assertEquals(
                "{job}",
                registry.get("zlink.host.application_job_queue.limit")
                        .gauge()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "{waiter}",
                registry.get("zlink.host.application_job_queue.capacity_waiters")
                        .gauge()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                5.0,
                registry.get("zlink.host.application_job_queue.capacity_waits")
                        .functionCounter()
                        .count());
        assertEquals(
                2.5,
                registry.get("zlink.host.application_job_queue.capacity_wait_duration")
                        .functionCounter()
                        .count(),
                0.000001);

        assertEquals(
                "{ppm}",
                registry.get("zlink.host.core_hwm.blocked_ratio").gauge().getId().getBaseUnit());
        assertEquals(
                "{wait}",
                registry.get("zlink.host.application_job_queue.capacity_waits")
                        .functionCounter()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "s",
                registry.get("zlink.host.application_job_queue.capacity_wait_duration")
                        .functionCounter()
                        .getId()
                        .getBaseUnit());
        source.set(capacity(1, 0, Duration.ZERO));
        assertEquals(
                0.0,
                registry.get("zlink.host.application_job_queue.capacity_waits")
                        .functionCounter()
                        .count());
        source.set(capacity(1, 3, Duration.ofSeconds(1)));
        assertEquals(
                3.0,
                registry.get("zlink.host.application_job_queue.capacity_waits")
                        .functionCounter()
                        .count());
        assertEquals(
                1.0,
                registry.get("zlink.host.application_job_queue.capacity_wait_duration")
                        .functionCounter()
                        .count(),
                0.000001);
        assertTrue(
                registry.getMeters().stream()
                        .filter(meter -> meter.getId().getName().startsWith("zlink.host."))
                        .allMatch(
                                meter ->
                                        meter.getId().getTags().stream()
                                                .allMatch(tag -> tag.getKey().equals("state"))));
    }

    @Test
    void exportsMetricOnlyApplicationJobQueuePressureAccounting() {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        ZLinkMicrometerMetricSink sink = new ZLinkMicrometerMetricSink(registry);
        AtomicReference<ZLinkApplicationJobQueuePressureMetrics> source =
                new AtomicReference<>(
                        new ZLinkApplicationJobQueuePressureMetrics(
                                ZLinkApplicationJobQueuePressureState.RUNNING,
                                2,
                                3,
                                Duration.ofSeconds(4),
                                Duration.ofSeconds(5),
                                6));

        sink.registerApplicationJobQueuePressure(source::get);

        assertEquals(
                Set.of(
                        "zlink.host.application_job_queue.pressure_state",
                        "zlink.host.application_job_queue.pressure_transitions",
                        "zlink.host.application_job_queue.pause_duration",
                        "zlink.host.application_job_queue.flow_state_config_failures"),
                registry.getMeters().stream()
                        .map(meter -> meter.getId().getName())
                        .collect(java.util.stream.Collectors.toSet()));
        assertEquals(
                1.0,
                registry.get("zlink.host.application_job_queue.pressure_state")
                        .tag("state", "running")
                        .gauge()
                        .value());
        assertEquals(
                "s",
                registry.get("zlink.host.application_job_queue.pause_duration")
                        .tag("state", "current")
                        .gauge()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "s",
                registry.get("zlink.host.application_job_queue.pause_duration")
                        .tag("state", "cumulative")
                        .gauge()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "{failure}",
                registry.get("zlink.host.application_job_queue.flow_state_config_failures")
                        .functionCounter()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "{state}",
                registry.get("zlink.host.application_job_queue.pressure_state")
                        .gauge()
                        .getId()
                        .getBaseUnit());
        assertEquals(
                "{transition}",
                registry.get("zlink.host.application_job_queue.pressure_transitions")
                        .tag("state", "running")
                        .functionCounter()
                        .getId()
                        .getBaseUnit());
        assertEquals(Set.of("running"), pressureStateSeries(registry));
        assertEquals(
                2.0,
                registry.get("zlink.host.application_job_queue.pressure_transitions")
                        .tag("state", "running")
                        .functionCounter()
                        .count());
        assertEquals(
                3.0,
                registry.get("zlink.host.application_job_queue.pressure_transitions")
                        .tag("state", "paused")
                        .functionCounter()
                        .count());
        assertEquals(
                5.0,
                registry.get("zlink.host.application_job_queue.pause_duration")
                        .tag("state", "cumulative")
                        .gauge()
                        .value());
        assertEquals(
                6.0,
                registry.get("zlink.host.application_job_queue.flow_state_config_failures")
                        .functionCounter()
                        .count());

        ZLinkApplicationJobQueuePressureMetrics paused =
                new ZLinkApplicationJobQueuePressureMetrics(
                        ZLinkApplicationJobQueuePressureState.PAUSED,
                        0,
                        0,
                        Duration.ofSeconds(7),
                        Duration.ZERO,
                        0);
        source.set(paused);
        sink.observeApplicationJobQueuePressure(paused);
        assertEquals(
                1.0,
                registry.get("zlink.host.application_job_queue.pressure_state")
                        .tag("state", "paused")
                        .gauge()
                        .value());
        assertEquals(Set.of("paused"), pressureStateSeries(registry));
        assertEquals(
                0.0,
                registry.get("zlink.host.application_job_queue.pressure_transitions")
                        .tag("state", "running")
                        .functionCounter()
                        .count());
        assertEquals(
                0.0,
                registry.get("zlink.host.application_job_queue.flow_state_config_failures")
                        .functionCounter()
                        .count());

        source.set(
                new ZLinkApplicationJobQueuePressureMetrics(
                        ZLinkApplicationJobQueuePressureState.RUNNING,
                        1,
                        1,
                        Duration.ZERO,
                        Duration.ofSeconds(1),
                        0));
        sink.observeApplicationJobQueuePressure(source.get());
        assertEquals(Set.of("running"), pressureStateSeries(registry));
    }

    private static Set<String> pressureStateSeries(SimpleMeterRegistry registry) {
        return registry.getMeters().stream()
                .filter(
                        meter ->
                                meter.getId()
                                        .getName()
                                        .equals("zlink.host.application_job_queue.pressure_state"))
                .map(meter -> meter.getId().getTag("state"))
                .collect(java.util.stream.Collectors.toSet());
    }

    private static ZLinkRuntimeMetrics.MeshTopologyMetrics topology(
            String meshName,
            String source,
            long configured,
            long connected,
            long ready,
            long readyMembers) {
        return new ZLinkRuntimeMetrics.MeshTopologyMetrics(
                meshName,
                source,
                configured,
                connected,
                ready,
                List.of(
                        new ZLinkRuntimeMetrics.MeshChannelTopologyMetrics(
                                "orders", readyMembers)));
    }

    private static ZLinkHostCapacityStatus capacity(long epoch, long waits, Duration waitDuration) {
        return new ZLinkHostCapacityStatus(
                epoch,
                new ZLinkCoreHwmStatus(
                        Optional.empty(),
                        Optional.empty(),
                        ZLinkCoreHwmProfile.BALANCED,
                        1024,
                        900,
                        100,
                        200,
                        300,
                        0,
                        400,
                        40,
                        50,
                        1,
                        340,
                        0,
                        0,
                        0,
                        0,
                        13,
                        1,
                        1,
                        1,
                        1,
                        0,
                        0,
                        0),
                new ZLinkApplicationJobQueueStatus(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        Optional.empty(),
                        80,
                        60,
                        1,
                        128,
                        103,
                        76,
                        1,
                        2,
                        3,
                        4,
                        ZLinkApplicationJobQueuePressureState.RUNNING,
                        Duration.ZERO,
                        1,
                        waits,
                        waitDuration));
    }
}
