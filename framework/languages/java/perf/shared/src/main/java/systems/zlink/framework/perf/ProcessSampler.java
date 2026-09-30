package systems.zlink.framework.perf;

import java.io.IOException;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// §14 process metrics from the public OS/JVM management API: CPU time, RSS sampled every 100 ms, allocation and the
// JVM collectors. The JVM has no .NET generations, so gc.gen0..2 stay null and the collector counts go to
// runtimeMetrics (§14.2: JVM collector counts are not renamed to .NET generations).
public final class ProcessSampler {
    private final com.sun.management.OperatingSystemMXBean os =
            (com.sun.management.OperatingSystemMXBean) ManagementFactory.getOperatingSystemMXBean();
    private final com.sun.management.ThreadMXBean threads =
            (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
    private final List<Long> sampleIntervals = new ArrayList<>();
    private long started;
    private long lastSample;
    private long rssMaxBytes;
    private long heapMaxBytes;
    private long nonHeapMaxBytes;
    private long cpuStartNs;
    private long cpuEndNs;
    private long allocatedStart;
    private long allocatedEnd;
    private final Map<String, long[]> collectorStart = new LinkedHashMap<>();
    private final Map<String, long[]> collectorEnd = new LinkedHashMap<>();

    public void start() {
        started = lastSample = PerfClock.now();
        cpuStartNs = os.getProcessCpuTime();
        allocatedStart = threads.getTotalThreadAllocatedBytes();
        collectors(collectorStart);
        rssMaxBytes = rssBytes();
        heapMaxBytes = ManagementFactory.getMemoryMXBean().getHeapMemoryUsage().getUsed();
        nonHeapMaxBytes = ManagementFactory.getMemoryMXBean().getNonHeapMemoryUsage().getUsed();
        sampleIntervals.clear();
    }

    public void sample() {
        long now = PerfClock.now();
        sampleIntervals.add(now - lastSample);
        lastSample = now;
        rssMaxBytes = Math.max(rssMaxBytes, rssBytes());
        heapMaxBytes = Math.max(heapMaxBytes, ManagementFactory.getMemoryMXBean().getHeapMemoryUsage().getUsed());
        nonHeapMaxBytes = Math.max(nonHeapMaxBytes, ManagementFactory.getMemoryMXBean().getNonHeapMemoryUsage().getUsed());
    }

    public void end() {
        sample();
        cpuEndNs = os.getProcessCpuTime();
        allocatedEnd = threads.getTotalThreadAllocatedBytes();
        collectors(collectorEnd);
    }

    private static void collectors(Map<String, long[]> into) {
        into.clear();
        for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans()) {
            into.put(bean.getName(), new long[] {bean.getCollectionCount(), bean.getCollectionTime()});
        }
    }

    // The resident set from the OS (Linux /proc); a JVM has no portable public RSS accessor.
    private static long rssBytes() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/self/status"))) {
                if (line.startsWith("VmRSS:")) {
                    return Long.parseLong(line.replaceAll("[^0-9]", "")) * 1024;
                }
            }
        } catch (IOException | NumberFormatException ignored) {
            // no OS resident-set observation on this platform
        }
        return 0;
    }

    public void export(Map<String, Object> metrics, Map<String, Object> runtime, Map<String, NullReason> reasons) {
        double seconds = (lastSample - started) / 1e9;
        metrics.put("process.cpuPercent", seconds > 0 ? (cpuEndNs - cpuStartNs) / 1e9 / seconds * 100 : 0.0);
        metrics.put("process.rssMb", rssMaxBytes / 1048576.0);
        metrics.put("process.allocatedMb", Math.max(0, allocatedEnd - allocatedStart) / 1048576.0);
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
        runtime.put("allocation", named("ThreadMXBean.getTotalThreadAllocatedBytes", "MiB", "number",
                Math.max(0, allocatedEnd - allocatedStart) / 1048576.0));
        List<Map<String, Object>> collectorsUsed = new ArrayList<>();
        collectorEnd.forEach((name, end) -> {
            long[] begin = collectorStart.getOrDefault(name, new long[] {0, 0});
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("collector", name);
            entry.put("count", DecimalText.of(Math.max(0, end[0] - begin[0])));
            entry.put("timeMs", DecimalText.of(Math.max(0, end[1] - begin[1])));
            collectorsUsed.add(entry);
        });
        runtime.put("jvmGc", named("GarbageCollectorMXBean window increase", "collections", "array", collectorsUsed));
        runtime.put("jvmHeapMax", named("MemoryMXBean heap used, sampled maximum", "MiB", "number", heapMaxBytes / 1048576.0));
        runtime.put("jvmNonHeapMax", named("MemoryMXBean non-heap used, sampled maximum", "MiB", "number", nonHeapMaxBytes / 1048576.0));
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
