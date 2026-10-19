package systems.zlink.framework.runtime.internal.metrics;

import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/** One current population per registered label series, including late provider attachment. */
public final class ZLinkPopulationMetrics implements AutoCloseable {
    private final AtomicLong current = new AtomicLong();
    private final AutoCloseable registration;

    public ZLinkPopulationMetrics(ZLinkRuntimeMetrics.Metric metric, Map<String, String> tags) {
        registration = ZLinkRuntimeMetrics.registerObservable(metric, tags, current::get);
    }

    public void add(long delta) {
        current.addAndGet(delta);
    }

    @Override
    public void close() {
        current.set(0L);
        try {
            registration.close();
        } catch (Exception ignored) {
        }
    }
}
