package systems.zlink.samples.bingo.server.configuration;

import io.micrometer.core.instrument.Meter;
import io.micrometer.core.instrument.MeterRegistry;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/**
 * Shows how an application connects the framework's Micrometer instruments to
 * its own registry. A production application would normally export this
 * registry through its existing OpenTelemetry or Prometheus pipeline.
 */
public final class BingoMetricsReporter implements AutoCloseable {
    private final MeterRegistry registry;
    private final ScheduledExecutorService reporter;

    public BingoMetricsReporter(MeterRegistry registry, String role) {
        this.registry = registry;
        this.reporter = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "bingo-" + role + "-metrics");
            thread.setDaemon(true);
            return thread;
        });
        reporter.scheduleAtFixedRate(this::report, 0, 500, TimeUnit.MILLISECONDS);
    }

    private void report() {
        for (Meter meter : registry.getMeters()) {
            if (!meter.getId().getName().startsWith("zlink.")) {
                continue;
            }
            meter.measure().forEach(measurement -> System.out.printf(
                "zlink metric role=%s name=%s statistic=%s value=%s%n",
                Thread.currentThread().getName(),
                meter.getId().getName(),
                measurement.getStatistic(),
                measurement.getValue()));
        }
    }

    @Override
    public void close() {
        reporter.shutdownNow();
    }
}
