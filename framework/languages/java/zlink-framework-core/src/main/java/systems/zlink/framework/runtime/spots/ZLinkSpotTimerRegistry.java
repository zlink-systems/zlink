package systems.zlink.framework.runtime.spots;
import java.util.logging.Logger;

import java.lang.reflect.InvocationTargetException;
import java.time.Duration;
import java.time.Instant;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

final class ZLinkSpotTimerRegistry implements AutoCloseable {
    private final String spotId;
    private final ScheduledExecutorService executor;
    private final ZLinkHandlerInstanceOwner handlers;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final String sourceName;
    private final Dispatch dispatch;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ManagedTimer> timers = new LinkedHashMap<>();
    private Object spot;
    private boolean frozen;

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    ZLinkSpotTimerRegistry(
        String spotId,
        ScheduledExecutorService executor,
        ZLinkHandlerActivator handlerFactory,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        String sourceName,
        Dispatch dispatch) {
        this(
            spotId,
            executor,
            new ZLinkHandlerInstanceOwner(handlerFactory),
            suspendHandlerInvokers,
            eventDispatcher,
            sourceName,
            dispatch);
    }

    ZLinkSpotTimerRegistry(
        String spotId,
        ScheduledExecutorService executor,
        ZLinkHandlerInstanceOwner handlers,
        List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        String sourceName,
        Dispatch dispatch) {
        this.spotId = spotId;
        this.executor = executor;
        this.handlers = handlers;
        this.suspendHandlerInvokers = suspendHandlerInvokers;
        this.eventDispatcher = eventDispatcher;
        this.sourceName = sourceName;
        this.dispatch = dispatch;
    }

    void setSpot(Object spot) {
        inStateLane(() -> {
            this.spot = spot;
            return null;
        });
    }

    CompletionStage<ZLinkTimer> add(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options) {
        AddedTimer added = inStateLane(() -> addCore(
            name, period, handlerType, options));
        if (added.previous() != null) {
            added.previous().close();
        }
        added.timer().start();
        return CompletableFuture.completedFuture(added.timer());
    }

    private AddedTimer addCore(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options) {
        if (frozen) {
            throw new ZLinkConfigurationException(
                "timer registration is sealed for relocation");
        }
        if (name == null || name.isBlank()) {
            throw new ZLinkConfigurationException("timer name is required");
        }
        if (period == null || period.isNegative() || period.isZero()) {
            throw new ZLinkConfigurationException("timer period must be positive");
        }
        ZLinkTimerOptions timerOptions = options == null
            ? new ZLinkTimerOptions(
                ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
                1,
                false)
            : options;
        if (timerOptions.overrunPolicy() == null) {
            throw new ZLinkConfigurationException("timer overrun policy is required");
        }
        if (timerOptions.overrunPolicy() == ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED
            && timerOptions.maxCatchUpTicks() <= 0) {
            throw new ZLinkConfigurationException(
                "timer maxCatchUpTicks must be greater than zero");
        }
        ManagedTimer timer = new ManagedTimer(name, period, handlerType, timerOptions);
        ManagedTimer previous = timers.put(name, timer);
        return new AddedTimer(timer, previous);
    }

    FrozenTimers freeze() {
        FreezeState state = inStateLane(this::freezeCore);
        state.futures().forEach(ZLinkSpotTimerRegistry::cancel);
        return state.snapshot();
    }

    private FreezeState freezeCore() {
        List<ScheduledFuture<?>> futures = List.of();
        if (!frozen) {
            frozen = true;
            futures = timers.values().stream()
                .map(ManagedTimer::freezeCore)
                .flatMap(Optional::stream)
                .toList();
        }
        FrozenTimers snapshot = new FrozenTimers(timers.values().stream()
            .filter(timer -> !timer.isDisposedCore())
            .map(ManagedTimer::snapshot)
            .sorted(Comparator.comparing(TimerSnapshot::name))
            .toList());
        return new FreezeState(snapshot, futures);
    }

    void resume() {
        List<ManagedTimer> current = inStateLane(() -> {
            if (!frozen) {
                return List.of();
            }
            frozen = false;
            return List.copyOf(timers.values());
        });
        current.forEach(ManagedTimer::resume);
    }

    void restore(FrozenTimers state) {
        stageRestore(state);
        publishStagedRestore();
    }

    boolean hasActiveTimers() {
        return inStateLane(() -> timers.values().stream()
            .anyMatch(timer -> !timer.isDisposedCore()));
    }

    void stageRestore(FrozenTimers state) {
        RestoreState previous = inStateLane(() -> stageRestoreCore(state));
        previous.futures().forEach(ZLinkSpotTimerRegistry::cancel);
    }

    private RestoreState stageRestoreCore(FrozenTimers state) {
        List<ManagedTimer> previous = List.copyOf(timers.values());
        List<ScheduledFuture<?>> futures = previous.stream()
            .map(ManagedTimer::disposeCore)
            .flatMap(Optional::stream)
            .toList();
        timers.clear();
        frozen = true;
        for (TimerSnapshot snapshot : state.timers()) {
            ManagedTimer timer = new ManagedTimer(snapshot);
            if (timers.put(snapshot.name(), timer) != null) {
                throw new ZLinkConfigurationException(
                    "duplicate timer in relocation envelope: " + snapshot.name());
            }
        }
        return new RestoreState(futures);
    }

    void publishStagedRestore() {
        List<ManagedTimer> current = inStateLane(() -> {
            if (!frozen) {
                throw new IllegalStateException(
                    "timer relocation staging is not active");
            }
            frozen = false;
            return List.copyOf(timers.values());
        });
        current.forEach(ManagedTimer::resume);
    }

    @Override
    public void close() {
        List<ScheduledFuture<?>> futures = inStateLane(() -> {
            List<ScheduledFuture<?>> current = timers.values().stream()
                .map(ManagedTimer::disposeCore)
                .flatMap(Optional::stream)
                .toList();
            timers.clear();
            frozen = false;
            return current;
        });
        futures.forEach(ZLinkSpotTimerRegistry::cancel);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> invokeHandler(
        Object handlerSpot,
        Class<?> handlerType,
        ZLinkTimerTick tick) {
        try {
            Object handler = handlers.instance(handlerType);
            return ZLinkHandlerMethodInvoker
                .invokeHandler(
                    handler,
                    "handle",
                    new Object[] {handlerSpot, tick},
                    suspendHandlerInvokers)
                .thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "failed to create timer handler: " + handlerType.getName(),
                ex));
        }
    }

    private void publishFailure(
        ManagedTimer timer,
        ZLinkTimerTick tick,
        Throwable error,
        boolean stopped) {
        Throwable failure = unwrap(error);
        Logger.getLogger(
            ZLinkSpotTimerRegistry.class.getName()).warning(
                (stopped ? "Spot timer stopped" : "Spot timer handler failed")
                    + ": source=" + sourceName
                    + ", spotId=" + spotId
                    + ", timer=" + timer.name
                    + ", handler=" + timer.handlerType.getName()
                    + ", deliveryIndex=" + tick.deliveryIndex()
                    + ", scheduledIndex=" + tick.scheduledIndex()
                    + ", exception=" + failure.getClass().getName()
                    + ", message=" + String.valueOf(failure.getMessage()));
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
            || current instanceof InvocationTargetException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private final class ManagedTimer implements ZLinkTimer {
        private final String name;
        private final Class<?> handlerType;
        private final ZLinkTimerOptions options;
        private final ZLinkSpotTimerSchedule schedule;
        private boolean disposed;
        private ScheduledFuture<?> future;
        private ScheduleAttempt scheduled;
        private Instant nextScheduledAt;
        private ZLinkSpotTimerSchedule.PendingTick pendingTick;

        ManagedTimer(
            String name,
            Duration period,
            Class<?> handlerType,
            ZLinkTimerOptions options) {
            this.name = name;
            this.handlerType = handlerType;
            this.options = options;
            this.schedule = new ZLinkSpotTimerSchedule(name, period, options);
        }

        ManagedTimer(TimerSnapshot snapshot) {
            this.name = snapshot.name();
            this.handlerType = snapshot.handlerType();
            this.options = snapshot.schedule().options();
            this.schedule = new ZLinkSpotTimerSchedule(snapshot.schedule());
            this.nextScheduledAt = snapshot.nextScheduledAt().orElse(null);
            this.pendingTick = snapshot.pendingTick().orElse(null);
        }

        void start() {
            Long delay = inStateLane(() -> disposed
                ? null
                : schedule.initialDelayNanos());
            if (delay != null) {
                scheduleNext(delay);
            }
        }

        private void scheduleNext(long delayNanos) {
            SchedulePlan plan = inStateLane(() -> {
                if (disposed || frozen) {
                    return null;
                }
                long boundedDelay = Math.max(0L, delayNanos);
                ScheduleAttempt attempt = new ScheduleAttempt();
                scheduled = attempt;
                nextScheduledAt = safePlusNanos(Instant.now(), boundedDelay);
                return new SchedulePlan(attempt, boundedDelay);
            });
            if (plan == null) {
                return;
            }
            ScheduledFuture<?> task = executor.schedule(
                () -> run(plan.attempt()),
                plan.delayNanos(),
                TimeUnit.NANOSECONDS);
            boolean cancel = inStateLane(() -> {
                if (disposed || frozen || scheduled != plan.attempt()) {
                    return true;
                }
                future = task;
                return false;
            });
            if (cancel) {
                task.cancel(false);
            }
        }

        private void run(ScheduleAttempt attempt) {
            ZLinkSpotTimerSchedule.PendingTick selected = inStateLane(() -> {
                if (disposed || frozen || scheduled != attempt) {
                    return null;
                }
                scheduled = null;
                future = null;
                ZLinkSpotTimerSchedule.PendingTick next = schedule.nextTick(
                    schedule.startedElapsedNanos(),
                    Instant.now());
                pendingTick = next;
                nextScheduledAt = null;
                return next;
            });
            if (selected != null) {
                dispatchPending(selected);
            }
        }

        private void dispatchPending(
            ZLinkSpotTimerSchedule.PendingTick selected) {
            ZLinkTimerTick tick = selected.tick();
            dispatch.enqueue(name, () -> {
                HandlerInvocation invocation = inStateLane(() -> {
                    if (disposed || frozen || pendingTick != selected) {
                        return null;
                    }
                    return new HandlerInvocation(spot, handlerType);
                });
                return invocation == null
                    ? CompletableFuture.completedFuture(null)
                    : invokeHandler(invocation.spot(), invocation.handlerType(), tick);
            })
                .whenComplete((ignored, error) -> {
                    DispatchResult result = inStateLane(() -> {
                        boolean stillCurrent = !frozen
                            && !disposed
                            && pendingTick == selected;
                        if (!stillCurrent) {
                            return new DispatchResult(false, false, false);
                        }
                        boolean stopped = error != null
                            && options.stopOnUnhandledException();
                        if (error == null) {
                            schedule.markDelivered(selected);
                        }
                        pendingTick = null;
                        return new DispatchResult(true, stopped, !stopped);
                    });
                    if (!result.stillCurrent()) {
                        return;
                    }
                    if (error != null) {
                        if (result.stopped()) {
                            close();
                        }
                        publishFailure(this, tick, error, result.stopped());
                    }
                    if (result.reschedule()) {
                        scheduleAfterDispatch();
                    }
                });
        }

        private void scheduleAfterDispatch() {
            Long delay = inStateLane(() -> disposed
                ? null
                : schedule.delayAfterDispatchNanos());
            if (delay != null) {
                scheduleNext(delay);
            }
        }

        Optional<ScheduledFuture<?>> freezeCore() {
            ScheduledFuture<?> current = future;
            future = null;
            scheduled = null;
            return Optional.ofNullable(current);
        }

        void resume() {
            ResumePlan plan = inStateLane(() -> {
                if (disposed) {
                    return null;
                }
                if (pendingTick != null) {
                    return new ResumePlan(pendingTick, 0L);
                }
                Instant scheduledAt = nextScheduledAt;
                long delay = scheduledAt == null
                    ? schedule.delayAfterDispatchNanos()
                    : nanosUntil(scheduledAt);
                return new ResumePlan(null, delay);
            });
            if (plan == null) {
                return;
            }
            if (plan.pendingTick() != null) {
                dispatchPending(plan.pendingTick());
            } else {
                scheduleNext(plan.delayNanos());
            }
        }

        TimerSnapshot snapshot() {
            return new TimerSnapshot(
                name,
                handlerType,
                schedule.snapshot(),
                Optional.ofNullable(nextScheduledAt),
                Optional.ofNullable(pendingTick));
        }

        boolean isDisposedCore() {
            return disposed;
        }

        Optional<ScheduledFuture<?>> disposeCore() {
            disposed = true;
            ScheduledFuture<?> current = future;
            future = null;
            scheduled = null;
            return Optional.ofNullable(current);
        }

        @Override
        public boolean isDisposed() {
            return inStateLane(this::isDisposedCore);
        }

        @Override
        public CompletionStage<Void> cancel() {
            Optional<ScheduledFuture<?>> task = inStateLane(() -> {
                timers.remove(name, this);
                return disposeCore();
            });
            task.ifPresent(ZLinkSpotTimerRegistry::cancel);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void close() {
            Optional<ScheduledFuture<?>> task = inStateLane(this::disposeCore);
            task.ifPresent(ZLinkSpotTimerRegistry::cancel);
        }
    }

    private static void cancel(ScheduledFuture<?> task) {
        if (task != null) {
            task.cancel(false);
        }
    }

    private static final class ScheduleAttempt {}

    private record AddedTimer(ManagedTimer timer, ManagedTimer previous) {}

    private record FreezeState(
        FrozenTimers snapshot,
        List<ScheduledFuture<?>> futures) {}

    private record RestoreState(List<ScheduledFuture<?>> futures) {}

    private record SchedulePlan(ScheduleAttempt attempt, long delayNanos) {}

    private record ResumePlan(
        ZLinkSpotTimerSchedule.PendingTick pendingTick,
        long delayNanos) {}

    private record HandlerInvocation(Object spot, Class<?> handlerType) {}

    private record DispatchResult(
        boolean stillCurrent,
        boolean stopped,
        boolean reschedule) {}

    record FrozenTimers(List<TimerSnapshot> timers) {
        FrozenTimers {
            timers = List.copyOf(timers);
            long distinctNames = timers.stream()
                .map(TimerSnapshot::name)
                .distinct()
                .count();
            if (distinctNames != timers.size()) {
                throw new ZLinkConfigurationException(
                    "duplicate timer in relocation envelope");
            }
        }
    }

    record TimerSnapshot(
        String name,
        Class<?> handlerType,
        ZLinkSpotTimerSchedule.State schedule,
        Optional<Instant> nextScheduledAt,
        Optional<ZLinkSpotTimerSchedule.PendingTick> pendingTick) {
        TimerSnapshot {
            if (name == null
                || name.isBlank()
                || handlerType == null
                || schedule == null
                || nextScheduledAt == null
                || pendingTick == null) {
                throw new ZLinkConfigurationException(
                    "invalid timer relocation state");
            }
            if (nextScheduledAt.isPresent() == pendingTick.isPresent()) {
                throw new ZLinkConfigurationException(
                    "timer relocation state must contain exactly one next action");
            }
        }
    }

    private static long nanosUntil(Instant deadline) {
        try {
            return Duration.between(Instant.now(), deadline).toNanos();
        } catch (ArithmeticException error) {
            return deadline.isAfter(Instant.now())
                ? Long.MAX_VALUE
                : Long.MIN_VALUE;
        }
    }

    private static Instant safePlusNanos(Instant now, long nanos) {
        try {
            return now.plusNanos(nanos);
        } catch (RuntimeException error) {
            return Instant.MAX;
        }
    }

    @FunctionalInterface
    interface Dispatch {
        CompletionStage<Void> enqueue(
            String timerName,
            Supplier<CompletionStage<Void>> operation);
    }
}
