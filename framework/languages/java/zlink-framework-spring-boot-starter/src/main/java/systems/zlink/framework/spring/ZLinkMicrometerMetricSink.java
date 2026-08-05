package systems.zlink.framework.spring;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.DistributionSummary;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Tags;
import io.micrometer.core.instrument.Timer;
import java.time.Duration;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

final class ZLinkMicrometerMetricSink implements ZLinkRuntimeMetrics.Sink {
    private static final Set<String> FORBIDDEN_TAGS = Set.of(
        "correlation_id", "flow_id", "actor_id", "spot_id");
    private final MeterRegistry registry;
    private final Map<Key, AtomicLong> gauges = new ConcurrentHashMap<>();

    ZLinkMicrometerMetricSink(MeterRegistry registry) {
        this.registry = registry;
    }

    @Override
    public void increment(String name, Map<String, String> tags) {
        Counter.builder(name).tags(toTags(tags)).register(registry).increment();
    }

    @Override
    public void add(String name, long delta, Map<String, String> tags) {
        Key key = new Key(name, Map.copyOf(tags));
        gauges.computeIfAbsent(key, ignored -> {
            AtomicLong value = new AtomicLong();
            Gauge.builder(name, value, AtomicLong::doubleValue)
                .tags(toTags(tags)).register(registry);
            return value;
        }).addAndGet(delta);
    }

    @Override
    public void record(String name, Duration duration, Map<String, String> tags) {
        Timer.builder(name).tags(toTags(tags)).register(registry)
            .record(duration.toNanos(), TimeUnit.NANOSECONDS);
    }

    @Override
    public void record(String name, double value, Map<String, String> tags) {
        DistributionSummary.builder(name).tags(toTags(tags)).register(registry).record(value);
    }

    private static Tags toTags(Map<String, String> tags) {
        for (String key : tags.keySet()) {
            if (FORBIDDEN_TAGS.contains(key)) {
                throw new IllegalArgumentException("high-cardinality metric tag is forbidden: " + key);
            }
        }
        Tags result = Tags.empty();
        for (Map.Entry<String, String> entry : tags.entrySet()) {
            result = result.and(entry.getKey(), entry.getValue());
        }
        return result;
    }

    private record Key(String name, Map<String, String> tags) { }
}
