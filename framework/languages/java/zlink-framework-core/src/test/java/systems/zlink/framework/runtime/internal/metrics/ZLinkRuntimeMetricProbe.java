package systems.zlink.framework.runtime.internal.metrics;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.LongSupplier;

public final class ZLinkRuntimeMetricProbe implements ZLinkRuntimeMetrics.Sink, AutoCloseable {
    public record Sample(String name, double value, Map<String, String> tags) {}

    public final List<Sample> samples = new CopyOnWriteArrayList<>();
    public final List<String> hostStates = new CopyOnWriteArrayList<>();

    private record Key(String name, Map<String, String> tags) {}

    private final Map<Key, LongSupplier> observables = new ConcurrentHashMap<>();
    private AutoCloseable registration;

    public static ZLinkRuntimeMetricProbe install() {
        var probe = new ZLinkRuntimeMetricProbe();
        probe.registration = ZLinkRuntimeMetrics.install(probe);
        return probe;
    }

    private java.util.function.Supplier<Map<String, Long>> hostStateSource = Map::of;

    @Override
    public void observeHostStates(java.util.function.Supplier<Map<String, Long>> source) {
        hostStateSource = source;
        source.get().keySet().forEach(this::observeHostState);
    }

    public Map<String, Long> collectHostStates() {
        return hostStateSource.get();
    }

    @Override
    public void observeHostState(String state) {
        hostStates.add(state);
    }

    @Override
    public void increment(String name, Map<String, String> tags) {
        samples.add(new Sample(name, 1, tags));
    }

    @Override
    public void add(String name, long delta, Map<String, String> tags) {
        samples.add(new Sample(name, delta, tags));
    }

    @Override
    public void record(String name, double value, Map<String, String> tags) {
        samples.add(new Sample(name, value, tags));
    }

    @Override
    public void record(String name, java.time.Duration value, Map<String, String> tags) {
        record(name, value.toNanos() / 1_000_000_000d, tags);
    }

    @Override
    public void registerObservable(
            ZLinkRuntimeMetrics.Metric metric, Map<String, String> tags, LongSupplier value) {
        observables.put(new Key(metric.metricName(), tags), value);
    }

    public boolean hasObservable(String name, Map<String, String> tags) {
        return observables.containsKey(new Key(name, tags));
    }

    public long observable(String name, Map<String, String> tags) {
        return observables.get(new Key(name, tags)).getAsLong();
    }

    public long count(String name) {
        return samples.stream().filter(sample -> sample.name().equals(name)).count();
    }

    public Sample only(String name) {
        var matching = samples.stream().filter(sample -> sample.name().equals(name)).toList();
        org.junit.jupiter.api.Assertions.assertEquals(1, matching.size(), name);
        return matching.get(0);
    }

    public void assertContractLabels() {
        for (var sample : samples) {
            var metric = ZLinkRuntimeMetrics.Metric.decode(sample.name());
            org.junit.jupiter.api.Assertions.assertNotNull(metric, sample.name());
            var expected = new java.util.HashSet<>(metric.labels());
            expected.remove("execution_mode");
            var actual = new java.util.HashSet<>(sample.tags().keySet());
            actual.remove("execution_mode");
            org.junit.jupiter.api.Assertions.assertEquals(expected, actual, sample.name());
        }
    }

    @Override
    public void close() throws Exception {
        registration.close();
    }
}
