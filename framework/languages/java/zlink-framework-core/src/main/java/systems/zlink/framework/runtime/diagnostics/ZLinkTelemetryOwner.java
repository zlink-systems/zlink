package systems.zlink.framework.runtime.diagnostics;

import org.slf4j.Logger;
import org.slf4j.event.Level;
import org.slf4j.helpers.NOPLogger;
import org.slf4j.helpers.SubstituteLogger;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Instant;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.logging.LogRecord;

/** Delivers Framework diagnostics on a single process worker. */
public final class ZLinkTelemetryOwner {
    static final int PENDING_LIMIT = 8192;
    private static final ZLinkTelemetryOwner PROCESS = new ZLinkTelemetryOwner();
    private final ConcurrentLinkedQueue<Delivery> pending = new ConcurrentLinkedQueue<>();
    private final AtomicInteger pendingCount = new AtomicInteger();
    private final Overflow flowOverflow = new Overflow(true);
    private final Overflow diagnosticOverflow = new Overflow(false);
    private final AtomicLong wakes = new AtomicLong();
    private final AtomicBoolean signalled = new AtomicBoolean();
    private final AtomicBoolean started = new AtomicBoolean();
    private final Thread worker = new Thread(null, this::run, "zlink-process-telemetry", 0, false);
    private volatile long loggerFailures;
    private volatile long traceFailures;
    private volatile long fallbackFailures;
    private volatile boolean executing;

    private ZLinkTelemetryOwner() {
        worker.setDaemon(true);
    }

    /** Provider discovery belongs to the delivery boundary and does not start its worker. */
    static boolean hasProvider(Logger logger) {
        while (logger instanceof SubstituteLogger substitute) {
            if (substitute.isDelegateNull()) return false;
            logger = substitute.delegate();
        }
        return !(logger instanceof NOPLogger);
    }

    /** Starts the process delivery resource when provisioning a diagnostic producer. */
    public static void prepare() {
        if (!PROCESS.started.get() && PROCESS.started.compareAndSet(false, true)) {
            PROCESS.worker.start();
        }
    }

    public static Host hostOf(ZLinkMessageFlowTracer flow) {
        return flow.telemetryHost();
    }

    public static Host openHost() {
        return new Host();
    }

    /** Admission state only; never retains the host runtime, registration or activator. */
    public static final class Host implements AutoCloseable {
        private volatile boolean retired;

        private Host() {}

        @Override
        public void close() {
            if (retired) return;
            retired = true;
            PROCESS.retire(this);
        }
    }

    private static final class Delivery {
        private static final AtomicReferenceFieldUpdater<Delivery, Diagnostic> RECORD =
                AtomicReferenceFieldUpdater.newUpdater(Delivery.class, Diagnostic.class, "record");
        private final Host host;
        private volatile Diagnostic record;

        private Delivery(Host host, Diagnostic record) {
            this.host = host;
            this.record = record;
        }
    }

    private sealed interface Diagnostic permits FlowRecord, JulRecord {
        void publish();
    }

    private record FlowRecord(
            ZLinkMessageFlowEvent event,
            Long size,
            Level level,
            Instant observed,
            Logger logger,
            AtomicLong failures)
            implements Diagnostic {
        @Override
        public void publish() {
            logger.atLevel(level)
                    .addKeyValue(ZLinkTraceFormat.TIMESTAMP_FIELD, observed)
                    .log(ZLinkTraceFormat.flowLine(event, size));
        }
    }

    private record JulRecord(
            java.util.logging.Logger logger,
            String name,
            java.util.logging.Level level,
            String message,
            Throwable failure,
            Object[] parameters,
            Instant observed,
            long threadId)
            implements Diagnostic {
        @Override
        public void publish() {
            var record = new LogRecord(level, message);
            record.setLoggerName(name);
            record.setSourceClassName(name);
            record.setInstant(observed);
            record.setLongThreadID(threadId);
            record.setThrown(failure);
            record.setParameters(parameters);
            logger.log(record);
        }
    }

    static void submit(
            Host host,
            ZLinkMessageFlowEvent event,
            Long size,
            Level level,
            Instant observed,
            Logger logger,
            AtomicLong failures) {
        if (!host.retired) {
            PROCESS.admit(host, new FlowRecord(event, size, level, observed, logger, failures));
        }
    }

    public static void log(
            Host host,
            java.util.logging.Logger logger,
            java.util.logging.Level level,
            Supplier<String> message,
            Throwable failure) {
        log(host, logger, level, message, failure, null);
    }

    public static void logParameters(
            Host host,
            java.util.logging.Logger logger,
            java.util.logging.Level level,
            String message,
            Object... parameters) {
        log(host, logger, level, () -> message, null, parameters);
    }

    private static void log(
            Host host,
            java.util.logging.Logger logger,
            java.util.logging.Level level,
            Supplier<String> message,
            Throwable failure,
            Object[] parameters) {
        if (logger.isLoggable(level) && !retired(host)) {
            PROCESS.admit(
                    host,
                    new JulRecord(
                            logger,
                            logger.getName(),
                            level,
                            message.get(),
                            failure,
                            parameters,
                            Instant.now(),
                            Thread.currentThread().threadId()));
        }
    }

    private static boolean retired(Host host) {
        return host != null && host.retired;
    }

    private void admit(Host host, Diagnostic record) {
        int count = pendingCount.get();
        while (true) {
            if (retired(host)) return;
            if (count == PENDING_LIMIT) {
                var overflow = record instanceof FlowRecord ? flowOverflow : diagnosticOverflow;
                overflow.total.incrementAndGet();
                signal();
                return;
            }
            if (pendingCount.compareAndSet(count, count + 1)) break;
            count = pendingCount.get();
        }
        var delivery = new Delivery(host, record);
        pending.add(delivery);
        // Retirement may have scanned the queue before this producer published its reservation.
        // Unlink that late entry before returning its slot, even if the worker is in a provider.
        if (retired(host) && pending.remove(delivery)) {
            claim(delivery);
            return;
        }
        signal();
    }

    private void signal() {
        if (!signalled.get() && signalled.compareAndSet(false, true)) {
            wakes.incrementAndGet();
            if (started.get()) LockSupport.unpark(worker);
            else prepare();
        }
    }

    private void retire(Host host) {
        pending.removeIf(
                delivery -> {
                    if (delivery.host != host) return false;
                    claim(delivery);
                    return true;
                });
    }

    private Diagnostic claim(Delivery delivery) {
        Diagnostic record = Delivery.RECORD.getAndSet(delivery, null);
        if (record != null) pendingCount.decrementAndGet();
        return record;
    }

    private void run() {
        while (true) {
            if (!hasPendingWork()) {
                executing = false;
                signalled.set(false);
                // Recheck publication after clearing the signal to avoid missing a wake.
                if (!hasPendingWork()) {
                    LockSupport.park(this);
                    Thread.interrupted();
                    continue;
                }
                signalled.set(true);
            }
            executing = true;
            flowOverflow.report();
            diagnosticOverflow.report();
            Delivery delivery = pending.poll();
            if (delivery != null) {
                Diagnostic record = claim(delivery);
                if (record != null && !retired(delivery.host)) deliver(record);
            }
        }
    }

    private boolean hasPendingWork() {
        return !pending.isEmpty()
                || flowOverflow.hasUnreported()
                || diagnosticOverflow.hasUnreported();
    }

    private final class Overflow {
        private final boolean messageFlow;
        private final AtomicLong total = new AtomicLong();
        private long reported;

        private Overflow(boolean messageFlow) {
            this.messageFlow = messageFlow;
        }

        private boolean hasUnreported() {
            return total.get() != reported;
        }

        private void report() {
            long current = total.get();
            long dropped = current - reported;
            reported = current;
            if (dropped == 0) return;
            try {
                ZLinkRuntimeMetrics.emitTelemetryOverflow(dropped, messageFlow);
            } catch (Throwable failure) {
                providerFailed(ProviderRole.TRACE, failure);
            }
        }
    }

    private void deliver(Diagnostic record) {
        try {
            record.publish();
        } catch (Throwable failure) {
            if (record instanceof FlowRecord flow) flow.failures().incrementAndGet();
            providerFailed(ProviderRole.LOGGER, failure);
        }
    }

    private enum ProviderRole {
        LOGGER,
        TRACE
    }

    private void providerFailed(ProviderRole role, Throwable failure) {
        long failures =
                switch (role) {
                    case LOGGER -> ++loggerFailures;
                    case TRACE -> ++traceFailures;
                };
        if (failures != 1 || fallbackFailures != 0) return;
        try {
            java.io.PrintStream fallback = System.err;
            fallback.println(
                    "ZLink telemetry "
                            + role.name().toLowerCase(java.util.Locale.ROOT)
                            + " provider failed: "
                            + failure.getClass().getName());
            if (fallback.checkError()) fallbackFailures++;
        } catch (Throwable fallbackFailure) {
            fallbackFailures++;
        }
    }

    record State(
            int pending,
            boolean executing,
            long overflow,
            long wakes,
            long loggerFailures,
            long traceFailures,
            long fallbackFailures) {}

    static State state() {
        return new State(
                PROCESS.pendingCount.get(),
                PROCESS.executing,
                PROCESS.flowOverflow.total.get() + PROCESS.diagnosticOverflow.total.get(),
                PROCESS.wakes.get(),
                PROCESS.loggerFailures,
                PROCESS.traceFailures,
                PROCESS.fallbackFailures);
    }
}
