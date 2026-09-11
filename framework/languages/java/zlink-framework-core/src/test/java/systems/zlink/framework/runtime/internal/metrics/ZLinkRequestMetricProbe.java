package systems.zlink.framework.runtime.internal.metrics;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.LongSupplier;

/** Recording metric sink for request-surface contract tests. */
public final class ZLinkRequestMetricProbe implements AutoCloseable,
    ZLinkRuntimeMetrics.Sink {
    private final Map<Map<String, String>, LongSupplier> inflight =
        new ConcurrentHashMap<>();
    private final List<Event> events = new CopyOnWriteArrayList<>();
    private final AutoCloseable registration;

    private ZLinkRequestMetricProbe() {
        registration = ZLinkRuntimeMetrics.install(this);
    }

    public static ZLinkRequestMetricProbe install() {
        return new ZLinkRequestMetricProbe();
    }

    @Override
    public void registerRequestInflight(
        Map<String, String> tags,
        LongSupplier value) {
        inflight.put(Map.copyOf(tags), value);
    }

    @Override
    public void increment(String name, Map<String, String> tags) {
        events.add(new Event(name, Map.copyOf(tags), null));
    }

    @Override
    public void record(
        String name,
        Duration duration,
        Map<String, String> tags) {
        events.add(new Event(name, Map.copyOf(tags), duration));
    }

    @Override
    public void record(
        String name,
        double value,
        Map<String, String> tags) {
        events.add(new Event(name, Map.copyOf(tags), value));
    }

    public long inflight(String meshName, String surface) {
        LongSupplier value = inflight.get(
            Map.of("mesh_name", meshName, "surface", surface));
        return value == null ? 0L : value.getAsLong();
    }

    public long durationCount(
        String meshName,
        String surface,
        String outcome) {
        return count(
            "zlink.mesh_node.request.duration",
            Map.of(
                "mesh_name", meshName,
                "surface", surface,
                "outcome", outcome));
    }

    public long timeoutCount(String meshName, String surface) {
        return count(
            "zlink.mesh_node.request.timeouts",
            Map.of("mesh_name", meshName, "surface", surface));
    }

    private long count(String name, Map<String, String> tags) {
        return events.stream()
            .filter(event -> event.name().equals(name) && event.tags().equals(tags))
            .count();
    }

    @Override
    public void close() throws Exception {
        registration.close();
    }

    private record Event(
        String name,
        Map<String, String> tags,
        Object value) { }
}
