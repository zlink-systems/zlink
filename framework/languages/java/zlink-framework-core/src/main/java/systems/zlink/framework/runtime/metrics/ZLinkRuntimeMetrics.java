package systems.zlink.framework.runtime.internal.metrics;

import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import systems.zlink.framework.monitoring.ZLinkHostCapacityStatus;

/** Internal, backend-neutral metric hook used by runtime hot paths. */
public final class ZLinkRuntimeMetrics {
    private static final Sink NOOP = new Sink() { };
    private static volatile Sink sink = NOOP;
    private static final AtomicReference<Supplier<ZLinkHostCapacityStatus>>
        HOST_CAPACITY_SOURCE = new AtomicReference<>();
    private static final Supplier<ZLinkHostCapacityStatus>
        HOST_CAPACITY_PROJECTION = () -> {
            Supplier<ZLinkHostCapacityStatus> source = HOST_CAPACITY_SOURCE.get();
            return source == null ? null : source.get();
        };

    private ZLinkRuntimeMetrics() { }

    public static AutoCloseable install(Sink replacement) {
        Sink previous = sink;
        sink = replacement == null ? NOOP : replacement;
        if (HOST_CAPACITY_SOURCE.get() != null) {
            try {
                sink.registerHostCapacity(HOST_CAPACITY_PROJECTION);
            } catch (RuntimeException ignored) {
            }
        }
        return () -> sink = previous;
    }

    /** Registers the one host-capacity projection read by all ten instruments. */
    public static AutoCloseable registerHostCapacity(
        Supplier<ZLinkHostCapacityStatus> source) {
        Objects.requireNonNull(source, "source");
        HOST_CAPACITY_SOURCE.set(source);
        try {
            sink.registerHostCapacity(HOST_CAPACITY_PROJECTION);
        } catch (RuntimeException ignored) {
        }
        return () -> HOST_CAPACITY_SOURCE.compareAndSet(source, null);
    }

    public static boolean enabled() { return sink != NOOP; }

    public static void increment(String name, Map<String, String> tags) {
        try { sink.increment(name, tags); } catch (RuntimeException ignored) { }
    }

    public static void add(String name, long delta, Map<String, String> tags) {
        try { sink.add(name, delta, tags); } catch (RuntimeException ignored) { }
    }

    public static void record(String name, Duration duration, Map<String, String> tags) {
        try { sink.record(name, duration, tags); } catch (RuntimeException ignored) { }
    }

    public static void record(String name, double value, Map<String, String> tags) {
        try { sink.record(name, value, tags); } catch (RuntimeException ignored) { }
    }

    public interface Sink {
        default void increment(String name, Map<String, String> tags) { }
        default void add(String name, long delta, Map<String, String> tags) { }
        default void record(String name, Duration duration, Map<String, String> tags) { }
        default void record(String name, double value, Map<String, String> tags) { }
        default void registerHostCapacity(
            Supplier<ZLinkHostCapacityStatus> source) { }
    }
}
