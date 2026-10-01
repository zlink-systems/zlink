package systems.zlink.framework.spring;

import io.micrometer.core.instrument.Counter;
import io.micrometer.core.instrument.DistributionSummary;
import io.micrometer.core.instrument.FunctionCounter;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Tags;
import io.micrometer.core.instrument.Timer;

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
    private static final String CORE_HWM_ACCOUNTED_NAME = "zlink.host.core_hwm.accounted";
    private static final String COMPLETION_ACCOUNTED_NAME =
            "zlink.host.core_hwm.completion_accounted";
    private static final String APPLICATION_JOBS_NAME = "zlink.host.application_job_queue.jobs";
    private static final String PAUSE_DURATION_NAME =
            "zlink.host.application_job_queue.pause_duration";
    private static final String PRESSURE_TRANSITIONS_SUFFIX = "pressure_transitions";

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
    private final Map<String, Map<Map<String, String>, AtomicLong>> gauges =
            new ConcurrentHashMap<>();
    private final Map<String, Map<Map<String, String>, Counter>> counters =
            new ConcurrentHashMap<>();
    private final Map<String, Map<Map<String, String>, Timer>> timers = new ConcurrentHashMap<>();
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
        timer(name, tags).record(duration.toNanos(), TimeUnit.NANOSECONDS);
    }

    @Override
    public void record(String name, double value, Map<String, String> tags) {
        ZLinkRuntimeMetrics.Metric metric = ZLinkRuntimeMetrics.Metric.decode(name);
        if (metric != null && metric.kind() == ZLinkRuntimeMetrics.Metric.Kind.HISTOGRAM) {
            timer(name, tags)
                    .record(
                            Math.max(0L, Math.round(value * (double) TimeUnit.SECONDS.toNanos(1))),
                            TimeUnit.NANOSECONDS);
            return;
        }
        summary(name, tags, null).record(value);
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
                    "zlink.mesh_node.peers.configured",
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.configuredPeers());
            registerMeshTopologyGauge(
                    "zlink.mesh_node.peers.connected",
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.connectedPeers());
            registerMeshTopologyGauge(
                    "zlink.mesh_node.peers.ready",
                    Unit.PEER.wire(),
                    peerTags,
                    value -> value.readyPeers());
            for (ZLinkRuntimeMetrics.MeshChannelTopologyMetrics channel : snapshot.channels()) {
                Map<String, String> channelTags =
                        Map.of(
                                ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(), snapshot.meshName(),
                                ZLinkRuntimeMetrics.Tag.CHANNEL_NAME.wire(), channel.channelName());
                registerMeshTopologyGauge(
                        "zlink.mesh_node.channels.ready_members",
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
                "zlink.host.core_hwm.effective_budget",
                Unit.BYTES.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.coreHwm().effectiveBudgetBytes());
        registerGauge(
                "zlink.host.core_hwm.applied",
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
                "zlink.host.core_hwm.blocked_ratio",
                Unit.PPM.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.coreHwm().blockedRatioPpm());
        registerGauge(
                "zlink.host.application_job_queue.limit",
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
                "zlink.host.application_job_queue.capacity_waiters",
                Unit.WAITER.wire(),
                Map.of(),
                hostCapacitySource,
                status -> status.applicationJobQueue().capacityWaiters());
        FunctionCounter.builder(
                        "zlink.host.application_job_queue.capacity_waits",
                        hostCapacitySource,
                        value ->
                                valueOrZero(
                                        value.get(),
                                        status -> status.applicationJobQueue().capacityWaitCount()))
                .baseUnit(Unit.WAIT.wire())
                .register(registry);
        FunctionCounter.builder(
                        "zlink.host.application_job_queue.capacity_wait_duration",
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
                    PRESSURE_TRANSITIONS_SUFFIX,
                    ZLinkApplicationJobQueuePressureState.RUNNING.wire(),
                    pressure -> pressure.runningTransitionCount());
            registerPressureCounter(
                    PRESSURE_TRANSITIONS_SUFFIX,
                    ZLinkApplicationJobQueuePressureState.PAUSED.wire(),
                    pressure -> pressure.pausedTransitionCount());
            FunctionCounter.builder(
                            "zlink.host.application_job_queue.flow_state_config_failures",
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
                                "zlink.host.application_job_queue.pressure_state",
                                applicationJobQueuePressureStateValue,
                                AtomicLong::doubleValue)
                        .baseUnit(Unit.STATE.wire())
                        .tag(ZLinkRuntimeMetrics.Tag.STATE.wire(), state)
                        .register(registry);
        applicationJobQueuePressureStateId = gauge.getId();
    }

    private void registerPressureCounter(
            String name,
            String state,
            ToDoubleFunction<ZLinkApplicationJobQueuePressureMetrics> value) {
        FunctionCounter.builder(
                        "zlink.host.application_job_queue." + name,
                        applicationJobQueuePressureSource,
                        current -> valueOrZero(current.get(), value))
                .baseUnit(Unit.TRANSITION.wire())
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

    private Timer timer(String name, Map<String, String> tags) {
        Map<Map<String, String>, Timer> byTags = timers.get(name);
        Timer existing = byTags == null ? null : byTags.get(tags);
        if (existing != null) {
            return existing;
        }
        byTags = timers.computeIfAbsent(name, ignored -> new ConcurrentHashMap<>());
        Map<String, String> immutableTags = Map.copyOf(tags);
        Timer created = Timer.builder(name).tags(toTags(immutableTags)).register(registry);
        Timer raced = byTags.putIfAbsent(immutableTags, created);
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
