package systems.zlink.framework.runtime.internal.dispatch;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.OptionalLong;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.LongSupplier;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkConfigurationException;

/**
 * The single host-owned aggregate that admits ordinary inbound application jobs.
 *
 * <p>A permit starts as a receive/claim reservation, moves with the queued job,
 * and is returned immediately before the handler's first instruction. Capacity
 * is handed directly to the oldest live waiter; a new caller cannot barge ahead
 * of an existing waiter.</p>
 */
public final class ZLinkApplicationJobQueue implements AutoCloseable {
    private static final Path PROC_STATUS = Path.of("/proc/self/status");
    private static final List<Path> CPUSET_PATHS = List.of(
        Path.of("/sys/fs/cgroup/cpuset.cpus.effective"),
        Path.of("/sys/fs/cgroup/cpuset/cpuset.cpus"));
    private static final Path CPU_MAX = Path.of("/sys/fs/cgroup/cpu.max");
    private static final Path CPU_QUOTA =
        Path.of("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    private static final Path CPU_PERIOD =
        Path.of("/sys/fs/cgroup/cpu/cpu.cfs_period_us");

    private final Object lock = new Object();
    private final ZLinkApplicationJobQueueProfile configuredProfile;
    private final OptionalLong configuredManualMax;
    private final int effectiveProcessorCount;
    private final long effectiveLimit;
    private final LongSupplier nanoTime;
    private final ArrayDeque<Waiter> waiters = new ArrayDeque<>();
    private long reservedSupplyPermits;
    private long queuedApplicationJobs;
    private long permitsInUse;
    private long peakPermitsInUse;
    private long capacityWaitCount;
    private long capacityWaitDurationNanos;
    private boolean closed;

    public ZLinkApplicationJobQueue(
        ZLinkApplicationJobQueueProfile profile,
        OptionalLong manualMax,
        ProcessorCandidates processorCandidates) {
        this(profile, manualMax, processorCandidates, System::nanoTime);
    }

    ZLinkApplicationJobQueue(
        ZLinkApplicationJobQueueProfile profile,
        OptionalLong manualMax,
        ProcessorCandidates processorCandidates,
        LongSupplier nanoTime) {
        ResolvedCapacity capacity = resolveCapacity(
            profile, manualMax, processorCandidates);
        this.configuredProfile = capacity.configuredProfile();
        this.configuredManualMax = capacity.configuredManualMax();
        this.effectiveProcessorCount = capacity.effectiveProcessorCount();
        this.effectiveLimit = capacity.effectiveLimit();
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
    }

    public CompletionStage<Permit> acquire() {
        synchronized (lock) {
            if (closed) {
                return CompletableFuture.failedFuture(
                    new CancellationException("Application Job Queue is closed"));
            }
            if (waiters.isEmpty() && permitsInUse < effectiveLimit) {
                return CompletableFuture.completedFuture(reserveUnderLock());
            }
            Waiter waiter = new Waiter(this, nanoTime.getAsLong());
            waiters.addLast(waiter);
            capacityWaitCount = saturatingIncrement(capacityWaitCount);
            return waiter.future;
        }
    }

    /** Blocking bridge for dedicated receive-loop threads. */
    public Permit acquireBlocking() throws InterruptedException {
        CompletableFuture<Permit> future = acquire().toCompletableFuture();
        try {
            return future.get();
        } catch (InterruptedException interrupted) {
            future.cancel(false);
            throw interrupted;
        } catch (java.util.concurrent.ExecutionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof CancellationException cancellation) {
                throw cancellation;
            }
            if (cause instanceof RuntimeException runtime) {
                throw runtime;
            }
            throw new IllegalStateException("Application Job Queue wait failed", cause);
        }
    }

    public Snapshot snapshot() {
        synchronized (lock) {
            return new Snapshot(
                configuredProfile,
                configuredManualMax,
                effectiveProcessorCount,
                effectiveLimit,
                reservedSupplyPermits,
                queuedApplicationJobs,
                permitsInUse,
                peakPermitsInUse,
                waiters.stream().filter(Waiter::waiting).count(),
                capacityWaitCount,
                Duration.ofNanos(capacityWaitDurationNanos));
        }
    }

    public void resetMetrics() {
        synchronized (lock) {
            peakPermitsInUse = permitsInUse;
            capacityWaitCount = 0;
            capacityWaitDurationNanos = 0;
        }
    }

    @Override
    public void close() {
        List<Waiter> cancelled = new ArrayList<>();
        synchronized (lock) {
            if (closed) {
                return;
            }
            closed = true;
            while (!waiters.isEmpty()) {
                Waiter waiter = waiters.removeFirst();
                if (waiter.state == WaiterState.WAITING) {
                    waiter.state = WaiterState.CANCELLED;
                    recordWaitDurationUnderLock(waiter);
                    cancelled.add(waiter);
                }
            }
        }
        cancelled.forEach(waiter -> waiter.future.cancelFromQueue());
    }

    public static ResolvedCapacity resolveCapacity(
        ZLinkApplicationJobQueueProfile profile,
        OptionalLong manualMax,
        ProcessorCandidates candidates) {
        Objects.requireNonNull(profile, "profile");
        Objects.requireNonNull(manualMax, "manualMax");
        Objects.requireNonNull(candidates, "candidates");
        int processors = candidates.minimumKnownPositive();
        if (manualMax.isPresent()) {
            long value = manualMax.getAsLong();
            if (value < 1 || value > Integer.MAX_VALUE) {
                throw new ZLinkConfigurationException(
                    "MaxQueuedApplicationJobs must be in 1..2147483647");
            }
            return new ResolvedCapacity(profile, manualMax, processors, value);
        }
        long effective;
        try {
            effective = Math.multiplyExact(
                (long) processors, jobsPerProcessor(profile));
        } catch (ArithmeticException overflow) {
            throw new ZLinkConfigurationException(
                "Application Job Queue capacity calculation overflowed", overflow);
        }
        if (effective < 1 || effective > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "Application Job Queue capacity calculation exceeds 2147483647");
        }
        return new ResolvedCapacity(profile, manualMax, processors, effective);
    }

    public static ProcessorCandidates productionProcessorCandidates(
        Executor handlerExecutor) {
        int runtime = Math.max(1, Runtime.getRuntime().availableProcessors());
        Integer affinity = minimumPositive(
            readAffinityCount(), readCpusetCount());
        Integer quota = readQuotaCount();
        Integer executor = explicitExecutorMaximum(handlerExecutor);
        return new ProcessorCandidates(runtime, affinity, quota, executor);
    }

    private static long jobsPerProcessor(
        ZLinkApplicationJobQueueProfile profile) {
        return switch (profile) {
            case COMPACT -> 32L;
            case LOW_LATENCY -> 64L;
            case BALANCED -> 128L;
            case THROUGHPUT -> 256L;
        };
    }

    private Permit reserveUnderLock() {
        if (permitsInUse >= effectiveLimit) {
            throw new IllegalStateException("Application Job Queue oversubscribed");
        }
        reservedSupplyPermits++;
        permitsInUse++;
        peakPermitsInUse = Math.max(peakPermitsInUse, permitsInUse);
        return new Permit(this);
    }

    private void queued(Permit permit) {
        synchronized (lock) {
            if (permit.state != PermitState.RESERVED) {
                return;
            }
            permit.state = PermitState.QUEUED;
            reservedSupplyPermits--;
            queuedApplicationJobs++;
        }
    }

    private void release(Permit permit) {
        Grant grant;
        synchronized (lock) {
            if (permit.state == PermitState.RELEASED) {
                return;
            }
            grant = releaseUnderLock(permit);
        }
        finishGrant(grant);
    }

    private void abandonReservation(Permit permit) {
        Grant grant;
        synchronized (lock) {
            if (permit.state != PermitState.RESERVED) {
                return;
            }
            grant = releaseUnderLock(permit);
        }
        finishGrant(grant);
    }

    private Grant releaseUnderLock(Permit permit) {
        if (permit.state == PermitState.RESERVED) {
            reservedSupplyPermits--;
        } else {
            queuedApplicationJobs--;
        }
        permitsInUse--;
        permit.state = PermitState.RELEASED;
        return closed ? null : grantOldestUnderLock();
    }

    private Grant grantOldestUnderLock() {
        while (!waiters.isEmpty()) {
            Waiter waiter = waiters.removeFirst();
            if (waiter.state != WaiterState.WAITING) {
                continue;
            }
            waiter.state = WaiterState.GRANTED;
            recordWaitDurationUnderLock(waiter);
            Permit permit = reserveUnderLock();
            waiter.permit = permit;
            return new Grant(waiter, permit);
        }
        return null;
    }

    private static void finishGrant(Grant grant) {
        if (grant == null) {
            return;
        }
        if (!grant.waiter.future.complete(grant.permit)) {
            grant.permit.close();
        }
    }

    private void cancelWaiter(Waiter waiter) {
        Permit granted = null;
        synchronized (lock) {
            if (waiter.state == WaiterState.WAITING) {
                waiter.state = WaiterState.CANCELLED;
                waiters.remove(waiter);
                recordWaitDurationUnderLock(waiter);
            } else if (waiter.state == WaiterState.GRANTED) {
                waiter.state = WaiterState.CANCELLED;
                granted = waiter.permit;
            }
        }
        if (granted != null) {
            granted.close();
        }
    }

    private void recordWaitDurationUnderLock(Waiter waiter) {
        if (waiter.durationRecorded) {
            return;
        }
        waiter.durationRecorded = true;
        long elapsed = Math.max(0L, nanoTime.getAsLong() - waiter.startedAtNanos);
        capacityWaitDurationNanos = saturatingAdd(
            capacityWaitDurationNanos, elapsed);
    }

    private static long saturatingIncrement(long value) {
        return value == Long.MAX_VALUE ? value : value + 1;
    }

    private static long saturatingAdd(long left, long right) {
        if (right <= 0) {
            return left;
        }
        return left > Long.MAX_VALUE - right ? Long.MAX_VALUE : left + right;
    }

    private static Integer explicitExecutorMaximum(Executor executor) {
        if (executor instanceof ThreadPoolExecutor pool) {
            return positiveOrNull(pool.getMaximumPoolSize());
        }
        if (executor instanceof ForkJoinPool pool) {
            return positiveOrNull(pool.getParallelism());
        }
        return null;
    }

    private static Integer readAffinityCount() {
        try {
            for (String line : Files.readAllLines(PROC_STATUS)) {
                if (line.startsWith("Cpus_allowed_list:")) {
                    return parseCpuList(line.substring(line.indexOf(':') + 1));
                }
            }
        } catch (IOException | RuntimeException ignored) {
        }
        return null;
    }

    private static Integer readCpusetCount() {
        Integer minimum = null;
        for (Path path : CPUSET_PATHS) {
            try {
                Integer count = parseCpuList(Files.readString(path));
                minimum = minimumPositive(minimum, count);
            } catch (IOException | RuntimeException ignored) {
            }
        }
        return minimum;
    }

    private static Integer readQuotaCount() {
        try {
            String[] fields = Files.readString(CPU_MAX).trim().split("\\s+");
            if (fields.length >= 2 && !"max".equals(fields[0])) {
                return quotaCount(Long.parseLong(fields[0]), Long.parseLong(fields[1]));
            }
        } catch (IOException | RuntimeException ignored) {
        }
        try {
            return quotaCount(
                Long.parseLong(Files.readString(CPU_QUOTA).trim()),
                Long.parseLong(Files.readString(CPU_PERIOD).trim()));
        } catch (IOException | RuntimeException ignored) {
            return null;
        }
    }

    private static Integer quotaCount(long quota, long period) {
        if (quota <= 0 || period <= 0) {
            return null;
        }
        long value = Math.max(1L, quota / period);
        return value > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) value;
    }

    private static Integer parseCpuList(String value) {
        long count = 0;
        for (String part : value.trim().split(",")) {
            if (part.isBlank()) {
                continue;
            }
            String[] bounds = part.trim().split("-");
            long first = Long.parseLong(bounds[0]);
            long last = bounds.length == 1 ? first : Long.parseLong(bounds[1]);
            if (first < 0 || last < first) {
                return null;
            }
            count = Math.addExact(count, last - first + 1);
        }
        return count <= 0
            ? null
            : count > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) count;
    }

    private static Integer minimumPositive(Integer left, Integer right) {
        if (left == null || left <= 0) {
            return positiveOrNull(right);
        }
        if (right == null || right <= 0) {
            return left;
        }
        return Math.min(left, right);
    }

    private static Integer positiveOrNull(Integer value) {
        return value == null || value <= 0 ? null : value;
    }

    public record ProcessorCandidates(
        Integer runtimeConstrainedLogicalCount,
        Integer affinityOrCpusetCount,
        Integer quotaCount,
        Integer explicitExecutorMaximum) {
        int minimumKnownPositive() {
            int minimum = Integer.MAX_VALUE;
            boolean found = false;
            for (Integer candidate : new Integer[] {
                runtimeConstrainedLogicalCount,
                affinityOrCpusetCount,
                quotaCount,
                explicitExecutorMaximum}) {
                if (candidate != null && candidate > 0) {
                    minimum = Math.min(minimum, candidate);
                    found = true;
                }
            }
            return found ? minimum : 1;
        }
    }

    public record ResolvedCapacity(
        ZLinkApplicationJobQueueProfile configuredProfile,
        OptionalLong configuredManualMax,
        int effectiveProcessorCount,
        long effectiveLimit) {
    }

    public record Snapshot(
        ZLinkApplicationJobQueueProfile configuredProfile,
        OptionalLong configuredManualMax,
        long effectiveProcessorCount,
        long effectiveMaxQueuedApplicationJobs,
        long reservedSupplyPermits,
        long queuedApplicationJobs,
        long permitsInUse,
        long peakPermitsInUse,
        long capacityWaiters,
        long capacityWaitCount,
        Duration capacityWaitDuration) {
    }

    public static final class Permit implements AutoCloseable {
        private final ZLinkApplicationJobQueue owner;
        private PermitState state = PermitState.RESERVED;

        private Permit(ZLinkApplicationJobQueue owner) {
            this.owner = owner;
        }

        /** Transfers this receive reservation to one queued application job. */
        public void queued() {
            owner.queued(this);
        }

        /** Returns capacity immediately before the handler's first instruction. */
        public void handlerStarted() {
            owner.release(this);
        }

        /**
         * Returns an ingress reservation only when no Framework queue accepted
         * it. Once transferred, the queued job owns the permit until handler
         * entry or terminal cleanup.
         */
        public void abandonReservation() {
            owner.abandonReservation(this);
        }

        @Override
        public void close() {
            owner.release(this);
        }
    }

    private enum PermitState { RESERVED, QUEUED, RELEASED }
    private enum WaiterState { WAITING, GRANTED, CANCELLED }

    private static final class Waiter {
        private final long startedAtNanos;
        private final WaitFuture future;
        private WaiterState state = WaiterState.WAITING;
        private Permit permit;
        private boolean durationRecorded;

        private Waiter(ZLinkApplicationJobQueue owner, long startedAtNanos) {
            this.startedAtNanos = startedAtNanos;
            this.future = new WaitFuture(owner, this);
        }

        private boolean waiting() {
            return state == WaiterState.WAITING;
        }
    }

    private static final class WaitFuture extends CompletableFuture<Permit> {
        private final ZLinkApplicationJobQueue owner;
        private final Waiter waiter;
        private boolean queueCancellation;

        private WaitFuture(ZLinkApplicationJobQueue owner, Waiter waiter) {
            this.owner = owner;
            this.waiter = waiter;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean cancelled = super.cancel(mayInterruptIfRunning);
            if (cancelled && !queueCancellation) {
                owner.cancelWaiter(waiter);
            }
            return cancelled;
        }

        private void cancelFromQueue() {
            queueCancellation = true;
            try {
                super.cancel(false);
            } finally {
                queueCancellation = false;
            }
        }
    }

    private record Grant(Waiter waiter, Permit permit) {
    }
}
