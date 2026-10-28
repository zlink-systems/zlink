package systems.zlink.framework.testutil;

import com.sun.management.ThreadMXBean;

import java.lang.management.ManagementFactory;
import java.util.Objects;
import java.util.function.Supplier;

/** Measures a call batch several times and returns the smallest allocation total. */
public final class PerCallAllocation {
    private static final int WINDOWS = 8;
    private static volatile Object sink;

    private PerCallAllocation() {}

    public static long minWindow(Supplier<?> action, int callsPerWindow, int warmupCalls) {
        return minWindow(action, callsPerWindow, warmupCalls, WINDOWS);
    }

    public static long minWindow(
            Supplier<?> action, int callsPerWindow, int warmupCalls, int windows) {
        Objects.requireNonNull(action, "action");
        if (callsPerWindow <= 0)
            throw new IllegalArgumentException("callsPerWindow must be positive");
        if (warmupCalls < 0) throw new IllegalArgumentException("warmupCalls must not be negative");
        if (windows <= 0) throw new IllegalArgumentException("windows must be positive");

        ThreadMXBean allocations = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        if (!allocations.isThreadAllocatedMemorySupported())
            throw new IllegalStateException("thread allocation accounting is required");
        if (!allocations.isThreadAllocatedMemoryEnabled())
            allocations.setThreadAllocatedMemoryEnabled(true);

        long threadId = Thread.currentThread().threadId();
        for (int index = 0; index < warmupCalls; index++) sink = action.get();

        long minimum = Long.MAX_VALUE;
        for (int window = 0; window < windows; window++) {
            long before = allocations.getThreadAllocatedBytes(threadId);
            for (int index = 0; index < callsPerWindow; index++) sink = action.get();
            minimum = Math.min(minimum, allocations.getThreadAllocatedBytes(threadId) - before);
        }
        return minimum;
    }
}
