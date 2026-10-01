package systems.zlink.framework.perf;

import java.io.IOException;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;

// §14 process metrics from public OS/JVM management APIs: CPU time, RSS sampled every 100 ms, allocation and the JVM
// collectors. Unsupported or failed observations stay null with an explicit reason; a missing observation is never 0.
public final class ProcessSampler {
    interface Source {
        Reading<Long> processCpuTimeNs();

        Reading<Long> allocatedBytes();

        Reading<Long> rssBytes();

        long heapUsedBytes();

        long nonHeapUsedBytes();

        Map<String, long[]> collectors();
    }

    static final class Reading<T> {
        private final T value;
        private final NullReason reason;

        private Reading(T value, NullReason reason) {
            this.value = value;
            this.reason = reason;
        }

        static <T> Reading<T> observed(T value) {
            return new Reading<>(java.util.Objects.requireNonNull(value), null);
        }

        static <T> Reading<T> unavailable(String code, String reason) {
            return new Reading<>(null, new NullReason(code, reason));
        }

        boolean isObserved() {
            return reason == null;
        }
    }

    private static final class SystemSource implements Source {
        private final com.sun.management.OperatingSystemMXBean operatingSystem;
        private final com.sun.management.ThreadMXBean threads;

        private SystemSource() {
            java.lang.management.OperatingSystemMXBean os = ManagementFactory.getOperatingSystemMXBean();
            operatingSystem = os instanceof com.sun.management.OperatingSystemMXBean extended ? extended : null;
            java.lang.management.ThreadMXBean threadBean = ManagementFactory.getThreadMXBean();
            threads = threadBean instanceof com.sun.management.ThreadMXBean extended ? extended : null;
        }

        @Override
        public Reading<Long> processCpuTimeNs() {
            if (operatingSystem == null) {
                return Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The JVM does not expose process CPU time.");
            }
            long value = operatingSystem.getProcessCpuTime();
            return value < 0
                    ? Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The JVM does not expose process CPU time.")
                    : Reading.observed(value);
        }

        @Override
        public Reading<Long> allocatedBytes() {
            if (threads == null || !threads.isThreadAllocatedMemorySupported() || !threads.isThreadAllocatedMemoryEnabled()) {
                return Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The JVM does not expose thread allocation totals.");
            }
            long value = threads.getTotalThreadAllocatedBytes();
            return value < 0
                    ? Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The JVM does not expose thread allocation totals.")
                    : Reading.observed(value);
        }

        @Override
        public Reading<Long> rssBytes() {
            try {
                for (String line : Files.readAllLines(Path.of("/proc/self/status"))) {
                    if (line.startsWith("VmRSS:")) {
                        String kilobytes = line.substring("VmRSS:".length()).trim().split("\\s+")[0];
                        return Reading.observed(Math.multiplyExact(Long.parseLong(kilobytes), 1024));
                    }
                }
                return Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The operating system exposes no VmRSS field.");
            } catch (NoSuchFileException error) {
                return Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", "The operating system has no /proc/self/status RSS source.");
            } catch (IOException | NumberFormatException | ArithmeticException | SecurityException error) {
                return Reading.unavailable("COLLECTION_FAILED", "RSS sampling failed: " + message(error));
            }
        }

        @Override
        public long heapUsedBytes() {
            return ManagementFactory.getMemoryMXBean().getHeapMemoryUsage().getUsed();
        }

        @Override
        public long nonHeapUsedBytes() {
            return ManagementFactory.getMemoryMXBean().getNonHeapMemoryUsage().getUsed();
        }

        @Override
        public Map<String, long[]> collectors() {
            Map<String, long[]> values = new LinkedHashMap<>();
            for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans()) {
                values.put(bean.getName(), new long[] {bean.getCollectionCount(), bean.getCollectionTime()});
            }
            return values;
        }
    }

    private final Source source;
    private final List<Long> sampleIntervals = new ArrayList<>();
    private long started;
    private long lastSample;
    private long rssMaxBytes;
    private long rssSamples;
    private NullReason rssFailure;
    private long heapMaxBytes;
    private long nonHeapMaxBytes;
    private Reading<Long> cpuStart;
    private Reading<Long> cpuEnd;
    private Reading<Long> allocatedStart;
    private Reading<Long> allocatedEnd;
    private final Map<String, long[]> collectorStart = new LinkedHashMap<>();
    private final Map<String, long[]> collectorEnd = new LinkedHashMap<>();

    public ProcessSampler() {
        this(new SystemSource());
    }

    ProcessSampler(Source source) {
        this.source = java.util.Objects.requireNonNull(source);
    }

    public void start() {
        started = lastSample = PerfClock.now();
        cpuStart = read(source::processCpuTimeNs, "Process CPU");
        allocatedStart = read(source::allocatedBytes, "Process allocation");
        collectorStart.clear();
        collectorStart.putAll(source.collectors());
        collectorEnd.clear();
        rssMaxBytes = 0;
        rssSamples = 0;
        rssFailure = null;
        heapMaxBytes = source.heapUsedBytes();
        nonHeapMaxBytes = source.nonHeapUsedBytes();
        sampleIntervals.clear();
        sampleRss();
    }

    public void sample() {
        long now = PerfClock.now();
        sampleIntervals.add(now - lastSample);
        lastSample = now;
        sampleRss();
        heapMaxBytes = Math.max(heapMaxBytes, source.heapUsedBytes());
        nonHeapMaxBytes = Math.max(nonHeapMaxBytes, source.nonHeapUsedBytes());
    }

    public void end() {
        sample();
        cpuEnd = read(source::processCpuTimeNs, "Process CPU");
        allocatedEnd = read(source::allocatedBytes, "Process allocation");
        collectorEnd.clear();
        collectorEnd.putAll(source.collectors());
    }

    private void sampleRss() {
        Reading<Long> current = read(source::rssBytes, "Process RSS");
        if (!current.isObserved()) {
            if (rssFailure == null) {
                rssFailure = current.reason;
            }
            return;
        }
        rssSamples++;
        rssMaxBytes = Math.max(rssMaxBytes, current.value);
    }

    private static <T> Reading<T> read(Supplier<Reading<T>> observation, String name) {
        try {
            Reading<T> reading = observation.get();
            return reading == null
                    ? Reading.unavailable("COLLECTION_FAILED", name + " returned no observation.")
                    : reading;
        } catch (UnsupportedOperationException error) {
            return Reading.unavailable("PUBLIC_OBSERVATION_UNSUPPORTED", name + " is not supported.");
        } catch (SecurityException error) {
            return Reading.unavailable("COLLECTION_FAILED", name + " sampling failed: " + message(error));
        } catch (RuntimeException error) {
            return Reading.unavailable("COLLECTION_FAILED", name + " sampling failed: " + message(error));
        }
    }

    private static Reading<Double> cpuPercent(Reading<Long> start, Reading<Long> end, double seconds) {
        NullReason issue = firstUnavailable(start, end);
        if (issue != null) {
            return new Reading<>(null, issue);
        }
        if (seconds <= 0) {
            return Reading.unavailable("ZERO_DENOMINATOR", "The process sampling interval is zero.");
        }
        long elapsed;
        try {
            elapsed = Math.subtractExact(end.value, start.value);
        } catch (ArithmeticException error) {
            return Reading.unavailable("COLLECTION_FAILED", "Process CPU time decreased or overflowed.");
        }
        if (elapsed < 0) {
            return Reading.unavailable("COLLECTION_FAILED", "Process CPU time decreased during the window.");
        }
        return Reading.observed(elapsed / 1e9 / seconds * 100);
    }

    private static Reading<Double> allocatedMb(Reading<Long> start, Reading<Long> end) {
        NullReason issue = firstUnavailable(start, end);
        if (issue != null) {
            return new Reading<>(null, issue);
        }
        long allocated;
        try {
            allocated = Math.subtractExact(end.value, start.value);
        } catch (ArithmeticException error) {
            return Reading.unavailable("COLLECTION_FAILED", "Process allocation total decreased or overflowed.");
        }
        if (allocated < 0) {
            return Reading.unavailable("COLLECTION_FAILED", "Process allocation total decreased during the window.");
        }
        return Reading.observed(allocated / 1048576.0);
    }

    private static NullReason firstUnavailable(Reading<?> start, Reading<?> end) {
        if (start == null || end == null) {
            return new NullReason("COLLECTION_FAILED", "Process observation did not include both window endpoints.");
        }
        return !start.isObserved() ? start.reason : !end.isObserved() ? end.reason : null;
    }

    public void export(Map<String, Object> metrics, Map<String, Object> runtime, Map<String, NullReason> reasons) {
        double seconds = (lastSample - started) / 1e9;
        Reading<Double> cpu = cpuPercent(cpuStart, cpuEnd, seconds);
        Reading<Double> allocation = allocatedMb(allocatedStart, allocatedEnd);
        Reading<Double> rss = rssFailure != null
                ? new Reading<>(null, rssFailure)
                : rssSamples == 0
                        ? Reading.unavailable("NO_SAMPLES", "No resident-set samples were collected.")
                        : Reading.observed(rssMaxBytes / 1048576.0);
        metric(metrics, reasons, "process.cpuPercent", cpu);
        metric(metrics, reasons, "process.rssMb", rss);
        metric(metrics, reasons, "process.allocatedMb", allocation);
        for (int generation = 0; generation < 3; generation++) {
            MetricCatalog.nullValue(metrics, reasons, "metrics", "gc.gen" + generation, "RUNTIME_METRIC_UNSUPPORTED",
                    "The JVM exposes collectors, not .NET generations; see runtimeMetrics.jvmGc.");
        }
        runtime.put("rssSampling", named("/proc/self/status VmRSS", "ns", "sampling", Map.of(
                "requestedIntervalNs", "100000000",
                "actualIntervalsNs", sampleIntervals.stream().map(DecimalText::of).toList(),
                "startedTicks", DecimalText.of(started),
                "endedTicks", DecimalText.of(lastSample))));
        runtime.put("cpuObservationSeconds", named("OperatingSystemMXBean.getProcessCpuTime observation span", "s", "number", seconds));
        runtime.put("allocation", named("ThreadMXBean.getTotalThreadAllocatedBytes", "MiB", "number", allocation.value));
        if (!allocation.isObserved()) {
            reasons.put("/runtimeMetrics/allocation/value", allocation.reason);
        }
        List<Map<String, Object>> collectorsUsed = new ArrayList<>();
        collectorEnd.forEach((name, end) -> {
            long[] begin = collectorStart.get(name);
            if (begin == null || end[0] < begin[0] || end[1] < begin[1]) {
                return;
            }
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("collector", name);
            entry.put("count", DecimalText.of(end[0] - begin[0]));
            entry.put("timeMs", DecimalText.of(end[1] - begin[1]));
            collectorsUsed.add(entry);
        });
        runtime.put("jvmGc", named("GarbageCollectorMXBean window increase", "collections", "array", collectorsUsed));
        runtime.put("jvmHeapMax", named("MemoryMXBean heap used, sampled maximum", "MiB", "number", heapMaxBytes / 1048576.0));
        runtime.put("jvmNonHeapMax", named("MemoryMXBean non-heap used, sampled maximum", "MiB", "number", nonHeapMaxBytes / 1048576.0));
    }

    private static void metric(Map<String, Object> metrics, Map<String, NullReason> reasons, String key, Reading<Double> value) {
        metrics.put(key, value.value);
        if (!value.isObserved()) {
            reasons.put("/metrics/" + key, value.reason);
        }
    }

    private static String message(Exception error) {
        return error.getMessage() == null || error.getMessage().isBlank() ? error.getClass().getName() : error.getMessage();
    }

    public static Map<String, Object> named(String name, String unit, String type, Object value) {
        Map<String, Object> entry = new LinkedHashMap<>();
        entry.put("name", name);
        entry.put("unit", unit);
        entry.put("type", type);
        entry.put("value", value);
        return entry;
    }
}
