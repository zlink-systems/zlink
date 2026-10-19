package systems.zlink.framework.runtime.internal.metrics;

import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Internal, backend-neutral metric hook used by runtime hot paths. */
public final class ZLinkRuntimeMetrics {

    public enum Unit {
        SECONDS("s"),
        BYTES("By"),
        PPM("{ppm}"),
        JOB("{job}"),
        WAITER("{waiter}"),
        WAIT("{wait}"),
        STATE("{state}"),
        TRANSITION("{transition}"),
        FAILURE("{failure}"),
        PEER("{peer}"),
        MEMBER("{member}"),
        REQUEST("{request}"),
        MESSAGE("{message}"),
        SPOT("{spot}"),
        ACTOR("{actor}"),
        OBJECT("{object}"),
        ACTIVATION("{activation}"),
        RELOCATION("{relocation}"),
        CONNECTION("{connection}"),
        CLAIM("{claim}"),
        TAKEOVER("{takeover}"),
        RUNTIME("{runtime}"),
        OPERATION("{operation}"),
        WARNING("{warning}"),
        ERROR("{error}"),
        EVENT("{event}");

        private final String wire;

        Unit(String wire) {
            this.wire = wire;
        }

        public String wire() {
            return wire;
        }
    }

    public enum Metric {
        HOST_CORE_HWM_EFFECTIVE_BUDGET(
                "zlink.host.core_hwm.effective_budget", Kind.OBSERVABLE, Unit.BYTES),
        HOST_CORE_HWM_APPLIED("zlink.host.core_hwm.applied", Kind.OBSERVABLE, Unit.BYTES),
        HOST_CORE_HWM_ACCOUNTED(
                "zlink.host.core_hwm.accounted", Kind.OBSERVABLE, Unit.BYTES, Tag.STATE),
        HOST_CORE_HWM_COMPLETION_ACCOUNTED(
                "zlink.host.core_hwm.completion_accounted", Kind.OBSERVABLE, Unit.BYTES, Tag.STATE),
        HOST_CORE_HWM_BLOCKED_RATIO("zlink.host.core_hwm.blocked_ratio", Kind.OBSERVABLE, Unit.PPM),
        HOST_APPLICATION_JOB_QUEUE_LIMIT(
                "zlink.host.application_job_queue.limit", Kind.OBSERVABLE, Unit.JOB),
        HOST_APPLICATION_JOB_QUEUE_JOBS(
                "zlink.host.application_job_queue.jobs", Kind.OBSERVABLE, Unit.JOB, Tag.STATE),
        HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAITERS(
                "zlink.host.application_job_queue.capacity_waiters", Kind.OBSERVABLE, Unit.WAITER),
        HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAITS(
                "zlink.host.application_job_queue.capacity_waits", Kind.COUNTER, Unit.WAIT),
        HOST_APPLICATION_JOB_QUEUE_CAPACITY_WAIT_DURATION(
                "zlink.host.application_job_queue.capacity_wait_duration",
                Kind.COUNTER,
                Unit.SECONDS),
        HOST_APPLICATION_JOB_QUEUE_PRESSURE_STATE(
                "zlink.host.application_job_queue.pressure_state",
                Kind.OBSERVABLE,
                Unit.STATE,
                Tag.STATE),
        HOST_APPLICATION_JOB_QUEUE_PRESSURE_TRANSITIONS(
                "zlink.host.application_job_queue.pressure_transitions",
                Kind.COUNTER,
                Unit.TRANSITION,
                Tag.STATE),
        HOST_APPLICATION_JOB_QUEUE_PAUSE_DURATION(
                "zlink.host.application_job_queue.pause_duration",
                Kind.OBSERVABLE,
                Unit.SECONDS,
                Tag.STATE),
        HOST_APPLICATION_JOB_QUEUE_FLOW_STATE_CONFIG_FAILURES(
                "zlink.host.application_job_queue.flow_state_config_failures",
                Kind.COUNTER,
                Unit.FAILURE),
        MESH_NODE_PEERS_CONFIGURED(
                "zlink.mesh_node.peers.configured",
                Kind.OBSERVABLE,
                Unit.PEER,
                Tag.MESH_NAME,
                Tag.SOURCE),
        MESH_NODE_PEERS_CONNECTED(
                "zlink.mesh_node.peers.connected",
                Kind.OBSERVABLE,
                Unit.PEER,
                Tag.MESH_NAME,
                Tag.SOURCE),
        MESH_NODE_PEERS_READY(
                "zlink.mesh_node.peers.ready",
                Kind.OBSERVABLE,
                Unit.PEER,
                Tag.MESH_NAME,
                Tag.SOURCE),
        MESH_NODE_CHANNELS_READY_MEMBERS(
                "zlink.mesh_node.channels.ready_members",
                Kind.OBSERVABLE,
                Unit.MEMBER,
                Tag.MESH_NAME,
                Tag.CHANNEL_NAME),
        CHANNEL_SELECTION_FAILURES(
                "zlink.mesh_node.channel.selection_failures",
                Kind.COUNTER,
                Unit.FAILURE,
                Tag.MESH_NAME,
                Tag.CHANNEL_NAME,
                Tag.REASON),
        MESH_NODE_REQUESTS_INFLIGHT(
                "zlink.mesh_node.requests.inflight",
                Kind.UPDOWN,
                Unit.REQUEST,
                Tag.MESH_NAME,
                Tag.SURFACE),
        REQUEST_DURATION(
                "zlink.mesh_node.request.duration",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.MESH_NAME,
                Tag.SURFACE,
                Tag.OUTCOME),
        REQUEST_TIMEOUTS(
                "zlink.mesh_node.request.timeouts",
                Kind.COUNTER,
                Unit.REQUEST,
                Tag.MESH_NAME,
                Tag.SURFACE),
        MESSAGES_DROPPED(
                "zlink.mesh_node.messages.dropped",
                Kind.COUNTER,
                Unit.MESSAGE,
                Tag.MESH_NAME,
                Tag.SURFACE,
                Tag.MESSAGE_KIND,
                Tag.REASON),
        SPOT_COUNT("zlink.spot.count", Kind.UPDOWN, Unit.SPOT, Tag.MESH_NAME, Tag.SPOT_KIND),
        ACTOR_COUNT("zlink.actor.count", Kind.UPDOWN, Unit.ACTOR, Tag.MESH_NAME),
        OBJECT_CAPACITY_ACTIVE(
                "zlink.object.capacity.active",
                Kind.OBSERVABLE,
                Unit.OBJECT,
                Tag.MESH_NAME,
                Tag.CAPACITY_SCOPE),
        OBJECT_CAPACITY_RESERVED(
                "zlink.object.capacity.reserved",
                Kind.OBSERVABLE,
                Unit.OBJECT,
                Tag.MESH_NAME,
                Tag.CAPACITY_SCOPE),
        OBJECT_CAPACITY_LIMIT(
                "zlink.object.capacity.limit",
                Kind.OBSERVABLE,
                Unit.OBJECT,
                Tag.MESH_NAME,
                Tag.CAPACITY_SCOPE),
        SPOT_TYPE_CAPACITY_ACTIVE(
                "zlink.spot.type.capacity.active",
                Kind.OBSERVABLE,
                Unit.SPOT,
                Tag.MESH_NAME,
                Tag.SPOT_KIND,
                Tag.STABLE_TYPE),
        SPOT_TYPE_CAPACITY_RESERVED(
                "zlink.spot.type.capacity.reserved",
                Kind.OBSERVABLE,
                Unit.SPOT,
                Tag.MESH_NAME,
                Tag.SPOT_KIND,
                Tag.STABLE_TYPE),
        SPOT_TYPE_CAPACITY_LIMIT(
                "zlink.spot.type.capacity.limit",
                Kind.OBSERVABLE,
                Unit.SPOT,
                Tag.MESH_NAME,
                Tag.SPOT_KIND,
                Tag.STABLE_TYPE),
        OBJECT_ACTIVATION_ACTIVE(
                "zlink.object.activation.active", Kind.OBSERVABLE, Unit.ACTIVATION, Tag.MESH_NAME),
        OBJECT_ACTIVATION_LIMIT(
                "zlink.object.activation.limit", Kind.OBSERVABLE, Unit.ACTIVATION, Tag.MESH_NAME),
        RELOCATION_STARTED(
                "zlink.relocation.started",
                Kind.COUNTER,
                Unit.RELOCATION,
                Tag.MESH_NAME,
                Tag.OBJECT_KIND,
                Tag.POLICY),
        RELOCATION_COMPLETED(
                "zlink.relocation.completed",
                Kind.COUNTER,
                Unit.RELOCATION,
                Tag.MESH_NAME,
                Tag.OBJECT_KIND,
                Tag.POLICY,
                Tag.OUTCOME),
        RELOCATION_DURATION(
                "zlink.relocation.duration",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.MESH_NAME,
                Tag.OBJECT_KIND,
                Tag.POLICY,
                Tag.OUTCOME),
        RELOCATION_BYTES(
                "zlink.relocation.bytes",
                Kind.HISTOGRAM,
                Unit.BYTES,
                Tag.MESH_NAME,
                Tag.OBJECT_KIND,
                Tag.POLICY),
        STREAM_CONNECTIONS_ACTIVE(
                "zlink.stream.connections.active", Kind.UPDOWN, Unit.CONNECTION, Tag.TRANSPORT),
        STREAM_CONNECTIONS_OPENED(
                "zlink.stream.connections.opened", Kind.COUNTER, Unit.CONNECTION, Tag.TRANSPORT),
        STREAM_CONNECTIONS_CLOSED(
                "zlink.stream.connections.closed",
                Kind.COUNTER,
                Unit.CONNECTION,
                Tag.TRANSPORT,
                Tag.CLOSE_REASON),
        INSTANCE_SPOT_ACTIVATIONS(
                "zlink.instance_spot.activations",
                Kind.COUNTER,
                Unit.ACTIVATION,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE,
                Tag.OUTCOME),
        INSTANCE_SPOT_ACTIVATION_DURATION(
                "zlink.instance_spot.activation.duration",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE,
                Tag.OUTCOME),
        INSTANCE_SPOT_PENDING_MESSAGES(
                "zlink.instance_spot.pending.messages",
                Kind.OBSERVABLE,
                Unit.MESSAGE,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE),
        INSTANCE_SPOT_PENDING_BYTES(
                "zlink.instance_spot.pending.bytes",
                Kind.OBSERVABLE,
                Unit.BYTES,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE),
        INSTANCE_SPOT_CLAIM_CONFLICTS(
                "zlink.instance_spot.claim.conflicts",
                Kind.COUNTER,
                Unit.CLAIM,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE,
                Tag.REASON),
        INSTANCE_SPOT_TAKEOVERS(
                "zlink.instance_spot.takeovers",
                Kind.COUNTER,
                Unit.TAKEOVER,
                Tag.MESH_NAME,
                Tag.INSTANCE_SPOT_TYPE,
                Tag.OUTCOME),
        HOST_STATE("zlink.host.state", Kind.OBSERVABLE, Unit.RUNTIME, Tag.STATE),
        HOST_RELOCATION_DURATION(
                "zlink.host.relocation.duration",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.MODE,
                Tag.OUTCOME),
        HOST_RELOCATION_BLOCKED(
                "zlink.host.relocation.blocked",
                Kind.COUNTER,
                Unit.OPERATION,
                Tag.MODE,
                Tag.REASON),
        RELOCATION_INTERRUPTION(
                "zlink.relocation.interruption",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.UNIT_KIND,
                Tag.EXECUTION_MODE),
        RELOCATION_TARGET_RESUME(
                "zlink.relocation.target_resume", Kind.HISTOGRAM, Unit.SECONDS, Tag.UNIT_KIND),
        RELOCATION_ROUTE_CONVERGENCE(
                "zlink.relocation.route_convergence", Kind.HISTOGRAM, Unit.SECONDS, Tag.UNIT_KIND),
        RELOCATION_CUTOVER_TIMEOUT(
                "zlink.relocation.cutover_timeout", Kind.COUNTER, Unit.WARNING, Tag.UNIT_KIND),
        HOST_SHUTDOWN_DURATION(
                "zlink.host.shutdown.duration", Kind.HISTOGRAM, Unit.SECONDS, Tag.OUTCOME),
        HOST_SHUTDOWN_FORCED(
                "zlink.host.shutdown.forced", Kind.COUNTER, Unit.OPERATION, Tag.REASON),
        LOCATION_STORE_ERRORS(
                "zlink.location.store.errors", Kind.COUNTER, Unit.ERROR, Tag.OPERATION),
        LOCATION_OWNER_LEASE_RENEW_FAILURES(
                "zlink.location.owner_lease.renew.failures",
                Kind.COUNTER,
                Unit.FAILURE,
                Tag.SCOPE_KIND,
                Tag.SCOPE_NAME),
        LOCATION_OWNER_LEASE_RENEW_LATENESS(
                "zlink.location.owner_lease.renew.lateness",
                Kind.HISTOGRAM,
                Unit.SECONDS,
                Tag.SCOPE_KIND,
                Tag.SCOPE_NAME),
        OBSERVABILITY_EVENTS_OVERFLOW(
                "zlink.observability.events.overflow", Kind.COUNTER, Unit.EVENT, Tag.SOURCE);

        public enum Kind {
            HISTOGRAM,
            COUNTER,
            UPDOWN,
            OBSERVABLE
        }

        private final String metricName;
        private final Kind kind;
        private final Unit unit;
        private final List<String> labels;

        Metric(String metricName, Kind kind, Unit unit, Tag... labels) {
            this.metricName = metricName;
            this.kind = kind;
            this.unit = unit;
            this.labels = java.util.Arrays.stream(labels).map(Tag::wire).toList();
        }

        public String metricName() {
            return metricName;
        }

        public Kind kind() {
            return kind;
        }

        public Unit unit() {
            return unit;
        }

        public List<String> labels() {
            return labels;
        }

        public static Metric decode(String name) {
            return BY_NAME.get(name);
        }

        private static final Map<String, Metric> BY_NAME =
                java.util.Arrays.stream(values())
                        .collect(
                                java.util.stream.Collectors.toUnmodifiableMap(
                                        Metric::metricName, value -> value));
    }

    public enum SpotKind {
        ENTRY,
        USER,
        INSTANCE;
        private final String wire = name().toLowerCase(java.util.Locale.ROOT);

        public String wire() {
            return wire;
        }
    }

    private enum TakeoverOutcome {
        CLAIMED,
        LOST,
        FAILED;
        private final String wire = name().toLowerCase(java.util.Locale.ROOT);

        public String wire() {
            return wire;
        }
    }

    public enum LocationOperation {
        READ("read"),
        COMPARE_EXCHANGE("compare_exchange"),
        RELOCATION_PUT("relocation_put"),
        RELOCATION_GET("relocation_get"),
        RELOCATION_DELETE("relocation_delete"),
        LEASE_RENEW("lease_renew"),
        RELEASE("release");
        private final Map<String, String> tags;

        LocationOperation(String value) {
            tags = Map.of(Tag.OPERATION.wire(), value);
        }

        public void failure() {
            increment(Metric.LOCATION_STORE_ERRORS.metricName(), tags);
        }
    }

    public enum Tag {
        MESH_NAME("mesh_name"),
        SURFACE("surface"),
        STATE("state"),
        REASON("reason"),
        OUTCOME("outcome"),
        SOURCE("source"),
        CHANNEL_NAME("channel_name"),
        MESSAGE_KIND("message_kind"),
        CAPACITY_SCOPE("capacity_scope"),
        CLOSE_REASON("close_reason"),
        EXECUTION_MODE("execution_mode"),
        INSTANCE_SPOT_TYPE("instance_spot_type"),
        MODE("mode"),
        OBJECT_KIND("object_kind"),
        OPERATION("operation"),
        POLICY("policy"),
        SCOPE_KIND("scope_kind"),
        SCOPE_NAME("scope_name"),
        SPOT_KIND("spot_kind"),
        STABLE_TYPE("stable_type"),
        TRANSPORT("transport"),
        UNIT_KIND("unit_kind");

        private final String wire;

        Tag(String wire) {
            this.wire = wire;
        }

        public String wire() {
            return wire;
        }
    }

    public static final String REQUEST_INFLIGHT_NAME =
            Metric.MESH_NODE_REQUESTS_INFLIGHT.metricName();
    private static final ConcurrentHashMap<Object, Supplier<String>> HOST_STATE_SOURCES =
            new ConcurrentHashMap<>();
    private static final Supplier<Map<String, Long>> HOST_STATE_PROJECTION =
            () -> {
                var states = new java.util.HashMap<String, Long>();
                HOST_STATE_SOURCES
                        .values()
                        .forEach(source -> states.merge(source.get(), 1L, Long::sum));
                return Map.copyOf(states);
            };

    public static void recordInstanceTakeover(
            String meshName, String stableType, Throwable failure) {
        while (failure instanceof java.util.concurrent.CompletionException
                && failure.getCause() != null) failure = failure.getCause();
        TakeoverOutcome outcome =
                failure == null
                        ? TakeoverOutcome.CLAIMED
                        : failure
                                                instanceof
                                                systems.zlink.framework.errors
                                                                .ZLinkFrameworkException
                                                        terminal
                                        && terminal.kind()
                                                == systems.zlink.framework.errors
                                                        .ZLinkFrameworkErrorKind.UNAVAILABLE
                                        && !(failure
                                                instanceof
                                                systems.zlink.framework.runtime.internal.locations
                                                        .ZLinkLocationStoreFailure)
                                ? TakeoverOutcome.LOST
                                : TakeoverOutcome.FAILED;
        increment(
                Metric.INSTANCE_SPOT_TAKEOVERS.metricName(),
                Map.of(
                        Tag.MESH_NAME.wire(),
                        meshName,
                        Tag.INSTANCE_SPOT_TYPE.wire(),
                        stableType,
                        Tag.OUTCOME.wire(),
                        outcome.wire()));
    }

    private static final Map<Integer, Map<String, String>> RELOCATION_UNIT_TAGS =
            java.util.Arrays.stream(
                            systems.zlink.framework.locations.ZLinkPlacementObjectKind.values())
                    .collect(
                            java.util.stream.Collectors.toUnmodifiableMap(
                                    systems.zlink.framework.locations.ZLinkPlacementObjectKind
                                            ::value,
                                    kind -> Map.of(Tag.UNIT_KIND.wire(), wire(kind))));

    public static Map<String, String> relocationUnitTags(int objectKind) {
        return RELOCATION_UNIT_TAGS.get(objectKind);
    }

    private static final Sink NOOP = new Sink() {};
    private static volatile Sink sink = NOOP;
    private static final AtomicReference<Supplier<ZLinkHostCapacityStatus>> HOST_CAPACITY_SOURCE =
            new AtomicReference<>();
    private static final AtomicReference<Supplier<ZLinkApplicationJobQueuePressureMetrics>>
            APPLICATION_JOB_QUEUE_PRESSURE_SOURCE = new AtomicReference<>();
    private static final ConcurrentHashMap<Object, Supplier<List<MeshTopologyMetrics>>>
            MESH_TOPOLOGY_SOURCES = new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<ObservableKey, ConcurrentHashMap<Object, LongSupplier>>
            OBSERVABLE_SOURCES = new ConcurrentHashMap<>();
    private static final Supplier<ZLinkHostCapacityStatus> HOST_CAPACITY_PROJECTION =
            () -> {
                Supplier<ZLinkHostCapacityStatus> source = HOST_CAPACITY_SOURCE.get();
                return source == null ? null : source.get();
            };
    private static final Supplier<ZLinkApplicationJobQueuePressureMetrics>
            APPLICATION_JOB_QUEUE_PRESSURE_PROJECTION =
                    () -> {
                        Supplier<ZLinkApplicationJobQueuePressureMetrics> source =
                                APPLICATION_JOB_QUEUE_PRESSURE_SOURCE.get();
                        return source == null ? null : source.get();
                    };
    private static final Supplier<List<MeshTopologyMetrics>> MESH_TOPOLOGY_PROJECTION =
            () -> {
                List<MeshTopologyMetrics> snapshots = new ArrayList<>();
                MESH_TOPOLOGY_SOURCES.values().forEach(source -> snapshots.addAll(source.get()));
                return List.copyOf(snapshots);
            };

    /** A metric source registration owns no checked-failure resource. */
    @FunctionalInterface
    public interface Registration extends AutoCloseable {
        @Override
        void close();
    }

    private static void reportObservationFailure(RuntimeException failure) {
        java.util.logging.Logger.getLogger(ZLinkRuntimeMetrics.class.getName())
                .log(java.util.logging.Level.WARNING, "Runtime metric observation failed", failure);
    }

    private ZLinkRuntimeMetrics() {}

    public static Registration install(Sink replacement) {
        Sink previous = sink;
        sink = replacement == null ? NOOP : replacement;
        publishHostState();
        if (HOST_CAPACITY_SOURCE.get() != null) {
            try {
                sink.registerHostCapacity(HOST_CAPACITY_PROJECTION);
            } catch (RuntimeException failure) {
                reportObservationFailure(failure);
            }
        }
        if (APPLICATION_JOB_QUEUE_PRESSURE_SOURCE.get() != null) {
            try {
                sink.registerApplicationJobQueuePressure(APPLICATION_JOB_QUEUE_PRESSURE_PROJECTION);
            } catch (RuntimeException failure) {
                reportObservationFailure(failure);
            }
        }
        if (!MESH_TOPOLOGY_SOURCES.isEmpty()) {
            try {
                sink.registerMeshTopology(MESH_TOPOLOGY_PROJECTION);
            } catch (RuntimeException failure) {
                reportObservationFailure(failure);
            }
        }
        OBSERVABLE_SOURCES
                .keySet()
                .forEach(
                        key -> {
                            try {
                                publishObservable(key);
                            } catch (RuntimeException failure) {
                                reportObservationFailure(failure);
                            }
                        });
        return () -> sink = previous;
    }

    public static Registration registerHostState(Supplier<String> source) {
        Object owner = new Object();
        HOST_STATE_SOURCES.put(owner, Objects.requireNonNull(source, "source"));
        publishHostState();
        return () -> {
            if (HOST_STATE_SOURCES.remove(owner, source)) {
                publishHostState();
            }
        };
    }

    public static void publishHostState() {
        try {
            sink.observeHostStates(HOST_STATE_PROJECTION);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public static String wire(Enum<?> value) {
        return value.name().toLowerCase(java.util.Locale.ROOT);
    }

    /** Registers the one host-capacity projection read by all ten instruments. */
    public static Registration registerHostCapacity(Supplier<ZLinkHostCapacityStatus> source) {
        Objects.requireNonNull(source, "source");
        HOST_CAPACITY_SOURCE.set(source);
        try {
            sink.registerHostCapacity(HOST_CAPACITY_PROJECTION);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
        return () -> HOST_CAPACITY_SOURCE.compareAndSet(source, null);
    }

    /** Registers metric-only application-queue pressure accounting. */
    public static Registration registerApplicationJobQueuePressure(
            Supplier<ZLinkApplicationJobQueuePressureMetrics> source) {
        Objects.requireNonNull(source, "source");
        APPLICATION_JOB_QUEUE_PRESSURE_SOURCE.set(source);
        try {
            sink.registerApplicationJobQueuePressure(APPLICATION_JOB_QUEUE_PRESSURE_PROJECTION);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
        return () -> {
            if (APPLICATION_JOB_QUEUE_PRESSURE_SOURCE.compareAndSet(source, null)) {
                try {
                    sink.observeApplicationJobQueuePressure(null);
                } catch (RuntimeException failure) {
                    reportObservationFailure(failure);
                }
            }
        };
    }

    /** Registers host-owned RouteMesh state read only by observable metrics. */
    public static Registration registerMeshTopology(Supplier<List<MeshTopologyMetrics>> source) {
        Objects.requireNonNull(source, "source");
        Object owner = new Object();
        MESH_TOPOLOGY_SOURCES.put(owner, source);
        try {
            sink.registerMeshTopology(MESH_TOPOLOGY_PROJECTION);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
        return () -> MESH_TOPOLOGY_SOURCES.remove(owner, source);
    }

    static Registration registerRequestInflight(Map<String, String> tags, LongSupplier value) {
        return registerObservable(Metric.MESH_NODE_REQUESTS_INFLIGHT, tags, value);
    }

    public static Registration registerObservable(
            Metric metric, Map<String, String> tags, LongSupplier value) {
        ObservableKey key = new ObservableKey(metric, Map.copyOf(tags));
        Object owner = new Object();
        OBSERVABLE_SOURCES.compute(
                key,
                (ignored, sources) -> {
                    if (sources == null) sources = new ConcurrentHashMap<>();
                    sources.put(owner, value);
                    return sources;
                });
        try {
            publishObservable(key);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
        return () ->
                OBSERVABLE_SOURCES.computeIfPresent(
                        key,
                        (ignored, sources) -> {
                            sources.remove(owner);
                            return sources.isEmpty() ? null : sources;
                        });
    }

    private static void publishObservable(ObservableKey key) {
        LongSupplier projection =
                () -> {
                    var sources = OBSERVABLE_SOURCES.get(key);
                    long total = 0;
                    if (sources != null)
                        for (var source : sources.values()) total += source.getAsLong();
                    return total;
                };
        if (key.metric() == Metric.MESH_NODE_REQUESTS_INFLIGHT)
            sink.registerRequestInflight(key.tags(), projection);
        else sink.registerObservable(key.metric(), key.tags(), projection);
    }

    public static boolean enabled() {
        return sink != NOOP;
    }

    public static void increment(String name, Map<String, String> tags) {
        try {
            sink.increment(name, tags);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public static void add(String name, long delta, Map<String, String> tags) {
        try {
            sink.add(name, delta, tags);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public static void record(String name, Duration duration, Map<String, String> tags) {
        try {
            sink.record(name, duration, tags);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public static void record(String name, double value, Map<String, String> tags) {
        try {
            sink.record(name, value, tags);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public static void publishApplicationJobQueuePressure(
            ZLinkApplicationJobQueuePressureMetrics snapshot) {
        try {
            sink.observeApplicationJobQueuePressure(snapshot);
        } catch (RuntimeException failure) {
            reportObservationFailure(failure);
        }
    }

    public record MeshTopologyMetrics(
            String meshName,
            String source,
            long configuredPeers,
            long connectedPeers,
            long readyPeers,
            List<MeshChannelTopologyMetrics> channels) {
        public MeshTopologyMetrics {
            Objects.requireNonNull(meshName, "meshName");
            Objects.requireNonNull(source, "source");
            channels = List.copyOf(channels);
        }
    }

    public record MeshChannelTopologyMetrics(String channelName, long readyMembers) {
        public MeshChannelTopologyMetrics {
            Objects.requireNonNull(channelName, "channelName");
        }
    }

    private record ObservableKey(Metric metric, Map<String, String> tags) {}

    public interface Sink {
        default void observeHostState(String state) {}

        default void observeHostStates(Supplier<Map<String, Long>> states) {
            states.get().keySet().forEach(this::observeHostState);
        }

        default void increment(String name, Map<String, String> tags) {}

        default void add(String name, long delta, Map<String, String> tags) {}

        default void record(String name, Duration duration, Map<String, String> tags) {}

        default void record(String name, double value, Map<String, String> tags) {}

        default void registerHostCapacity(Supplier<ZLinkHostCapacityStatus> source) {}

        default void registerApplicationJobQueuePressure(
                Supplier<ZLinkApplicationJobQueuePressureMetrics> source) {}

        default void observeApplicationJobQueuePressure(
                ZLinkApplicationJobQueuePressureMetrics snapshot) {}

        default void registerMeshTopology(Supplier<List<MeshTopologyMetrics>> source) {}

        default void registerObservable(
                Metric metric, Map<String, String> tags, LongSupplier value) {}

        default void registerRequestInflight(Map<String, String> tags, LongSupplier value) {}
    }
}
