package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

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
import java.util.logging.Logger;

final class ZLinkSpotTimerRegistry implements AutoCloseable {
    private final String spotId;
    private final ScheduledExecutorService executor;
    private final ZLinkHandlerInstanceOwner handlers;
    private final List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers;
    private final String sourceName;
    private final Dispatch dispatch;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ManagedTimer> timers = new LinkedHashMap<>();
    private Object spot;
    private Phase phase = Phase.RUNNING;
    private boolean initializing;

    private enum Phase {
        RUNNING,
        FROZEN,
        RESTORING
    }

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
            String sourceName,
            Dispatch dispatch) {
        this(
                spotId,
                executor,
                new ZLinkHandlerInstanceOwner(handlerFactory),
                suspendHandlerInvokers,
                sourceName,
                dispatch);
    }

    ZLinkSpotTimerRegistry(
            String spotId,
            ScheduledExecutorService executor,
            ZLinkHandlerInstanceOwner handlers,
            List<ZLinkSuspendInvocationAdapter> suspendHandlerInvokers,
            String sourceName,
            Dispatch dispatch) {
        this.spotId = spotId;
        this.executor = executor;
        this.handlers = handlers;
        this.suspendHandlerInvokers = suspendHandlerInvokers;
        this.sourceName = sourceName;
        this.dispatch = dispatch;
    }

    void setSpot(Object spot) {
        inStateLane(
                () -> {
                    this.spot = spot;
                    return null;
                });
    }

    CompletionStage<ZLinkTimer> add(
            String name, Duration period, Class<?> handlerType, ZLinkTimerOptions options) {
        return stateLane
                .runAsync(() -> addCore(name, period, handlerType, options))
                .thenApplyAsync(
                        added -> {
                            if (added.previous() != null) added.previous().close();
                            added.timer().publishSchedule(added.schedule());
                            return added.timer();
                        });
    }

    private AddedTimer addCore(
            String name, Duration period, Class<?> handlerType, ZLinkTimerOptions options) {
        if (name == null || name.isBlank()) {
            throw new ZLinkConfigurationException("timer name is required");
        }
        if (period == null || period.isNegative() || period.isZero()) {
            throw new ZLinkConfigurationException("timer period must be positive");
        }
        ZLinkTimerOptions timerOptions =
                options == null
                        ? new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false)
                        : options;
        if (timerOptions.overrunPolicy() == null) {
            throw new ZLinkConfigurationException("timer overrun policy is required");
        }
        if (timerOptions.overrunPolicy() == ZLinkTimerOverrunPolicy.CATCH_UP_BOUNDED
                && timerOptions.maxCatchUpTicks() <= 0) {
            throw new ZLinkConfigurationException(
                    "timer maxCatchUpTicks must be greater than zero");
        }
        if (phase == Phase.FROZEN) {
            throw new ZLinkConfigurationException("timer registration is sealed for relocation");
        }
        ManagedTimer existing = timers.get(name);
        if (initializing && existing != null && existing.restored) {
            if (existing.handlerType != handlerType) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                        "restored timer handler type mismatch: " + name);
            }
            return new AddedTimer(existing, null, null);
        }
        ManagedTimer timer = new ManagedTimer(name, period, handlerType, timerOptions);
        ManagedTimer previous = timers.put(name, timer);
        return new AddedTimer(
                timer, previous, timer.prepareScheduleCore(timer.schedule.initialDelayNanos()));
    }

    FrozenTimers freeze() {
        FreezeState state = inStateLane(this::freezeCore);
        state.futures().forEach(ZLinkSpotTimerRegistry::cancel);
        return state.snapshot();
    }

    private FreezeState freezeCore() {
        List<ScheduledFuture<?>> futures = List.of();
        if (phase == Phase.RUNNING) {
            phase = Phase.FROZEN;
            futures =
                    timers.values().stream()
                            .map(ManagedTimer::freezeCore)
                            .flatMap(Optional::stream)
                            .toList();
        }
        FrozenTimers snapshot =
                new FrozenTimers(
                        timers.values().stream()
                                .filter(timer -> !timer.isDisposedCore())
                                .map(ManagedTimer::snapshot)
                                .sorted(Comparator.comparing(TimerSnapshot::name))
                                .toList());
        return new FreezeState(snapshot, futures);
    }

    void resume() {
        List<ManagedTimer> current =
                inStateLane(
                        () -> {
                            if (phase == Phase.RUNNING) {
                                return List.of();
                            }
                            phase = Phase.RUNNING;
                            return List.copyOf(timers.values());
                        });
        current.forEach(ManagedTimer::resume);
    }

    void restore(FrozenTimers state) {
        stageRestore(state);
        publishStagedRestore();
    }

    boolean hasActiveTimers() {
        return inStateLane(
                () -> timers.values().stream().anyMatch(timer -> !timer.isDisposedCore()));
    }

    void stageRestore(FrozenTimers state) {
        List<ManagedTimer> previous = inStateLane(() -> stageRestoreCore(state));
        previous.forEach(ManagedTimer::close);
    }

    private List<ManagedTimer> stageRestoreCore(FrozenTimers state) {
        List<ManagedTimer> previous = List.copyOf(timers.values());
        timers.clear();
        phase = Phase.RESTORING;
        for (TimerSnapshot snapshot : state.timers()) {
            ManagedTimer timer = new ManagedTimer(snapshot);
            timers.put(snapshot.name(), timer);
        }
        return previous;
    }

    <T> CompletionStage<T> runRelocationInitialization(Supplier<CompletionStage<T>> operation) {
        return stateLane
                .runAsync(
                        () -> {
                            if (phase != Phase.RESTORING || initializing) {
                                throw new IllegalStateException(
                                        "timer relocation staging is not active");
                            }
                            initializing = true;
                            return null;
                        })
                .thenComposeAsync(
                        ignored -> {
                            CompletionStage<T> result;
                            try {
                                result =
                                        java.util.Objects.requireNonNull(
                                                operation.get(), "initialization result");
                            } catch (RuntimeException | Error failure) {
                                result = CompletableFuture.failedFuture(failure);
                            }
                            return result.handle(
                                            (value, failure) ->
                                                    stateLane.runAsync(
                                                            () -> {
                                                                initializing = false;
                                                                if (failure != null) {
                                                                    throw new CompletionException(
                                                                            unwrap(failure));
                                                                }
                                                                return value;
                                                            }))
                                    .thenCompose(stage -> stage);
                        });
    }

    void publishStagedRestore() {
        List<ManagedTimer> current =
                inStateLane(
                        () -> {
                            if (phase != Phase.RESTORING) {
                                throw new IllegalStateException(
                                        "timer relocation staging is not active");
                            }
                            phase = Phase.RUNNING;
                            return List.copyOf(timers.values());
                        });
        current.forEach(ManagedTimer::resume);
    }

    @Override
    public void close() {
        List<ManagedTimer> current =
                inStateLane(
                        () -> {
                            List<ManagedTimer> active = List.copyOf(timers.values());
                            timers.clear();
                            phase = Phase.RUNNING;
                            return active;
                        });
        current.forEach(ManagedTimer::close);
    }

    CompletionStage<Void> closeAsync() {
        return stateLane
                .runAsync(
                        () -> {
                            List<ManagedTimer> active = List.copyOf(timers.values());
                            timers.clear();
                            phase = Phase.RUNNING;
                            return active;
                        })
                .thenComposeAsync(
                        current ->
                                CompletableFuture.allOf(
                                        current.stream()
                                                .map(ManagedTimer::getOrStartFinalizationAsync)
                                                .map(CompletionStage::toCompletableFuture)
                                                .toArray(CompletableFuture[]::new)));
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> invokeHandler(
            Object handlerSpot,
            Class<?> handlerType,
            ZLinkHandlerMethodInvoker.PreparedMethod invocation,
            ZLinkTimerTick tick) {
        try {
            Object handler = handlers.instance(handlerType);
            return invocation.invoke(handler, handlerSpot, tick).thenApply(ignored -> null);
        } catch (RuntimeException ex) {
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "failed to create timer handler: " + handlerType.getName(), ex));
        }
    }

    private void publishFailure(
            ManagedTimer timer, ZLinkTimerTick tick, Throwable error, boolean stopped) {
        Throwable failure = unwrap(error);
        Logger.getLogger(ZLinkSpotTimerRegistry.class.getName())
                .warning(
                        (stopped ? "Spot timer stopped" : "Spot timer handler failed")
                                + ": source="
                                + sourceName
                                + ", spotId="
                                + spotId
                                + ", timer="
                                + timer.name
                                + ", handler="
                                + timer.handlerType.getName()
                                + ", deliveryIndex="
                                + tick.deliveryIndex()
                                + ", scheduledIndex="
                                + tick.scheduledIndex()
                                + ", exception="
                                + failure.getClass().getName()
                                + ", message="
                                + String.valueOf(failure.getMessage()));
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

    private ZLinkHandlerMethodInvoker.PreparedMethod prepareInvocation(Class<?> handlerType) {
        var method =
                ZLinkHandlerMethodInvoker.requireHandlerMethodForTypes(
                        handlerType,
                        ZLinkHandlerMethodInvoker.HANDLER_METHOD_NAME,
                        null,
                        ZLinkTimerTick.class);
        return ZLinkHandlerMethodInvoker.prepare(method, suspendHandlerInvokers, 0, 1);
    }

    private final class ManagedTimer implements ZLinkTimer {
        private final String name;
        private final boolean restored;
        private final Class<?> handlerType;
        private final ZLinkHandlerMethodInvoker.PreparedMethod invocation;
        private final ZLinkTimerOptions options;
        private final ZLinkSpotTimerSchedule schedule;
        private volatile boolean disposed;
        private ScheduledFuture<?> future;
        private ScheduleAttempt scheduled;
        private Instant nextScheduledAt;
        private ZLinkSpotTimerSchedule.PendingTick pendingTick;
        private ActiveDispatch activeDispatch;
        private final CompletableFuture<Void> finalization = new CompletableFuture<>();

        ManagedTimer(
                String name, Duration period, Class<?> handlerType, ZLinkTimerOptions options) {
            handlers.prepare(handlerType);
            this.restored = false;
            this.name = name;
            this.handlerType = handlerType;
            this.invocation = prepareInvocation(handlerType);
            this.options = options;
            this.schedule = new ZLinkSpotTimerSchedule(name, period, options);
            this.nextScheduledAt = safePlusNanos(Instant.now(), schedule.initialDelayNanos());
        }

        ManagedTimer(TimerSnapshot snapshot) {
            handlers.prepare(snapshot.handlerType());
            this.restored = true;
            this.name = snapshot.name();
            this.handlerType = snapshot.handlerType();
            this.invocation = prepareInvocation(handlerType);
            this.options = snapshot.schedule().options();
            this.schedule = new ZLinkSpotTimerSchedule(snapshot.schedule());
            this.nextScheduledAt = snapshot.nextScheduledAt().orElse(null);
            this.pendingTick = snapshot.pendingTick().orElse(null);
        }

        private void scheduleNext(long delayNanos) {
            SchedulePlan plan = inStateLane(() -> prepareScheduleCore(delayNanos));
            publishSchedule(plan);
        }

        private SchedulePlan prepareScheduleCore(long delayNanos) {
            if (disposed || phase != Phase.RUNNING) {
                return null;
            }
            long boundedDelay = Math.max(0L, delayNanos);
            ScheduleAttempt attempt = new ScheduleAttempt();
            scheduled = attempt;
            nextScheduledAt = safePlusNanos(Instant.now(), boundedDelay);
            return new SchedulePlan(attempt, boundedDelay);
        }

        private void publishSchedule(SchedulePlan plan) {
            if (plan == null) {
                return;
            }
            ScheduledFuture<?> task =
                    executor.schedule(
                            () -> run(plan.attempt()), plan.delayNanos(), TimeUnit.NANOSECONDS);
            boolean cancel =
                    inStateLane(
                            () -> {
                                if (disposed
                                        || phase != Phase.RUNNING
                                        || scheduled != plan.attempt()) {
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
            ZLinkSpotTimerSchedule.PendingTick selected =
                    inStateLane(
                            () -> {
                                if (disposed || phase != Phase.RUNNING || scheduled != attempt) {
                                    return null;
                                }
                                scheduled = null;
                                future = null;
                                ZLinkSpotTimerSchedule.PendingTick next =
                                        schedule.nextTick(
                                                schedule.startedElapsedNanos(), Instant.now());
                                pendingTick = next;
                                nextScheduledAt = null;
                                return next;
                            });
            if (selected != null) {
                dispatchPending(selected);
            }
        }

        private void dispatchPending(ZLinkSpotTimerSchedule.PendingTick selected) {
            ZLinkTimerTick tick = selected.tick();
            dispatch.enqueue(name, () -> dispatchClaimed(selected, tick))
                    .whenComplete(
                            (ignored, error) -> {
                                if (error != null) {
                                    stateLane
                                            .runAsync(
                                                    () ->
                                                            activeDispatch == null
                                                                    || activeDispatch.tick()
                                                                            != selected)
                                            .whenComplete(
                                                    (notStarted, ownerFailure) -> {
                                                        if (ownerFailure != null) {
                                                            reportCompletionFailure(ownerFailure);
                                                        } else if (notStarted) {
                                                            completeDispatch(
                                                                    selected, tick, error, null);
                                                        }
                                                    });
                                }
                            });
        }

        private CompletionStage<Void> dispatchClaimed(
                ZLinkSpotTimerSchedule.PendingTick selected, ZLinkTimerTick tick) {
            CompletionStage<HandlerInvocation> claim =
                    stateLane.tryRunNow(
                            () -> {
                                if (disposed || phase != Phase.RUNNING || pendingTick != selected)
                                    return null;
                                activeDispatch =
                                        new ActiveDispatch(selected, new CompletableFuture<>());
                                schedule.markDelivered(selected);
                                return new HandlerInvocation(
                                        spot, handlerType, activeDispatch.completion());
                            });
            if (claim == null) {
                return ZLinkSerialExecutionQueue.yieldCurrent(stateLane.runAsync(() -> null))
                        .thenCompose(ignored -> dispatchClaimed(selected, tick));
            }
            return claim.thenCompose(
                    invocation ->
                            invocation == null
                                    ? CompletableFuture.completedFuture(null)
                                    : invokeHandler(
                                                    invocation.spot(),
                                                    invocation.handlerType(),
                                                    this.invocation,
                                                    tick)
                                            .whenComplete(
                                                    (ignored, error) ->
                                                            completeDispatch(
                                                                    selected,
                                                                    tick,
                                                                    error,
                                                                    invocation.completion())));
        }

        private void completeDispatch(
                ZLinkSpotTimerSchedule.PendingTick selected,
                ZLinkTimerTick tick,
                Throwable error,
                CompletableFuture<Void> dispatchCompletionFallback) {
            CompletionStage<DispatchResult> preparation;
            try {
                preparation =
                        stateLane.runAsync(
                                () -> {
                                    boolean stillCurrent =
                                            phase == Phase.RUNNING
                                                    && !disposed
                                                    && pendingTick == selected;
                                    CompletableFuture<Void> dispatchCompletion =
                                            activeDispatch != null
                                                            && activeDispatch.tick() == selected
                                                    ? activeDispatch.completion()
                                                    : null;
                                    if (!stillCurrent) {
                                        return new DispatchResult(
                                                false, null, null, dispatchCompletion);
                                    }
                                    boolean stopped =
                                            error != null && options.stopOnUnhandledException();
                                    SchedulePlan next = null;
                                    FinalizationPlan stopFinalization = null;
                                    if (stopped) {
                                        stopFinalization = finalizationPlanOnLane();
                                    } else {
                                        pendingTick = null;
                                        next =
                                                prepareScheduleCore(
                                                        schedule.delayAfterDispatchNanos());
                                    }
                                    return new DispatchResult(
                                            true, stopFinalization, next, dispatchCompletion);
                                });
            } catch (RuntimeException failure) {
                if (dispatchCompletionFallback != null) {
                    dispatchCompletionFallback.completeExceptionally(failure);
                }
                reportCompletionFailure(failure);
                return;
            }
            preparation.whenComplete(
                    (result, failure) -> {
                        if (failure != null) {
                            if (dispatchCompletionFallback != null) {
                                dispatchCompletionFallback.completeExceptionally(failure);
                            }
                            reportCompletionFailure(failure);
                            return;
                        }
                        publishDispatchCompletion(result, selected, tick, error);
                    });
        }

        private void reportCompletionFailure(Throwable failure) {
            Logger.getLogger(ZLinkSpotTimerRegistry.class.getName())
                    .log(
                            java.util.logging.Level.WARNING,
                            "Timer completion ownership failed",
                            failure);
        }

        private void publishDispatchCompletion(
                DispatchResult result,
                ZLinkSpotTimerSchedule.PendingTick selected,
                ZLinkTimerTick tick,
                Throwable error) {
            Throwable completionFailure = null;
            try {
                if (result.stillCurrent() && error != null) {
                    if (result.stopFinalization() != null && result.stopFinalization().start()) {
                        completeFinalization(result.stopFinalization());
                    }
                    publishFailure(this, tick, error, result.stopFinalization() != null);
                }
            } catch (RuntimeException | Error failure) {
                completionFailure = failure;
            }
            if (result.dispatchCompletion() != null) {
                if (completionFailure == null) {
                    result.dispatchCompletion().complete(null);
                } else {
                    result.dispatchCompletion().completeExceptionally(completionFailure);
                }
            }
            publishSchedule(result.nextSchedule());
        }

        Optional<ScheduledFuture<?>> freezeCore() {
            ScheduledFuture<?> current = future;
            future = null;
            scheduled = null;
            return Optional.ofNullable(current);
        }

        void resume() {
            ResumePlan plan =
                    inStateLane(
                            () -> {
                                if (disposed) {
                                    return null;
                                }
                                if (pendingTick != null) {
                                    return new ResumePlan(pendingTick, 0L);
                                }
                                Instant scheduledAt = nextScheduledAt;
                                long delay =
                                        scheduledAt == null
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
            return disposed;
        }

        @Override
        public CompletionStage<Void> cancel() {
            return getOrStartFinalizationAsync();
        }

        @Override
        public void close() {
            getOrStartFinalizationAsync()
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) reportCompletionFailure(unwrap(failure));
                            });
        }

        private CompletableFuture<Void> getOrStartFinalizationAsync() {
            try {
                stateLane
                        .runAsync(this::finalizationPlanOnLane)
                        .whenComplete(
                                (plan, failure) -> {
                                    if (failure != null) {
                                        finalization.completeExceptionally(unwrap(failure));
                                    } else if (plan.start()) {
                                        completeFinalization(plan);
                                    }
                                });
            } catch (RuntimeException | Error failure) {
                finalization.completeExceptionally(failure);
            }
            return finalization;
        }

        private FinalizationPlan finalizationPlanOnLane() {
            if (disposed) {
                return new FinalizationPlan(finalization, null, null, false);
            }
            timers.remove(name, this);
            Optional<ScheduledFuture<?>> task = disposeCore();
            return new FinalizationPlan(
                    finalization,
                    task.orElse(null),
                    activeDispatch == null ? null : activeDispatch.completion(),
                    true);
        }

        private void completeFinalization(FinalizationPlan plan) {
            Throwable cleanupFailure = null;
            try {
                ZLinkSpotTimerRegistry.cancel(plan.task());
            } catch (RuntimeException | Error failure) {
                cleanupFailure = failure;
            }

            Throwable capturedCleanupFailure = cleanupFailure;
            CompletionStage<Void> drain =
                    plan.activeDispatch() == null
                            ? CompletableFuture.completedFuture(null)
                            : plan.activeDispatch();
            drain.whenComplete(
                    (ignored, dispatchFailure) -> {
                        Throwable failure = capturedCleanupFailure;
                        if (dispatchFailure != null) {
                            Throwable unwrapped = unwrap(dispatchFailure);
                            if (failure == null) {
                                failure = unwrapped;
                            } else if (failure != unwrapped) {
                                failure.addSuppressed(unwrapped);
                            }
                        }
                        if (failure == null) {
                            plan.finalization().complete(null);
                        } else {
                            plan.finalization().completeExceptionally(failure);
                        }
                    });
        }
    }

    private static void cancel(ScheduledFuture<?> task) {
        if (task != null) {
            task.cancel(false);
        }
    }

    private static final class ScheduleAttempt {}

    private record AddedTimer(ManagedTimer timer, ManagedTimer previous, SchedulePlan schedule) {}

    private record FreezeState(FrozenTimers snapshot, List<ScheduledFuture<?>> futures) {}

    private record SchedulePlan(ScheduleAttempt attempt, long delayNanos) {}

    private record ResumePlan(ZLinkSpotTimerSchedule.PendingTick pendingTick, long delayNanos) {}

    private record HandlerInvocation(
            Object spot, Class<?> handlerType, CompletableFuture<Void> completion) {}

    private record ActiveDispatch(
            ZLinkSpotTimerSchedule.PendingTick tick, CompletableFuture<Void> completion) {}

    private record DispatchResult(
            boolean stillCurrent,
            FinalizationPlan stopFinalization,
            SchedulePlan nextSchedule,
            CompletableFuture<Void> dispatchCompletion) {}

    private record FinalizationPlan(
            CompletableFuture<Void> finalization,
            ScheduledFuture<?> task,
            CompletableFuture<Void> activeDispatch,
            boolean start) {}

    record FrozenTimers(List<TimerSnapshot> timers) {
        FrozenTimers {
            timers = List.copyOf(timers);
            long distinctNames = timers.stream().map(TimerSnapshot::name).distinct().count();
            if (distinctNames != timers.size()) {
                throw new ZLinkConfigurationException("duplicate timer in relocation envelope");
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
                throw new ZLinkConfigurationException("invalid timer relocation state");
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
            return deadline.isAfter(Instant.now()) ? Long.MAX_VALUE : Long.MIN_VALUE;
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
        CompletionStage<Void> enqueue(String timerName, Supplier<CompletionStage<Void>> operation);
    }
}
