package systems.zlink.framework.spring;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.DistributionSummary;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.FunctionCounter;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Tags;
import io.micrometer.core.instrument.Timer;
import java.time.Duration;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;
import java.util.function.ToDoubleFunction;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;
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

    @Override
    public void registerHostCapacity(
        Supplier<ZLinkHostCapacityStatus> source) {
        registerGauge(
            "zlink.host.core_hwm.effective_budget", "By", Map.of(), source,
            status -> status.coreHwm().effectiveBudgetBytes());
        registerGauge(
            "zlink.host.core_hwm.applied", "By", Map.of(), source,
            status -> status.coreHwm().totalAppliedHwmBytes());
        registerGauge(
            "zlink.host.core_hwm.accounted", "By", Map.of("state", "current"), source,
            status -> status.coreHwm().currentAccountedBytes());
        registerGauge(
            "zlink.host.core_hwm.accounted", "By", Map.of("state", "peak"), source,
            status -> status.coreHwm().peakAccountedBytes());
        registerGauge(
            "zlink.host.core_hwm.completion_accounted", "By",
            Map.of("state", "current"), source,
            status -> status.coreHwm().completionCurrentAccountedBytes());
        registerGauge(
            "zlink.host.core_hwm.completion_accounted", "By",
            Map.of("state", "peak"), source,
            status -> status.coreHwm().completionPeakAccountedBytes());
        registerGauge(
            "zlink.host.core_hwm.blocked_ratio", "{ppm}", Map.of(), source,
            status -> status.coreHwm().blockedRatioPpm());
        registerGauge(
            "zlink.host.application_job_queue.limit", "{job}", Map.of(), source,
            status -> status.applicationJobQueue()
                .effectiveMaxQueuedApplicationJobs());
        registerGauge(
            "zlink.host.application_job_queue.jobs", "{job}",
            Map.of("state", "reserved"), source,
            status -> status.applicationJobQueue().reservedSupplyPermits());
        registerGauge(
            "zlink.host.application_job_queue.jobs", "{job}",
            Map.of("state", "queued"), source,
            status -> status.applicationJobQueue().queuedApplicationJobs());
        registerGauge(
            "zlink.host.application_job_queue.jobs", "{job}",
            Map.of("state", "in_use"), source,
            status -> status.applicationJobQueue().permitsInUse());
        registerGauge(
            "zlink.host.application_job_queue.jobs", "{job}",
            Map.of("state", "peak"), source,
            status -> status.applicationJobQueue().peakPermitsInUse());
        registerGauge(
            "zlink.host.application_job_queue.capacity_waiters", "{waiter}",
            Map.of(), source,
            status -> status.applicationJobQueue().capacityWaiters());
        FunctionCounter.builder(
                "zlink.host.application_job_queue.capacity_waits",
                source,
                value -> valueOrZero(value,
                    status -> status.applicationJobQueue().capacityWaitCount()))
            .baseUnit("{wait}")
            .register(registry);
        FunctionCounter.builder(
                "zlink.host.application_job_queue.capacity_wait_duration",
                source,
                value -> valueOrZero(value,
                    status -> status.applicationJobQueue()
                        .capacityWaitDuration().toNanos() / 1_000_000_000.0))
            .baseUnit("s")
            .register(registry);
    }

    private void registerGauge(
        String name,
        String baseUnit,
        Map<String, String> tags,
        Supplier<ZLinkHostCapacityStatus> source,
        ToDoubleFunction<ZLinkHostCapacityStatus> value) {
        Gauge.builder(name, source,
                current -> valueOrZero(current, value))
            .baseUnit(baseUnit)
            .tags(toTags(tags))
            .register(registry);
    }

    private static double valueOrZero(
        Supplier<ZLinkHostCapacityStatus> source,
        ToDoubleFunction<ZLinkHostCapacityStatus> value) {
        ZLinkHostCapacityStatus status = source.get();
        return status == null ? 0.0 : value.applyAsDouble(status);
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
