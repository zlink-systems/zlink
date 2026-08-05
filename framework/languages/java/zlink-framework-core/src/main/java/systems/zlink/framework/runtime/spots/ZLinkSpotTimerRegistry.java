package systems.zlink.framework.runtime.spots;

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
    private final Map<String, ManagedTimer> timers = new LinkedHashMap<>();
    private Object spot;
    private boolean frozen;

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
        this.spot = spot;
    }

    synchronized CompletionStage<ZLinkTimer> add(
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
                true)
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
        if (previous != null) {
            previous.close();
        }
        timer.start();
        return CompletableFuture.completedFuture(timer);
    }

    synchronized FrozenTimers freeze() {
        if (!frozen) {
            frozen = true;
            timers.values().forEach(ManagedTimer::freeze);
        }
        return new FrozenTimers(timers.values().stream()
            .filter(timer -> !timer.isDisposed())
            .map(ManagedTimer::snapshot)
            .sorted(Comparator.comparing(TimerSnapshot::name))
            .toList());
    }

    synchronized void resume() {
        if (!frozen) {
            return;
        }
        frozen = false;
        timers.values().forEach(ManagedTimer::resume);
    }

    synchronized void restore(FrozenTimers state) {
        stageRestore(state);
        publishStagedRestore();
    }

    synchronized boolean hasActiveTimers() {
        return timers.values().stream().anyMatch(timer -> !timer.isDisposed());
    }

    synchronized void stageRestore(FrozenTimers state) {
        close();
        frozen = true;
        for (TimerSnapshot snapshot : state.timers()) {
            ManagedTimer timer = new ManagedTimer(snapshot);
            if (timers.put(snapshot.name(), timer) != null) {
                throw new ZLinkConfigurationException(
                    "duplicate timer in relocation envelope: " + snapshot.name());
            }
        }
    }

    synchronized void publishStagedRestore() {
        if (!frozen) {
            throw new IllegalStateException(
                "timer relocation staging is not active");
        }
        frozen = false;
        timers.values().forEach(ManagedTimer::resume);
    }

    @Override
    public synchronized void close() {
        timers.values().forEach(ManagedTimer::close);
        timers.clear();
        frozen = false;
    }

    private synchronized void removeTimer(String name, ManagedTimer timer) {
        timers.remove(name, timer);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<Void> invokeHandler(Class<?> handlerType, ZLinkTimerTick tick) {
        try {
            Object handler = handlers.instance(handlerType);
            return ZLinkHandlerMethodInvoker
                .invokeHandler(
                    handler,
                    "handle",
                    new Object[] {spot, tick},
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
        java.util.logging.Logger.getLogger(
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
        private volatile boolean disposed;
        private ScheduledFuture<?> future;
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
            scheduleNext(schedule.initialDelayNanos());
        }

        private void scheduleNext(long delayNanos) {
            if (disposed || frozen) {
                return;
            }
            long boundedDelay = Math.max(0L, delayNanos);
            nextScheduledAt = safePlusNanos(Instant.now(), boundedDelay);
            future = executor.schedule(
                this::run,
                boundedDelay,
                TimeUnit.NANOSECONDS);
        }

        private void run() {
            ZLinkSpotTimerSchedule.PendingTick selected;
            synchronized (ZLinkSpotTimerRegistry.this) {
                if (disposed || frozen) {
                    return;
                }
                selected = schedule.nextTick(
                    schedule.startedElapsedNanos(),
                    Instant.now());
                pendingTick = selected;
                nextScheduledAt = null;
            }
            dispatchPending(selected);
        }

        private void dispatchPending(
            ZLinkSpotTimerSchedule.PendingTick selected) {
            ZLinkTimerTick tick = selected.tick();
            dispatch.enqueue(name, () -> {
                synchronized (ZLinkSpotTimerRegistry.this) {
                    if (disposed || frozen || pendingTick != selected) {
                        return CompletableFuture.completedFuture(null);
                    }
                }
                return invokeHandler(handlerType, tick);
            })
                .whenComplete((ignored, error) -> {
                    boolean stopped = false;
                    synchronized (ZLinkSpotTimerRegistry.this) {
                        if (frozen || disposed || pendingTick != selected) {
                            return;
                        }
                        if (error == null) {
                            schedule.markDelivered(selected);
                        } else {
                            stopped = options.stopOnUnhandledException();
                            if (stopped) {
                                close();
                            }
                            publishFailure(this, tick, error, stopped);
                        }
                        pendingTick = null;
                        if (!stopped) {
                            scheduleAfterDispatch();
                        }
                    }
                });
        }

        private void scheduleAfterDispatch() {
            if (!disposed) {
                scheduleNext(schedule.delayAfterDispatchNanos());
            }
        }

        void freeze() {
            if (future != null) {
                future.cancel(false);
                future = null;
            }
        }

        void resume() {
            if (disposed) {
                return;
            }
            if (pendingTick != null) {
                dispatchPending(pendingTick);
                return;
            }
            Instant scheduledAt = nextScheduledAt;
            long delay = scheduledAt == null
                ? schedule.delayAfterDispatchNanos()
                : nanosUntil(scheduledAt);
            scheduleNext(delay);
        }

        TimerSnapshot snapshot() {
            return new TimerSnapshot(
                name,
                handlerType,
                schedule.snapshot(),
                Optional.ofNullable(nextScheduledAt),
                Optional.ofNullable(pendingTick));
        }

        @Override
        public boolean isDisposed() {
            return disposed;
        }

        @Override
        public CompletionStage<Void> cancel() {
            close();
            removeTimer(name, this);
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public void close() {
            disposed = true;
            if (future != null) {
                future.cancel(false);
                future = null;
            }
        }
    }

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
