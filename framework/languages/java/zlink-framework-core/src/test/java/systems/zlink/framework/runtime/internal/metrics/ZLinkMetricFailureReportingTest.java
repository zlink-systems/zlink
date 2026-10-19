package systems.zlink.framework.runtime.internal.metrics;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.*;
import java.util.logging.*;

final class ZLinkMetricFailureReportingTest {
    @Test
    void failedObservationReportsTheOriginalThrowable() throws Exception {
        var failure = new IllegalStateException("observer-failure");
        var records = new ArrayList<LogRecord>();
        var logger = Logger.getLogger(ZLinkRuntimeMetrics.class.getName());
        var handler =
                new Handler() {
                    public void publish(LogRecord record) {
                        records.add(record);
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(handler);
        try (var ignored =
                ZLinkRuntimeMetrics.install(
                        new ZLinkRuntimeMetrics.Sink() {
                            public void increment(String name, Map<String, String> tags) {
                                throw failure;
                            }

                            public void add(String name, long delta, Map<String, String> tags) {}

                            public void record(
                                    String name,
                                    java.time.Duration duration,
                                    Map<String, String> tags) {}
                        })) {
            assertDoesNotThrow(
                    () -> ZLinkRuntimeMetrics.increment("zlink.relocation.started", Map.of()));
            assertTrue(records.stream().anyMatch(record -> record.getThrown() == failure));
        } finally {
            logger.removeHandler(handler);
        }
    }

    @Test
    void metricRegistrationCloseDeclaresNoCheckedException() throws Exception {
        var method =
                ZLinkRuntimeMetrics.class.getMethod(
                        "registerHostState", java.util.function.Supplier.class);
        assertEquals(0, method.getReturnType().getMethod("close").getExceptionTypes().length);
    }
}
