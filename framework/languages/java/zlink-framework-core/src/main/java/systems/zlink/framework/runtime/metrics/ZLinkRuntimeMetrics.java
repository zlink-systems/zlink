package systems.zlink.framework.runtime.internal.metrics;

import java.time.Duration;
import java.util.Map;

/** Internal, backend-neutral metric hook used by runtime hot paths. */
public final class ZLinkRuntimeMetrics {
    private static final Sink NOOP = new Sink() { };
    private static volatile Sink sink = NOOP;

    private ZLinkRuntimeMetrics() { }

    public static AutoCloseable install(Sink replacement) {
        Sink previous = sink;
        sink = replacement == null ? NOOP : replacement;
        return () -> sink = previous;
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
    }
}
