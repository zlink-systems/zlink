/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;

import java.lang.management.ManagementFactory;
import java.lang.management.ThreadMXBean;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Client CPU over one measured window.
 *
 * <p>Process cores are the wrong instrument for java, measured rather than assumed. Two things are
 * wrong with them. They are polluted: 94% of the process CPU of a {@code zlink-java} send cell is
 * Core's native I/O threads running no user code, against 3% for {@code grpc-java}, so {@code
 * client_cores} compares unlike quantities between exactly the two rows a 0.80 ratio divides. And
 * they cannot discriminate: against a 20-core ceiling the largest reading observed on this machine
 * was 0.154 of it, so the mark could never fire, which is the failure node hit from the other
 * direction (FB-023).
 *
 * <p>So the declared instrument is the CPU of the threads this harness runs its own submit loop on,
 * against the submit parallelism it declares. Both sides are then the same thing -- "the threads
 * this harness offers its submit path" -- and the mark fires exactly when the harness, not the
 * transport, is the limit, which is what FB-010 and FB-016 showed matters. GC and JIT threads never
 * appear in {@link ThreadMXBean}, so they cannot inflate it.
 *
 * <p>Process cores and all-JVM-thread cores are still reported beside it as observations. Neither
 * decides saturation.
 */
public final class ClientResources {
    public static final int LOGICAL_CORES = Runtime.getRuntime().availableProcessors();

    /**
     * FB-023 extended: the java row declares {@code jvm_thread_cores}, the CPU charged to the JVM
     * threads this harness runs its SUBMIT loop on, divided by elapsed time. Its ceiling is the
     * submit parallelism the driver declares -- 1 for the serial and window drivers, the send
     * concurrency for the send driver -- so the instrument and the ceiling measure the same thing.
     */
    public static final String CLIENT_SATURATION_METRIC = "jvm_thread_cores";

    private static final ThreadMXBean THREADS = ManagementFactory.getThreadMXBean();

    private final long cpuStartNs;
    private final long threadCpuStartNs;
    private final ConcurrentHashMap<Long, SubmitCpu> submitThreads = new ConcurrentHashMap<>();

    public ClientResources() {
        this.cpuStartNs = BenchServerMetrics.processCpuNs();
        this.threadCpuStartNs = jvmThreadCpuNs();
    }

    /** CPU of the calling thread, for measuring a submit loop's own cost. */
    public static long currentThreadCpuNs() {
        if (!THREADS.isCurrentThreadCpuTimeSupported()) {
            return 0;
        }
        long cpu = THREADS.getCurrentThreadCpuTime();
        return cpu > 0 ? cpu : 0;
    }

    /**
     * Retain a submit thread's final CPU sample before it exits. At active close, live threads are
     * read directly without waiting for the drain to finish.
     */
    public void addSubmitCpuNs(long cpuNs) {
        submitThreads.computeIfPresent(
                Thread.currentThread().threadId(),
                (id, sample) -> new SubmitCpu(sample.startNs(), Math.max(0, cpuNs)));
    }

    public long beginSubmit() {
        long start = currentThreadCpuNs();
        submitThreads.put(Thread.currentThread().threadId(), new SubmitCpu(start, null));
        return start;
    }

    public Usage finish(long elapsedNs) {
        double elapsedSeconds = elapsedNs / 1e9;
        double cpuSeconds = (BenchServerMetrics.processCpuNs() - cpuStartNs) / 1e9;
        double threadCpuSeconds = (jvmThreadCpuNs() - threadCpuStartNs) / 1e9;
        long submitCpuNs = 0;
        for (var entry : submitThreads.entrySet()) {
            SubmitCpu sample = entry.getValue();
            long cpu = THREADS.getThreadCpuTime(entry.getKey());
            // A worker publishes its final sample before exiting. If it exited
            // during this read, use that publication rather than a missing bean value.
            if (cpu < 0) {
                sample = submitThreads.get(entry.getKey());
            }
            submitCpuNs +=
                    sample.finishedNs() != null
                            ? sample.finishedNs()
                            : Math.max(0, cpu - sample.startNs());
        }
        double submitCpuSeconds = submitCpuNs / 1e9;
        double cores = elapsedSeconds > 0 ? cpuSeconds / elapsedSeconds : 0;
        double threadCores = elapsedSeconds > 0 ? threadCpuSeconds / elapsedSeconds : 0;
        double submitCores = elapsedSeconds > 0 ? submitCpuSeconds / elapsedSeconds : 0;
        return new Usage(
                cpuSeconds,
                elapsedSeconds,
                cores,
                (cores / LOGICAL_CORES) * 100.0,
                submitCores,
                threadCores,
                BenchServerMetrics.residentSetMb());
    }

    private record SubmitCpu(long startNs, Long finishedNs) {}

    /** CPU charged to JVM threads. Core's native I/O threads are not JVM threads. */
    private static long jvmThreadCpuNs() {
        if (!THREADS.isThreadCpuTimeSupported()) {
            return 0;
        }
        if (!THREADS.isThreadCpuTimeEnabled()) {
            THREADS.setThreadCpuTimeEnabled(true);
        }
        long total = 0;
        for (long id : THREADS.getAllThreadIds()) {
            long cpu = THREADS.getThreadCpuTime(id);
            if (cpu > 0) {
                total += cpu;
            }
        }
        return total;
    }

    public record Usage(
            double cpuSeconds,
            double elapsedSeconds,
            double cores,
            double cpuPercent,
            /** The declared instrument: submit-thread CPU over elapsed time. */
            double submitCores,
            /** Observation: every live JVM thread, which is not the declared instrument. */
            double jvmAllThreadCores,
            double memoryMb) {}
}
