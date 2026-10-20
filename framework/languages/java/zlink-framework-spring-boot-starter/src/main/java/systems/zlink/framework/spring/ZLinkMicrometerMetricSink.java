package systems.zlink.framework.spring;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.DistributionSummary;
import io.micrometer.core.instrument.FunctionCounter;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Tags;

import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;
import systems.zlink.framework.runtime.internal.metrics.ZLinkApplicationJobQueuePressureMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Unit;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import java.util.function.ToDoubleFunction;

final class ZLinkMicrometerMetricSink implements ZLinkRuntimeMetrics.Sink {
    private static final String CORE_HWM_ACCOUNTED_NAME =
            ZLinkRuntimeMetrics.Metric.HOST_CORE_HWM_ACCOUNTED.metricName();
    private static final String COMPLETION_ACCOUNTED_NAME =
            ZLinkRuntimeMetrics.Metric.HOST_CORE_HWM_COMPLETION_ACCOUNTED.metricName();
    private static final String APPLICATION_JOBS_NAME =
            ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_JOBS.metricName();
    private static final String PAUSE_DURATION_NAME =
            ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_PAUSE_DURATION.metricName();

    private enum GaugeState {
        CURRENT("current"),
        PEAK("peak"),
        RESERVED("reserved"),
        QUEUED("queued"),
        IN_USE("in_use"),
        CUMULATIVE("cumulative");

        private final String wire;

        GaugeState(String wire) {
            this.wire = wire;
        }

        String wire() {
            return wire;
        }
    }

    private static final Set<String> FORBIDDEN_TAGS =
            Set.of("correlation_id", "flow_id", "actor_id", "spot_id");
    private final MeterRegistry registry;

    private record HostStateSeries(AtomicLong value, io.micrometer.core.instrument.Meter.Id id) {}

    private final ConcurrentHashMap<String, HostStateSeries> hostStates = new ConcurrentHashMap<>();

    @Override
    public void observeHostState(String state) {
        observeHostStates(() -> Map.of(state, 1L));
    }

    @Override
    public void observeHostStates(Supplier<Map<String, Long>> source) {
        Map<String, Long> snapshot = source.get();
        for (var state : systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.values()) {
            String label = ZLinkRuntimeMetrics.wire(state);
            hostStates.compute(
                    label,
                    (key, current) -> {
                        long count = snapshot.getOrDefault(key, 0L);
                        if (count == 0) {
                            if (current != null) registry.remove(current.id());
                            return null;
                        }
                        if (current != null) {
                            current.value().set(count);
                            return current;
                        }
                        AtomicLong value = new AtomicLong(count);
                        var meter =
                                Gauge.builder(
                                                ZLinkRuntimeMetrics.Metric.HOST_STATE.metricName(),
                                                value,
                                                AtomicLong::doubleValue)
                                        .baseUnit(Unit.RUNTIME.wire())
                                        .tag(ZLinkRuntimeMetrics.Tag.STATE.wire(), key)
                                        .register(registry);
                        return new HostStateSeries(value, meter.getId());
                    });
        }
    }

    private final Map<String, Map<Map<String, String>, AtomicLong>> gauges =
            new ConcurrentHashMap<>();
    private final Map<String, Map<Map<String, String>, Counter>> counters =
            new ConcurrentHashMap<>();
    private final Map<String, Map<Map<String, String>, DistributionSummary>> summaries =
            new ConcurrentHashMap<>();
    private final Map<String, Map<Map<String, String>, LongSupplier>> requestInflight =
            new ConcurrentHashMap<>();
    private final Set<Key> meshTopologyGauges = ConcurrentHashMap.newKeySet();
    private final Set<Key> requestInflightGauges = ConcurrentHashMap.newKeySet();
    private final AtomicReference<Supplier<ZLinkHostCapacityStatus>> hostCapacitySource =
            new AtomicReference<>();
    private final AtomicReference<Supplier<List<ZLinkRuntimeMetrics.MeshTopologyMetrics>>>
            meshTopologySource = new AtomicReference<>();
    private final AtomicReference<Supplier<ZLinkApplicationJobQueuePressureMetrics>>
            applicationJobQueuePressureSource = new AtomicReference<>();
    private final AtomicLong applicationJobQueuePressureStateValue = new AtomicLong(1L);
    private io.micrometer.core.instrument.Meter.Id applicationJobQueuePressureStateId;
    private boolean applicationJobQueuePressureRegistered;

    ZLinkMicrometerMetricSink(MeterRegistry registry) {
        this.registry = registry;
    }

    @Override
    public void increment(String name, Map<String, String> tags) {
        counter(name, tags).increment();
    }

    @Override
    public void add(String name, long delta, Map<String, String> tags) {
        AtomicLong value = gaugeValue(name, tags);
        value.addAndGet(delta);
    }

    @Override
    public void record(String name, Duration duration, Map<String, String> tags) {
        record(name, duration.toNanos() / (double) TimeUnit.SECONDS.toNanos(1), tags);
    }

    @Override
    public void record(String name, double value, Map<String, String> tags) {
        ZLinkRuntimeMetrics.Metric metric = ZLinkRuntimeMetrics.Metric.decode(name);
        summary(name, tags, metric == null ? null : metric.unit().wire()).record(value);
    }

    @Override
    public void registerMeshTopology(
            Supplier<List<ZLinkRuntimeMetrics.MeshTopologyMetrics>> source) {
        meshTopologySource.set(source);
        for (ZLinkRuntimeMetrics.MeshTopologyMetrics snapshot : source.get()) {
            Map<String, String> peerTags =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            snapshot.meshName(),
                            ZLinkRuntimeMetrics.Tag.SOURCE.wire(),
                            snapshot.source());
            registerMeshTopologyGauge(
                    ZLinkRuntimeMetrics.Metric.MESH_NODE_PEERS_CONFIGURED.metricName(),
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.configuredPeers());
            registerMeshTopologyGauge(
                    ZLinkRuntimeMetrics.Metric.MESH_NODE_PEERS_CONNECTED.metricName(),
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.connectedPeers());
            registerMeshTopologyGauge(
                    ZLinkRuntimeMetrics.Metric.MESH_NODE_PEERS_READY.metricName(),
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.readyPeers());
            for (ZLinkRuntimeMetrics.MeshChannelTopologyMetrics channel : snapshot.channels()) {
                Map<String, String> channelTags =
                        Map.of(
                                ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(), snapshot.meshName(),
                                ZLinkRuntimeMetrics.Tag.CHANNEL_NAME.wire(), channel.channelName());
                registerMeshTopologyGauge(
                        ZLinkRuntimeMetrics.Metric.MESH_NODE_CHANNELS_READY_MEMBERS.metricName(),
                        Unit.MEMBER.wire(),
                        channelTags,
                        value ->
                                value.channels().stream()
                                        .filter(
                                                candidate ->
                                                        candidate
                                                                .channelName()
                                                                .equals(channel.channelName()))
                                        .mapToLong(
                                                ZLinkRuntimeMetrics.MeshChannelTopologyMetrics
                                                        ::readyMembers)
                                        .sum());
            }
        }
    }

    @Override
    public void registerObservable(
            ZLinkRuntimeMetrics.Metric metric, Map<String, String> tags, LongSupplier value) {
        Gauge.builder(metric.metricName(), value, LongSupplier::getAsLong)
                .strongReference(true)
                .baseUnit(metric.unit().wire())
                .tags(toTags(tags))
                .register(registry);
    }

    @Override
    public void registerRequestInflight(Map<String, String> tags, LongSupplier value) {
        LongSupplier source = requestInflightValue(tags, value);
        Key key = new Key(ZLinkRuntimeMetrics.REQUEST_INFLIGHT_NAME, Map.copyOf(tags));
        if (!requestInflightGauges.add(key)) {
            return;
        }
        Gauge.builder(ZLinkRuntimeMetrics.REQUEST_INFLIGHT_NAME, source, LongSupplier::getAsLong)
                .baseUnit(Unit.REQUEST.wire())
                .tags(toTags(key.tags()))
                .register(registry);
    }

    @Override
    public void registerHostCapacity(Supplier<ZLinkHostCapacityStatus> source) {
        hostCapacitySource.set(source);
        registerGauge(
                ZLinkRuntimeMetrics.Metric.HOST_CORE_HWM_EFFECTIVE_BUDGET.metricName(),
                Unit.BYTES.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.coreHwm().effectiveBudgetBytes());
        registerGauge(
                ZLinkRuntimeMetrics.Metric.HOST_CORE_HWM_APPLIED.metricName(),
                Unit.BYTES.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.coreHwm().totalAppliedHwmBytes());
        registerGauge(
                CORE_HWM_ACCOUNTED_NAME,
                Unit.BYTES.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.CURRENT.wire()),
                hostCapacitySource,
                status -> status.coreHwm().currentAccountedBytes());
        registerGauge(
                CORE_HWM_ACCOUNTED_NAME,
                Unit.BYTES.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.PEAK.wire()),
                hostCapacitySource,
                status -> status.coreHwm().peakAccountedBytes());
        registerGauge(
                COMPLETION_ACCOUNTED_NAME,
                Unit.BYTES.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.CURRENT.wire()),
                hostCapacitySource,
                status -> status.coreHwm().completionCurrentAccountedBytes());
        registerGauge(
                COMPLETION_ACCOUNTED_NAME,
                Unit.BYTES.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.PEAK.wire()),
                hostCapacitySource,
                status -> status.coreHwm().completionPeakAccountedBytes());
        registerGauge(
                ZLinkRuntimeMetrics.Metric.HOST_CORE_HWM_BLOCKED_RATIO.metricName(),
                Unit.PPM.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.coreHwm().blockedRatioPpm());
        registerGauge(
                ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_LIMIT.metricName(),
                Unit.JOB.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.applicationJobQueue().effectiveMaxQueuedApplicationJobs());
        registerGauge(
                APPLICATION_JOBS_NAME,
                Unit.JOB.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.RESERVED.wire()),
                hostCapacitySource,
                status -> status.applicationJobQueue().reservedSupplyPermits());
        registerGauge(
                APPLICATION_JOBS_NAME,
                Unit.JOB.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.QUEUED.wire()),
                hostCapacitySource,
                status -> status.applicationJobQueue().queuedApplicationJobs());
        registerGauge(
                APPLICATION_JOBS_NAME,
                Unit.JOB.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.IN_USE.wire()),
                hostCapacitySource,
                status -> status.applicationJobQueue().permitsInUse());
        registerGauge(
                APPLICATION_JOBS_NAME,
                Unit.JOB.wire(),
                Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.PEAK.wire()),
                hostCapacitySource,
                status -> status.applicationJobQueue().peakPermitsInUse());
        registerGauge(
                ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAITERS.metricName(),
                Unit.WAITER.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.applicationJobQueue().capacityWaiters());
        FunctionCounter.builder(
                        ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAITS
                                .metricName(),
                        hostCapacitySource,
                        value ->
                                valueOrZero(
                                        value.get(),
                                        status -> status.applicationJobQueue().capacityWaitCount()))
                .baseUnit(Unit.WAIT.wire())
                .register(registry);
        FunctionCounter.builder(
                        ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAIT_DURATION
                                .metricName(),
                        hostCapacitySource,
                        value ->
                                valueOrZero(
                                        value.get(),
                                        status ->
                                                status.applicationJobQueue()
                                                                .capacityWaitDuration()
                                                                .toNanos()
                                                        / (double) TimeUnit.SECONDS.toNanos(1)))
                .baseUnit(Unit.SECONDS.wire())
                .register(registry);
    }

    @Override
    public synchronized void registerApplicationJobQueuePressure(
            Supplier<ZLinkApplicationJobQueuePressureMetrics> source) {
        applicationJobQueuePressureSource.set(source);
        if (!applicationJobQueuePressureRegistered) {
            applicationJobQueuePressureRegistered = true;
            registerGauge(
                    PAUSE_DURATION_NAME,
                    Unit.SECONDS.wire(),
                    Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.CURRENT.wire()),
                    applicationJobQueuePressureSource,
                    pressure ->
                            pressure.currentPauseDuration().toNanos()
                                    / (double) TimeUnit.SECONDS.toNanos(1));
            registerGauge(
                    PAUSE_DURATION_NAME,
                    Unit.SECONDS.wire(),
                    Map.of(ZLinkRuntimeMetrics.Tag.STATE.wire(), GaugeState.CUMULATIVE.wire()),
                    applicationJobQueuePressureSource,
                    pressure ->
                            pressure.cumulativePauseDuration().toNanos()
                                    / (double) TimeUnit.SECONDS.toNanos(1));
            registerPressureCounter(
                    ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_PRESSURE_TRANSITIONS,
                    ZLinkApplicationJobQueuePressureState.RUNNING.wire(),
                    pressure -> pressure.runningTransitionCount());
            registerPressureCounter(
                    ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_PRESSURE_TRANSITIONS,
                    ZLinkApplicationJobQueuePressureState.PAUSED.wire(),
                    pressure -> pressure.pausedTransitionCount());
            FunctionCounter.builder(
                            ZLinkRuntimeMetrics.Metric
                                    .HOST_APPLICATION_JOB_QUEUE_FLOW_STATE_CONFIG_FAILURES
                                    .metricName(),
                            applicationJobQueuePressureSource,
                            current ->
                                    valueOrZero(
                                            current.get(),
                                            pressure -> pressure.flowStateConfigFailureCount()))
                    .baseUnit(Unit.FAILURE.wire())
                    .register(registry);
        }
        observeApplicationJobQueuePressure(applicationJobQueuePressure());
    }

    @Override
    public synchronized void observeApplicationJobQueuePressure(
            ZLinkApplicationJobQueuePressureMetrics snapshot) {
        if (applicationJobQueuePressureStateId != null) {
            registry.remove(applicationJobQueuePressureStateId);
            applicationJobQueuePressureStateId = null;
        }
        if (snapshot == null) {
            return;
        }
        String state = snapshot.pressureState().wire();
        Gauge gauge =
                Gauge.builder(
                                ZLinkRuntimeMetrics.Metric.HOST_APPLICATION_JOB_QUEUE_PRESSURE_STATE
                                        .metricName(),
                                applicationJobQueuePressureStateValue,
                                AtomicLong::doubleValue)
                        .baseUnit(Unit.STATE.wire())
                        .tag(ZLinkRuntimeMetrics.Tag.STATE.wire(), state)
                        .register(registry);
        applicationJobQueuePressureStateId = gauge.getId();
    }

    private void registerPressureCounter(
            ZLinkRuntimeMetrics.Metric metric,
            String state,
            ToDoubleFunction<ZLinkApplicationJobQueuePressureMetrics> value) {
        FunctionCounter.builder(
                        metric.metricName(),
                        applicationJobQueuePressureSource,
                        current -> valueOrZero(current.get(), value))
                .baseUnit(metric.unit().wire())
                .tags(ZLinkRuntimeMetrics.Tag.STATE.wire(), state)
                .register(registry);
    }

    private ZLinkApplicationJobQueuePressureMetrics applicationJobQueuePressure() {
        Supplier<ZLinkApplicationJobQueuePressureMetrics> source =
                applicationJobQueuePressureSource.get();
        return source == null ? null : source.get();
    }

    private <T> void registerGauge(
            String name,
            String baseUnit,
            Map<String, String> tags,
            AtomicReference<Supplier<T>> source,
            ToDoubleFunction<T> value) {
        Gauge.builder(name, source, current -> valueOrZero(current.get(), value))
                .baseUnit(baseUnit)
                .tags(toTags(tags))
                .register(registry);
    }

    private static <T> double valueOrZero(Supplier<T> source, ToDoubleFunction<T> value) {
        T status = source.get();
        return status == null ? 0.0 : value.applyAsDouble(status);
    }

    private static Tags toTags(Map<String, String> tags) {
        for (String key : tags.keySet()) {
            if (FORBIDDEN_TAGS.contains(key)) {
                throw new IllegalArgumentException(
                        "high-cardinality metric tag is forbidden: " + key);
            }
        }
        Tags result = Tags.empty();
        for (Map.Entry<String, String> entry : tags.entrySet()) {
            result = result.and(entry.getKey(), entry.getValue());
        }
        return result;
    }

    private Counter counter(String name, Map<String, String> tags) {
        Map<Map<String, String>, Counter> byTags = counters.get(name);
        Counter existing = byTags == null ? null : byTags.get(tags);
        if (existing != null) {
            return existing;
        }
        byTags = counters.computeIfAbsent(name, ignored -> new ConcurrentHashMap<>());
        Map<String, String> immutableTags = Map.copyOf(tags);
        Counter.Builder builder = Counter.builder(name).tags(toTags(immutableTags));
        String baseUnit = counterUnit(name);
        if (baseUnit != null) {
            builder.baseUnit(baseUnit);
        }
        Counter created = builder.register(registry);
        Counter raced = byTags.putIfAbsent(immutableTags, created);
        return raced == null ? created : raced;
    }

    private DistributionSummary summary(String name, Map<String, String> tags, String baseUnit) {
        Map<Map<String, String>, DistributionSummary> byTags = summaries.get(name);
        DistributionSummary existing = byTags == null ? null : byTags.get(tags);
        if (existing != null) {
            return existing;
        }
        byTags = summaries.computeIfAbsent(name, ignored -> new ConcurrentHashMap<>());
        Map<String, String> immutableTags = Map.copyOf(tags);
        DistributionSummary.Builder builder =
                DistributionSummary.builder(name).tags(toTags(immutableTags));
        if (baseUnit != null) {
            builder.baseUnit(baseUnit);
        }
        DistributionSummary created = builder.register(registry);
        DistributionSummary raced = byTags.putIfAbsent(immutableTags, created);
        return raced == null ? created : raced;
    }

    private void registerMeshTopologyGauge(
            String name,
            String baseUnit,
            Map<String, String> tags,
            ToDoubleFunction<ZLinkRuntimeMetrics.MeshTopologyMetrics> value) {
        Key key = new Key(name, Map.copyOf(tags));
        if (!meshTopologyGauges.add(key)) {
            return;
        }
        Gauge.builder(name, meshTopologySource, ignored -> meshTopologyValue(key.tags(), value))
                .baseUnit(baseUnit)
                .tags(toTags(key.tags()))
                .register(registry);
    }

    private double meshTopologyValue(
            Map<String, String> tags,
            ToDoubleFunction<ZLinkRuntimeMetrics.MeshTopologyMetrics> value) {
        Supplier<List<ZLinkRuntimeMetrics.MeshTopologyMetrics>> source = meshTopologySource.get();
        if (source == null) {
            return 0.0;
        }
        String meshName = tags.get(ZLinkRuntimeMetrics.Tag.MESH_NAME.wire());
        String sourceName = tags.get(ZLinkRuntimeMetrics.Tag.SOURCE.wire());
        return source.get().stream()
                .filter(snapshot -> snapshot.meshName().equals(meshName))
                .filter(snapshot -> sourceName == null || snapshot.source().equals(sourceName))
                .mapToDouble(value)
                .sum();
    }

    private static String counterUnit(String name) {
        ZLinkRuntimeMetrics.Metric metric = ZLinkRuntimeMetrics.Metric.decode(name);
        return metric != null && metric.kind() == ZLinkRuntimeMetrics.Metric.Kind.COUNTER
                ? metric.unit().wire()
                : null;
    }

    private AtomicLong gaugeValue(String name, Map<String, String> tags) {
        Map<Map<String, String>, AtomicLong> byTags = gauges.get(name);
        AtomicLong existing = byTags == null ? null : byTags.get(tags);
        if (existing != null) {
            return existing;
        }
        byTags = gauges.computeIfAbsent(name, ignored -> new ConcurrentHashMap<>());
        Map<String, String> immutableTags = Map.copyOf(tags);
        return byTags.computeIfAbsent(
                immutableTags,
                registeredTags -> {
                    AtomicLong created = new AtomicLong();
                    Gauge.builder(name, created, AtomicLong::doubleValue)
                            .baseUnit(
                                    java.util.Objects.requireNonNull(
                                                    ZLinkRuntimeMetrics.Metric.decode(name))
                                            .unit()
                                            .wire())
                            .tags(toTags(registeredTags))
                            .register(registry);
                    return created;
                });
    }

    private LongSupplier requestInflightValue(Map<String, String> tags, LongSupplier value) {
        String name = ZLinkRuntimeMetrics.REQUEST_INFLIGHT_NAME;
        Map<Map<String, String>, LongSupplier> byTags = requestInflight.get(name);
        LongSupplier existing = byTags == null ? null : byTags.get(tags);
        if (existing != null) {
            return existing;
        }
        byTags = requestInflight.computeIfAbsent(name, ignored -> new ConcurrentHashMap<>());
        Map<String, String> immutableTags = Map.copyOf(tags);
        LongSupplier raced = byTags.putIfAbsent(immutableTags, value);
        return raced == null ? value : raced;
    }

    private record Key(String name, Map<String, String> tags) {}
}
