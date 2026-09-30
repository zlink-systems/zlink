package systems.zlink.framework.perf;

import java.lang.management.ManagementFactory;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

// §15.2: monotonic elapsed time. Ticks are nanoseconds of System.nanoTime() shifted so every value is positive
// (0 marks "not started" in the window bookkeeping); the domain is this process only.
public final class PerfClock {
    private PerfClock() {}

    private static final long ORIGIN = System.nanoTime() - 1_000_000_000L;
    public static final String DOMAIN =
            "process-" + ProcessHandle.current().pid() + "-" + UUID.randomUUID().toString().replace("-", "");

    public static long now() {
        return System.nanoTime() - ORIGIN;
    }

    public static String unixMs() {
        return DecimalText.of(System.currentTimeMillis());
    }

    /** The §15.2 ClockMetadata object of this process. */
    public static Map<String, Object> metadata() {
        Map<String, Object> clock = new LinkedHashMap<>();
        clock.put("source", "System.nanoTime");
        clock.put("nativeFrequencyHz", "1000000000");
        clock.put("ticksUnit", "ns");
        clock.put("clockDomainId", DOMAIN);
        clock.put("scope", "process");
        clock.put("alignmentMethod", null);
        clock.put("maxErrorNs", null);
        clock.put("validFromTicks", null);
        clock.put("validThroughTicks", null);
        clock.put("evidence", List.of(
                "JVM " + ManagementFactory.getRuntimeMXBean().getVmName() + " " + System.getProperty("java.version"),
                "RTT uses only this process clock; remote receivedTicks is diagnostic."));
        return clock;
    }
}
