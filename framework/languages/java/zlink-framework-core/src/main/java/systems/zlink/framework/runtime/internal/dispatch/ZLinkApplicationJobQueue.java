package systems.zlink.framework.runtime.internal.dispatch;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;
import systems.zlink.framework.runtime.internal.metrics.ZLinkApplicationJobQueuePressureMetrics;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

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
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.LongSupplier;

/**
 * The single host-owned aggregate that admits ordinary inbound application jobs.
 *
 * <p>A permit starts as a receive/claim reservation, moves with the queued job, and is returned
 * immediately before the handler's first instruction. Capacity is handed directly to the oldest
 * live waiter; a new caller cannot barge ahead of an existing waiter. Local publication is posted
 * to its destination FIFO on the reservation turn, without waiting for the destination to run.
 */
public final class ZLinkApplicationJobQueue implements AutoCloseable {
    public static final int DEFAULT_PAUSE_THRESHOLD_PERCENT = 80;
    public static final int DEFAULT_RESUME_THRESHOLD_PERCENT = 60;
    private static final int PERCENT_SCALE = 100;
    private static final String CPU_AFFINITY_FIELD = "Cpus_allowed_list:";
    private static final String UNLIMITED_CPU_QUOTA = "max";

    private static final Path PROC_STATUS = Path.of("/proc/self/status");
    private static final List<Path> CPUSET_PATHS =
            List.of(
                    Path.of("/sys/fs/cgroup/cpuset.cpus.effective"),
                    Path.of("/sys/fs/cgroup/cpuset/cpuset.cpus"));
    private static final Path CPU_MAX = Path.of("/sys/fs/cgroup/cpu.max");
    private static final Path CPU_QUOTA = Path.of("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    private static final Path CPU_PERIOD = Path.of("/sys/fs/cgroup/cpu/cpu.cfs_period_us");

    private final Object lock = new Object();
    private final ZLinkApplicationJobQueueProfile configuredProfile;
    private final OptionalLong configuredManualMax;
    private final int configuredPauseThresholdPercent;
    private final int configuredResumeThresholdPercent;
    private final int effectiveProcessorCount;
    private final long effectiveLimit;
    private final long pausePermitCount;
    private final long resumePermitCount;
    private final LongSupplier nanoTime;
    private final ZLinkApplicationJobReceiveFlowController receiveFlow;
    private final ArrayDeque<Waiter> waiters = new ArrayDeque<>();
    private long reservedSupplyPermits;
    private long queuedApplicationJobs;
    private long permitsInUse;
    private long peakPermitsInUse;
    private long localWaiters;
    private boolean localBacklogLogged;
    private static final java.util.logging.Logger LOGGER =
            java.util.logging.Logger.getLogger(ZLinkApplicationJobQueue.class.getName());
    private long capacityWaitCount;
    private long capacityWaitDurationNanos;
    private long metricsEpoch;
    private long pressureTransitionSequence;
    private long pausedTransitionCount;
    private long runningTransitionCount;
    private ZLinkApplicationJobQueuePressureState pressureState =
            ZLinkApplicationJobQueuePressureState.RUNNING;
    private long pausedAtNanos = -1;
    private long cumulativePauseStartedAtNanos = -1;
    private long cumulativePauseDurationNanos;
    private long receiveFlowConfigurationFailureCount;
    private boolean closed;

    public ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile profile,
            OptionalLong manualMax,
            ProcessorCandidates processorCandidates) {
        this(
                profile,
                manualMax,
                processorCandidates,
                DEFAULT_PAUSE_THRESHOLD_PERCENT,
                DEFAULT_RESUME_THRESHOLD_PERCENT,
                System::nanoTime);
    }

    public ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile profile,
            OptionalLong manualMax,
            ProcessorCandidates processorCandidates,
            int pauseThresholdPercent,
            int resumeThresholdPercent) {
        this(
                profile,
                manualMax,
                processorCandidates,
                pauseThresholdPercent,
                resumeThresholdPercent,
                System::nanoTime);
    }

    ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile profile,
            OptionalLong manualMax,
            ProcessorCandidates processorCandidates,
            LongSupplier nanoTime) {
        this(
                profile,
                manualMax,
                processorCandidates,
                DEFAULT_PAUSE_THRESHOLD_PERCENT,
                DEFAULT_RESUME_THRESHOLD_PERCENT,
                nanoTime);
    }

    ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile profile,
            OptionalLong manualMax,
            ProcessorCandidates processorCandidates,
            int pauseThresholdPercent,
            int resumeThresholdPercent,
            LongSupplier nanoTime) {
        ResolvedCapacity capacity = resolveCapacity(profile, manualMax, processorCandidates);
        validateThresholds(pauseThresholdPercent, resumeThresholdPercent);
        this.configuredProfile = capacity.configuredProfile();
        this.configuredManualMax = capacity.configuredManualMax();
        this.configuredPauseThresholdPercent = pauseThresholdPercent;
        this.configuredResumeThresholdPercent = resumeThresholdPercent;
        this.effectiveProcessorCount = capacity.effectiveProcessorCount();
        this.effectiveLimit = capacity.effectiveLimit();
        this.pausePermitCount =
                ceilPercent(this.effectiveLimit, this.configuredPauseThresholdPercent);
        this.resumePermitCount =
                floorPercent(this.effectiveLimit, this.configuredResumeThresholdPercent);
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
        this.receiveFlow = new ZLinkApplicationJobReceiveFlowController(this);
    }

    public enum Origin {
        REMOTE,
        LOCAL
    }

    public CompletionStage<Permit> acquire(Origin origin) {
        Objects.requireNonNull(origin, "origin");
        PressureSnapshot transition = null;
        Permit permit = null;
        CompletionStage<Permit> result = null;
        long warning = 0;
        synchronized (lock) {
            if (closed) {
                return CompletableFuture.failedFuture(
                        new CancellationException("Application Job Queue is closed"));
            }
            if (waiters.isEmpty() && permitsInUse < effectiveLimit) {
                permit = reserveUnderLock();
                transition = evaluatePressureUnderLock();
            } else {
                Waiter waiter = new Waiter(this, null, nanoTime.getAsLong(), metricsEpoch, origin);
                waiters.addLast(waiter);
                capacityWaitCount = saturatingIncrement(capacityWaitCount);
                warning = updateLocalWaitersUnderLock(origin, 1);
                result = waiter.future;
            }
        }
        notifyPressureTransition(transition);
        if (warning != 0) logLocalBacklog(warning);
        return result != null ? result : CompletableFuture.completedFuture(permit);
    }

    /**
     * Posts publication to the destination FIFO on the reservation turn, including an immediate
     * grant. The post function must only enqueue the task, never run it inline. Its stage reports
     * destination rejection or shutdown so a dropped task returns its reservation.
     */
    public <T> CompletionStage<T> acquireAndPublish(
            Origin origin,
            Function<Runnable, CompletionStage<Void>> post,
            Function<Permit, CompletionStage<T>> publication) {
        Objects.requireNonNull(post, "post");
        Objects.requireNonNull(publication, "publication");
        return acquire(
                origin,
                post,
                acquisition -> {
                    CompletableFuture<T> completion =
                            new CompletableFuture<>() {
                                @Override
                                public boolean cancel(boolean mayInterruptIfRunning) {
                                    return acquisition.cancel(mayInterruptIfRunning);
                                }
                            };
                    acquisition
                            .thenCompose(publication)
                            .whenComplete(
                                    (value, failure) -> {
                                        if (failure == null) completion.complete(value);
                                        else completion.completeExceptionally(failure);
                                    });
                    return completion;
                });
    }

    private <T> T acquire(
            Origin origin,
            Function<Runnable, CompletionStage<Void>> post,
            Function<CompletableFuture<Permit>, T> registration) {
        Objects.requireNonNull(origin, "origin");
        T result;
        PressureSnapshot transition;
        Grant grant;
        long warning = 0;
        synchronized (lock) {
            Waiter waiter = new Waiter(this, post, nanoTime.getAsLong(), metricsEpoch, origin);
            result = registration.apply(waiter.future);
            if (closed) {
                waiter.future.cancelFromQueue();
                return result;
            }
            boolean waiting = !waiters.isEmpty() || permitsInUse >= effectiveLimit;
            waiters.addLast(waiter);
            if (waiting) {
                capacityWaitCount = saturatingIncrement(capacityWaitCount);
                warning = updateLocalWaitersUnderLock(origin, 1);
            } else waiter.durationRecorded = true;
            grant = waiting ? null : grantOldestUnderLock();
            transition = evaluatePressureUnderLock();
        }
        notifyPressureTransition(transition);
        finishGrant(grant);
        if (warning != 0) logLocalBacklog(warning);
        return result;
    }

    /**
     * Reserves one permit for a receive owner without holding its thread while it waits.
     *
     * <p>Returns the permit when capacity is free now. Otherwise the owner joins the same FIFO as
     * {@link #acquire()}, this method returns {@code null}, and the owner ends its current turn.
     * The grant runs {@code resume} with the permit on {@code executor}. The owner keeps the
     * acquisition until that task starts, so close can return a grant even when shutdown drops the
     * queued task. Queue close ends the receive turn.
     */
    public Permit acquireOrResume(
            Executor executor,
            Consumer<Permit> resume,
            java.util.concurrent.atomic.AtomicReference<CompletableFuture<Permit>> pending,
            Origin origin) {
        Objects.requireNonNull(executor, "executor");
        Objects.requireNonNull(resume, "resume");
        Objects.requireNonNull(pending, "pending");
        CompletableFuture<Permit> acquisition = acquire(origin).toCompletableFuture();
        if (acquisition.isDone()) {
            try {
                return acquisition.join();
            } catch (CancellationException closedQueue) {
                return null;
            }
        }
        pending.set(acquisition);
        acquisition.whenComplete(
                (permit, failure) -> {
                    try {
                        executor.execute(
                                () -> {
                                    if (pending.compareAndSet(acquisition, null)) {
                                        resume.accept(permit);
                                    }
                                });
                    } catch (java.util.concurrent.RejectedExecutionException stopped) {
                        if (permit != null && pending.compareAndSet(acquisition, null)) {
                            permit.abandonReservation();
                        }
                    }
                });
        return null;
    }

    /** Returns an unconsumed receive grant and cancels its FIFO wait when the owner closes. */
    public static void cancelPendingAcquire(
            java.util.concurrent.atomic.AtomicReference<CompletableFuture<Permit>> pending) {
        CompletableFuture<Permit> waiting = pending.getAndSet(null);
        if (waiting != null) {
            waiting.thenAccept(
                    permit -> {
                        if (permit != null) {
                            permit.abandonReservation();
                        }
                    });
            waiting.cancel(false);
        }
    }

    /** Blocking bridge for dedicated receive-loop threads. */
    public Permit acquireBlocking(Origin origin) throws InterruptedException {
        CompletableFuture<Permit> future = acquire(origin).toCompletableFuture();
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

    /**
     * Claims one receive-owner batch. The first permit observes the normal FIFO wait; the same
     * owner then takes only capacity that is immediately available without crossing an older
     * waiter.
     */
    public List<Permit> acquireBatchBlocking(Origin origin, int maximum)
            throws InterruptedException {
        if (maximum < 1) {
            throw new IllegalArgumentException("maximum must be positive");
        }
        ArrayList<Permit> permits = new ArrayList<>(maximum);
        permits.add(acquireBlocking(origin));
        PressureSnapshot transition = null;
        synchronized (lock) {
            while (!closed
                    && waiters.isEmpty()
                    && permits.size() < maximum
                    && permitsInUse < effectiveLimit) {
                permits.add(reserveUnderLock());
            }
            if (permits.size() > 1) {
                transition = evaluatePressureUnderLock();
            }
        }
        notifyPressureTransition(transition);
        return List.copyOf(permits);
    }

    public Snapshot snapshot() {
        synchronized (lock) {
            return new Snapshot(
                    configuredProfile,
                    configuredManualMax,
                    configuredPauseThresholdPercent,
                    configuredResumeThresholdPercent,
                    effectiveProcessorCount,
                    effectiveLimit,
                    pausePermitCount,
                    resumePermitCount,
                    reservedSupplyPermits,
                    queuedApplicationJobs,
                    permitsInUse,
                    peakPermitsInUse,
                    pressureState,
                    currentPauseDurationUnderLock(),
                    waiters.stream().filter(Waiter::waiting).count(),
                    capacityWaitCount,
                    Duration.ofNanos(capacityWaitDurationNanos));
        }
    }

    public void resetMetrics() {
        synchronized (lock) {
            metricsEpoch = saturatingIncrement(metricsEpoch);
            peakPermitsInUse = permitsInUse;
            capacityWaitCount = 0;
            capacityWaitDurationNanos = 0;
            pausedTransitionCount = 0;
            runningTransitionCount = 0;
            cumulativePauseDurationNanos = 0;
            if (pressureState == ZLinkApplicationJobQueuePressureState.PAUSED) {
                cumulativePauseStartedAtNanos = nanoTime.getAsLong();
            }
            receiveFlowConfigurationFailureCount = 0;
        }
        publishPressureMetrics();
    }

    @Override
    public void close() {
        List<Waiter> cancelled = new ArrayList<>();
        synchronized (lock) {
            if (closed) {
                return;
            }
            closed = true;
            // Fence registration and detach current targets at the same
            // linearization point as queue close. This only sets close flags;
            // each socket owner joins its registration before native close.
            receiveFlow.beginClose();
            while (!waiters.isEmpty()) {
                Waiter waiter = waiters.removeFirst();
                if (waiter.state != WaiterState.CANCELLED) {
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
            effective = Math.multiplyExact((long) processors, jobsPerProcessor(profile));
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

    public static ProcessorCandidates productionProcessorCandidates(Executor handlerExecutor) {
        int runtime = Math.max(1, Runtime.getRuntime().availableProcessors());
        Integer affinity = minimumPositive(readAffinityCount(), readCpusetCount());
        Integer quota = readQuotaCount();
        Integer executor = explicitExecutorMaximum(handlerExecutor);
        return new ProcessorCandidates(runtime, affinity, quota, executor);
    }

    private static long jobsPerProcessor(ZLinkApplicationJobQueueProfile profile) {
        return switch (profile) {
            case COMPACT -> 32L;
            case LOW_LATENCY -> 64L;
            case BALANCED -> 128L;
            case THROUGHPUT -> 256L;
        };
    }

    /** Registers a RouteMesh or ClientServer paired socket for receive flow. */
    public ZLinkApplicationJobReceiveFlowController.Registration registerReceiveFlowTarget(
            Consumer<systems.zlink.contracts.sockets.ReceiveFlowState> setter) {
        return receiveFlow.register(setter);
    }

    PressureSnapshot pressureSnapshot() {
        synchronized (lock) {
            return pressureSnapshotUnderLock();
        }
    }

    void recordReceiveFlowConfigurationFailure() {
        synchronized (lock) {
            receiveFlowConfigurationFailureCount =
                    saturatingIncrement(receiveFlowConfigurationFailureCount);
        }
        publishPressureMetrics();
    }

    private PressureSnapshot evaluatePressureUnderLock() {
        ZLinkApplicationJobQueuePressureState next = pressureState;
        if (pressureState == ZLinkApplicationJobQueuePressureState.RUNNING
                && permitsInUse >= pausePermitCount) {
            next = ZLinkApplicationJobQueuePressureState.PAUSED;
        } else if (pressureState == ZLinkApplicationJobQueuePressureState.PAUSED
                && permitsInUse <= resumePermitCount) {
            next = ZLinkApplicationJobQueuePressureState.RUNNING;
        }
        if (next == pressureState) {
            return null;
        }
        long now = nanoTime.getAsLong();
        pressureState = next;
        pressureTransitionSequence = saturatingIncrement(pressureTransitionSequence);
        if (next == ZLinkApplicationJobQueuePressureState.PAUSED) {
            pausedTransitionCount = saturatingIncrement(pausedTransitionCount);
            pausedAtNanos = now;
            cumulativePauseStartedAtNanos = now;
        } else if (pausedAtNanos >= 0) {
            runningTransitionCount = saturatingIncrement(runningTransitionCount);
            cumulativePauseDurationNanos =
                    saturatingAdd(
                            cumulativePauseDurationNanos,
                            elapsedSince(cumulativePauseStartedAtNanos, now));
            pausedAtNanos = -1;
            cumulativePauseStartedAtNanos = -1;
        }
        return pressureSnapshotUnderLock();
    }

    private void notifyPressureTransition(PressureSnapshot transition) {
        if (transition != null) {
            receiveFlow.onPressureTransition(transition);
            publishPressureMetrics();
        }
    }

    private PressureSnapshot pressureSnapshotUnderLock() {
        return new PressureSnapshot(
                pressureTransitionSequence,
                pressureState,
                currentPauseDurationUnderLock(),
                cumulativePauseDurationUnderLock());
    }

    private Duration currentPauseDurationUnderLock() {
        return pressureState == ZLinkApplicationJobQueuePressureState.PAUSED && pausedAtNanos >= 0
                ? Duration.ofNanos(elapsedSince(pausedAtNanos, nanoTime.getAsLong()))
                : Duration.ZERO;
    }

    /** Internal metrics projection; public status deliberately omits counters. */
    public ZLinkApplicationJobQueuePressureMetrics pressureMetrics() {
        synchronized (lock) {
            return new ZLinkApplicationJobQueuePressureMetrics(
                    pressureState,
                    runningTransitionCount,
                    pausedTransitionCount,
                    currentPauseDurationUnderLock(),
                    cumulativePauseDurationUnderLock(),
                    receiveFlowConfigurationFailureCount);
        }
    }

    private Duration cumulativePauseDurationUnderLock() {
        if (pressureState != ZLinkApplicationJobQueuePressureState.PAUSED) {
            return Duration.ofNanos(cumulativePauseDurationNanos);
        }
        return Duration.ofNanos(
                saturatingAdd(
                        cumulativePauseDurationNanos,
                        elapsedSince(cumulativePauseStartedAtNanos, nanoTime.getAsLong())));
    }

    private void publishPressureMetrics() {
        ZLinkRuntimeMetrics.publishApplicationJobQueuePressure(pressureMetrics());
    }

    private static long elapsedSince(long startedAtNanos, long nowNanos) {
        return startedAtNanos < 0 || nowNanos <= startedAtNanos ? 0L : nowNanos - startedAtNanos;
    }

    private static void validateThresholds(int pause, int resume) {
        if (pause < 1 || pause > PERCENT_SCALE) {
            throw new ZLinkConfigurationException(
                    "ApplicationJobQueuePauseThresholdPercent must be in 1.." + PERCENT_SCALE);
        }
        if (resume < 0 || resume >= PERCENT_SCALE) {
            throw new ZLinkConfigurationException(
                    "ApplicationJobQueueResumeThresholdPercent must be in 0.."
                            + (PERCENT_SCALE - 1));
        }
        if (resume >= pause) {
            throw new ZLinkConfigurationException(
                    "ApplicationJobQueueResumeThresholdPercent must be less than"
                            + " ApplicationJobQueuePauseThresholdPercent");
        }
    }

    private static long ceilPercent(long value, int percent) {
        return Math.addExact(Math.multiplyExact(value, percent), PERCENT_SCALE - 1L)
                / PERCENT_SCALE;
    }

    private static long floorPercent(long value, int percent) {
        return Math.multiplyExact(value, percent) / PERCENT_SCALE;
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
        PressureSnapshot transition;
        synchronized (lock) {
            if (permit.state == PermitState.RELEASED) {
                return;
            }
            grant = releaseUnderLock(permit);
            transition = evaluatePressureUnderLock();
        }
        notifyPressureTransition(transition);
        finishGrant(grant);
    }

    private void abandonReservation(Permit permit) {
        Grant grant;
        PressureSnapshot transition;
        synchronized (lock) {
            if (permit.state != PermitState.RESERVED) {
                return;
            }
            grant = releaseUnderLock(permit);
            transition = evaluatePressureUnderLock();
        }
        notifyPressureTransition(transition);
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
            Grant grant = new Grant(waiter, permit);
            if (waiter.post != null) {
                // Only post here. The destination FIFO runs publication after
                // this reservation turn, in the same order as the grants.
                try {
                    waiter.post
                            .apply(() -> deliverGrant(grant))
                            .whenComplete(
                                    (ignored, failure) -> {
                                        if (failure != null) {
                                            waiter.future.completeExceptionally(failure);
                                            permit.abandonReservation();
                                        }
                                    });
                } catch (java.util.concurrent.RejectedExecutionException stopped) {
                    waiter.future.completeExceptionally(stopped);
                    permit.abandonReservation();
                }
            }
            return grant;
        }
        return null;
    }

    private static void finishGrant(Grant grant) {
        if (grant != null && grant.waiter.post == null) {
            deliverGrant(grant);
        }
    }

    private static void deliverGrant(Grant grant) {
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

    private long updateLocalWaitersUnderLock(Origin origin, int delta) {
        if (origin != Origin.LOCAL) return 0;
        localWaiters += delta;
        if (localWaiters == 0) localBacklogLogged = false;
        if (!localBacklogLogged && localWaiters > effectiveLimit) {
            localBacklogLogged = true;
            return localWaiters;
        }
        return 0;
    }

    private void logLocalBacklog(long count) {
        try {
            LOGGER.log(
                    java.util.logging.Level.WARNING,
                    "zlink.runtime.host.local_job_backlog_exceeded source_kind=host source_name=zlink.runtime.host local_waiters={0} effective_maximum={1}",
                    new Object[] {count, effectiveLimit});
        } catch (RuntimeException failure) {
            new java.util.logging.ErrorManager()
                    .error(
                            "Host local backlog logger failed",
                            failure,
                            java.util.logging.ErrorManager.WRITE_FAILURE);
        }
    }

    private void recordWaitDurationUnderLock(Waiter waiter) {
        if (waiter.durationRecorded) {
            return;
        }
        waiter.durationRecorded = true;
        updateLocalWaitersUnderLock(waiter.origin, -1);
        if (waiter.metricsEpoch != metricsEpoch) {
            return;
        }
        long elapsed = Math.max(0L, nanoTime.getAsLong() - waiter.startedAtNanos);
        capacityWaitDurationNanos = saturatingAdd(capacityWaitDurationNanos, elapsed);
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
                if (line.startsWith(CPU_AFFINITY_FIELD)) {
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
            if (fields.length >= 2 && !UNLIMITED_CPU_QUOTA.equals(fields[0])) {
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
        return count <= 0 ? null : count > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) count;
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
            for (Integer candidate :
                    new Integer[] {
                        runtimeConstrainedLogicalCount,
                        affinityOrCpusetCount,
                        quotaCount,
                        explicitExecutorMaximum
                    }) {
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
            long effectiveLimit) {}

    public record Snapshot(
            ZLinkApplicationJobQueueProfile configuredProfile,
            OptionalLong configuredManualMax,
            int configuredPauseThresholdPercent,
            int configuredResumeThresholdPercent,
            long effectiveProcessorCount,
            long effectiveMaxQueuedApplicationJobs,
            long pausePermitCount,
            long resumePermitCount,
            long reservedSupplyPermits,
            long queuedApplicationJobs,
            long permitsInUse,
            long peakPermitsInUse,
            ZLinkApplicationJobQueuePressureState pressureState,
            Duration currentPauseDuration,
            long capacityWaiters,
            long capacityWaitCount,
            Duration capacityWaitDuration) {}

    record PressureSnapshot(
            long sequence,
            ZLinkApplicationJobQueuePressureState pressureState,
            Duration currentPauseDuration,
            Duration cumulativePauseDuration) {}

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
         * Returns an ingress reservation only when no Framework queue accepted it. Once
         * transferred, the queued job owns the permit until handler entry or terminal cleanup.
         */
        public void abandonReservation() {
            owner.abandonReservation(this);
        }

        @Override
        public void close() {
            owner.release(this);
        }
    }

    private enum PermitState {
        RESERVED,
        QUEUED,
        RELEASED
    }

    private enum WaiterState {
        WAITING,
        GRANTED,
        CANCELLED
    }

    private static final class Waiter {
        private final Origin origin;
        private final long startedAtNanos;
        private final long metricsEpoch;
        private final WaitFuture future;
        private final Function<Runnable, CompletionStage<Void>> post;
        private WaiterState state = WaiterState.WAITING;
        private Permit permit;
        private boolean durationRecorded;

        private Waiter(
                ZLinkApplicationJobQueue owner,
                Function<Runnable, CompletionStage<Void>> post,
                long startedAtNanos,
                long metricsEpoch,
                Origin origin) {
            this.origin = origin;
            this.post = post;
            this.startedAtNanos = startedAtNanos;
            this.metricsEpoch = metricsEpoch;
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

    private record Grant(Waiter waiter, Permit permit) {}
}
