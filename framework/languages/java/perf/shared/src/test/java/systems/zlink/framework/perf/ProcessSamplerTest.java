package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.junit.jupiter.api.Test;

import java.util.LinkedHashMap;
import java.util.Map;

class ProcessSamplerTest {
    private static class FixedSource implements ProcessSampler.Source {
        private final ProcessSampler.Reading<Long> cpu;
        private final ProcessSampler.Reading<Long> allocated;
        private final ProcessSampler.Reading<Long> rss;

        private FixedSource(ProcessSampler.Reading<Long> cpu, ProcessSampler.Reading<Long> allocated,
                ProcessSampler.Reading<Long> rss) {
            this.cpu = cpu;
            this.allocated = allocated;
            this.rss = rss;
        }

        @Override
        public ProcessSampler.Reading<Long> processCpuTimeNs() {
            return cpu;
        }

        @Override
        public ProcessSampler.Reading<Long> allocatedBytes() {
            return allocated;
        }

        @Override
        public ProcessSampler.Reading<Long> rssBytes() {
            return rss;
        }

        @Override
        public long heapUsedBytes() {
            return 0;
        }

        @Override
        public long nonHeapUsedBytes() {
            return 0;
        }

        @Override
        public Map<String, long[]> collectors() {
            return Map.of();
        }
    }

    @Test
    void unsupportedAndFailedReadingsRemainNullWithReasons() {
        ProcessSampler sampler = new ProcessSampler(new FixedSource(
                ProcessSampler.Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "CPU unsupported."),
                ProcessSampler.Reading.unavailable("COLLECTION_FAILED", "Allocation read failed."),
                ProcessSampler.Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "RSS unsupported.")));
        sampler.start();
        sampler.end();
        Map<String, Object> metrics = new LinkedHashMap<>();
        Map<String, Object> runtime = new LinkedHashMap<>();
        Map<String, NullReason> reasons = new LinkedHashMap<>();

        sampler.export(metrics, runtime, reasons);

        assertNull(metrics.get("process.cpuPercent"));
        assertNull(metrics.get("process.rssMb"));
        assertNull(metrics.get("process.allocatedMb"));
        assertEquals("PUBLIC_OBSERVATION_UNSUPPORTED", reasons.get("/metrics/process.cpuPercent").code());
        assertEquals("PUBLIC_OBSERVATION_UNSUPPORTED", reasons.get("/metrics/process.rssMb").code());
        assertEquals("COLLECTION_FAILED", reasons.get("/metrics/process.allocatedMb").code());
        assertEquals("COLLECTION_FAILED", reasons.get("/runtimeMetrics/allocation/value").code());
        @SuppressWarnings("unchecked")
        Map<String, Object> allocation = (Map<String, Object>) runtime.get("allocation");
        assertNull(allocation.get("value"));
    }

    @Test
    void observedZeroIsPreservedAsZero() {
        ProcessSampler sampler = new ProcessSampler(new FixedSource(
                ProcessSampler.Reading.observed(0L), ProcessSampler.Reading.observed(0L), ProcessSampler.Reading.observed(0L)));
        sampler.start();
        sampler.end();
        Map<String, Object> metrics = new LinkedHashMap<>();
        sampler.export(metrics, new LinkedHashMap<>(), new LinkedHashMap<>());

        assertEquals(0.0, metrics.get("process.cpuPercent"));
        assertEquals(0.0, metrics.get("process.rssMb"));
        assertEquals(0.0, metrics.get("process.allocatedMb"));
    }

    @Test
    void runtimeCollectionFailuresRemainNullWithReasons() {
        ProcessSampler.Source source = new FixedSource(
                ProcessSampler.Reading.observed(0L), ProcessSampler.Reading.observed(0L),
                ProcessSampler.Reading.observed(0L)) {
            @Override
            public ProcessSampler.Reading<Long> allocatedBytes() {
                throw new IllegalStateException("allocation unavailable");
            }
        };
        ProcessSampler sampler = new ProcessSampler(source);
        sampler.start();
        sampler.end();
        Map<String, Object> metrics = new LinkedHashMap<>();
        Map<String, NullReason> reasons = new LinkedHashMap<>();

        sampler.export(metrics, new LinkedHashMap<>(), reasons);

        assertNull(metrics.get("process.allocatedMb"));
        assertEquals("COLLECTION_FAILED", reasons.get("/metrics/process.allocatedMb").code());
    }
}
