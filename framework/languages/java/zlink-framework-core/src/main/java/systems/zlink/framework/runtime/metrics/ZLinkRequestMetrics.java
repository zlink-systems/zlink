package systems.zlink.framework.runtime.internal.metrics;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;

/** Request metric state shared by all MeshNode request surfaces. */
public final class ZLinkRequestMetrics {
    public static final long NO_START = Long.MIN_VALUE;
    private static final int MAX_FAILURE_CAUSE_DEPTH = 16;
    private static final ConcurrentHashMap<String, Series> NODE = new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> CHANNEL = new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> SPOT = new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> INSTANCE_SPOT =
            new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> ACTOR = new ConcurrentHashMap<>();

    private ZLinkRequestMetrics() {}

    public static Series node(String meshName) {
        return series(NODE, meshName, ZLinkDispatchErrorSurface.NODE.traceName());
    }

    public static Series channel(String meshName) {
        return series(CHANNEL, meshName, ZLinkDispatchErrorSurface.CHANNEL.traceName());
    }

    public static Series spot(String meshName) {
        return series(SPOT, meshName, ZLinkDispatchErrorSurface.SPOT_ROUTE.traceName());
    }

    public static Series instanceSpot(String meshName) {
        return series(INSTANCE_SPOT, meshName, ZLinkDispatchErrorSurface.INSTANCE_SPOT.traceName());
    }

    public static Series actor(String meshName) {
        return series(ACTOR, meshName, ZLinkDispatchErrorSurface.SPOT_ACTOR.traceName());
    }

    public static boolean durationEnabled() {
        return ZLinkRuntimeMetrics.enabled();
    }

    public static long elapsed(long startedNanos, long completedNanos) {
        return startedNanos == NO_START ? -1L : Math.max(0L, completedNanos - startedNanos);
    }

    public static void start(Series series) {
        if (series == null) {
            return;
        }
        series.inflight.incrementAndGet();
    }

    public static void complete(Series series, long elapsedNanos, Throwable failure) {
        if (series == null) {
            return;
        }
        series.inflight.decrementAndGet();
        Outcome outcome = outcome(failure);
        if (elapsedNanos >= 0L) {
            ZLinkRuntimeMetrics.record(
                    ZLinkRuntimeMetrics.Metric.REQUEST_DURATION.metricName(),
                    (double) elapsedNanos / TimeUnit.SECONDS.toNanos(1),
                    switch (outcome) {
                        case COMPLETED -> series.completed;
                        case FAILED -> series.failed;
                        case TIMED_OUT -> series.timedOut;
                    });
        }
        if (outcome == Outcome.TIMED_OUT) {
            ZLinkRuntimeMetrics.increment(
                    ZLinkRuntimeMetrics.Metric.REQUEST_TIMEOUTS.metricName(), series.request);
        }
    }

    private static Series series(
            ConcurrentHashMap<String, Series> cache, String meshName, String surface) {
        if (meshName == null) {
            return null;
        }
        Series existing = cache.get(meshName);
        if (existing != null) {
            return existing;
        }
        Series created = new Series(meshName, surface);
        existing = cache.putIfAbsent(meshName, created);
        if (existing != null) {
            return existing;
        }
        created.registerInflight();
        return created;
    }

    private static Outcome outcome(Throwable failure) {
        if (failure == null) {
            return Outcome.COMPLETED;
        }
        Throwable current = failure;
        for (int depth = 0; current != null && depth < MAX_FAILURE_CAUSE_DEPTH; depth++) {
            if (current instanceof TimeoutException
                    || current instanceof ZlinkRequestException request
                            && request.getResult() == RequestResult.TIMED_OUT
                    || current instanceof ZLinkFrameworkException framework
                            && framework.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED) {
                return Outcome.TIMED_OUT;
            }
            Throwable cause = current.getCause();
            if (cause == current) {
                break;
            }
            current = cause;
        }
        return Outcome.FAILED;
    }

    private enum Outcome {
        COMPLETED("completed"),
        FAILED("failed"),
        TIMED_OUT("timed_out");
        private final String wire;

        Outcome(String wire) {
            this.wire = wire;
        }

        String wire() {
            return wire;
        }
    }

    public static final class Series {
        private final AtomicLong inflight = new AtomicLong();
        private final Map<String, String> request;
        private final Map<String, String> completed;
        private final Map<String, String> failed;
        private final Map<String, String> timedOut;

        private Series(String meshName, String surface) {
            request =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.SURFACE.wire(),
                            surface);
            completed =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.SURFACE.wire(),
                            surface,
                            ZLinkRuntimeMetrics.Tag.OUTCOME.wire(),
                            Outcome.COMPLETED.wire());
            failed =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.SURFACE.wire(),
                            surface,
                            ZLinkRuntimeMetrics.Tag.OUTCOME.wire(),
                            Outcome.FAILED.wire());
            timedOut =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.SURFACE.wire(),
                            surface,
                            ZLinkRuntimeMetrics.Tag.OUTCOME.wire(),
                            Outcome.TIMED_OUT.wire());
        }

        private void registerInflight() {
            ZLinkRuntimeMetrics.registerRequestInflight(request, inflight::get);
        }
    }
}
