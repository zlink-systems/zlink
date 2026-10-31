package systems.zlink.framework.testutil;

import com.sun.management.ThreadMXBean;

import java.lang.management.ManagementFactory;
import java.util.Objects;
import java.util.function.Supplier;

/** Measures a call batch several times and returns the smallest allocation total. */
public final class PerCallAllocation {
    private static final int WINDOWS = 8;
    private static volatile Object sink;

    public record AllocationComparison(long baselineBytes, long measuredBytes) {}

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

    /**
     * Measures an action that alternates between a baseline and measured operation on each call.
     * The baseline operation must run first, and {@code callsPerOperation} calls of each operation
     * are measured in every window. Returns the pair with the smallest measured-minus-baseline
     * allocation difference.
     */
    public static AllocationComparison minAlternatingWindow(
            Supplier<?> action, int callsPerOperation, int warmupCallsPerOperation) {
        Objects.requireNonNull(action, "action");
        if (callsPerOperation <= 0)
            throw new IllegalArgumentException("callsPerOperation must be positive");
        if (warmupCallsPerOperation < 0)
            throw new IllegalArgumentException("warmupCallsPerOperation must not be negative");

        ThreadMXBean allocations = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        if (!allocations.isThreadAllocatedMemorySupported())
            throw new IllegalStateException("thread allocation accounting is required");
        if (!allocations.isThreadAllocatedMemoryEnabled())
            allocations.setThreadAllocatedMemoryEnabled(true);

        long threadId = Thread.currentThread().threadId();
        for (int index = 0; index < warmupCallsPerOperation * 2; index++) sink = action.get();

        AllocationComparison minimum = null;
        long minimumDifference = Long.MAX_VALUE;
        for (int window = 0; window < WINDOWS; window++) {
            long baselineBytes = 0;
            long measuredBytes = 0;
            for (int index = 0; index < callsPerOperation; index++) {
                long before = allocations.getThreadAllocatedBytes(threadId);
                sink = action.get();
                baselineBytes += allocations.getThreadAllocatedBytes(threadId) - before;

                before = allocations.getThreadAllocatedBytes(threadId);
                sink = action.get();
                measuredBytes += allocations.getThreadAllocatedBytes(threadId) - before;
            }

            long difference = measuredBytes - baselineBytes;
            if (difference < minimumDifference) {
                minimumDifference = difference;
                minimum = new AllocationComparison(baselineBytes, measuredBytes);
            }
        }
        return minimum;
    }
}
