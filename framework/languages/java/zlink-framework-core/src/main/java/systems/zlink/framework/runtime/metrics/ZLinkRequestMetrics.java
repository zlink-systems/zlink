package systems.zlink.framework.runtime.internal.metrics;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Request metric state shared by all MeshNode request surfaces. */
public final class ZLinkRequestMetrics {
    public static final long NO_START = Long.MIN_VALUE;
    private static final ConcurrentHashMap<String, Series> NODE =
        new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> CHANNEL =
        new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> SPOT =
        new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> INSTANCE_SPOT =
        new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<String, Series> ACTOR =
        new ConcurrentHashMap<>();

    private ZLinkRequestMetrics() { }

    public static Series node(String meshName) {
        return series(NODE, meshName, "node");
    }

    public static Series channel(String meshName) {
        return series(CHANNEL, meshName, "channel");
    }

    public static Series spot(String meshName) {
        return series(SPOT, meshName, "spot");
    }

    public static Series instanceSpot(String meshName) {
        return series(INSTANCE_SPOT, meshName, "instance_spot");
    }

    public static Series actor(String meshName) {
        return series(ACTOR, meshName, "actor");
    }

    public static boolean durationEnabled() {
        return ZLinkRuntimeMetrics.enabled();
    }

    public static long elapsed(long startedNanos, long completedNanos) {
        return startedNanos == NO_START
            ? -1L
            : Math.max(0L, completedNanos - startedNanos);
    }

    public static void start(Series series) {
        if (series == null) {
            return;
        }
        series.inflight.incrementAndGet();
    }

    public static void complete(
        Series series,
        long elapsedNanos,
        Throwable failure) {
        if (series == null) {
            return;
        }
        series.inflight.decrementAndGet();
        Outcome outcome = outcome(failure);
        if (elapsedNanos >= 0L) {
            ZLinkRuntimeMetrics.record(
                "zlink.mesh_node.request.duration",
                elapsedNanos / 1_000_000_000.0,
                switch (outcome) {
                    case COMPLETED -> series.completed;
                    case FAILED -> series.failed;
                    case TIMED_OUT -> series.timedOut;
                });
        }
        if (outcome == Outcome.TIMED_OUT) {
            ZLinkRuntimeMetrics.increment(
                "zlink.mesh_node.request.timeouts",
                series.request);
        }
    }

    private static Series series(
        ConcurrentHashMap<String, Series> cache,
        String meshName,
        String surface) {
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
        for (int depth = 0; current != null && depth < 16; depth++) {
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
        COMPLETED,
        FAILED,
        TIMED_OUT
    }

    public static final class Series {
        private final AtomicLong inflight = new AtomicLong();
        private final Map<String, String> request;
        private final Map<String, String> completed;
        private final Map<String, String> failed;
        private final Map<String, String> timedOut;

        private Series(String meshName, String surface) {
            request = Map.of("mesh_name", meshName, "surface", surface);
            completed = Map.of(
                "mesh_name", meshName, "surface", surface,
                "outcome", "completed");
            failed = Map.of(
                "mesh_name", meshName, "surface", surface,
                "outcome", "failed");
            timedOut = Map.of(
                "mesh_name", meshName, "surface", surface,
                "outcome", "timed_out");
        }

        private void registerInflight() {
            ZLinkRuntimeMetrics.registerRequestInflight(request, inflight::get);
        }
    }
}
