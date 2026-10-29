package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Instant;
import java.util.ArrayDeque;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/** The process boundary for message-flow provider delivery, independent of host executors. */
public final class ZLinkTelemetryOwner {
    static final int PENDING_LIMIT = 8192;
    private static volatile ZLinkTelemetryOwner process;
    private final ArrayDeque<Delivery> pending = new ArrayDeque<>(PENDING_LIMIT);
    private final Thread worker;
    private long overflow;
    private long reportedOverflow;
    private long wakes;
    private volatile long loggerFailures;
    private volatile long traceFailures;
    private volatile long fallbackFailures;
    private boolean executing;

    private ZLinkTelemetryOwner() {
        worker = new Thread(null, this::run, "zlink-process-telemetry", 0, false);
        worker.setDaemon(true);
        worker.start();
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
            ZLinkTelemetryOwner owner;
            synchronized (ZLinkTelemetryOwner.class) {
                owner = process;
                if (owner == null) {
                    retired = true;
                    return;
                }
            }
            owner.retire(this);
        }
    }

    private record Delivery(
            Host host,
            ZLinkMessageFlowEvent event,
            Long size,
            Level level,
            Instant observed,
            Logger logger,
            AtomicLong failures) {}

    static void submit(
            Host host,
            ZLinkMessageFlowEvent event,
            Long size,
            Level level,
            Instant observed,
            Logger logger,
            AtomicLong failures) {
        ZLinkTelemetryOwner owner = process;
        if (owner == null) {
            synchronized (ZLinkTelemetryOwner.class) {
                if (host.retired) return;
                if (process == null) process = new ZLinkTelemetryOwner();
                owner = process;
            }
        }
        owner.admit(host, event, size, level, observed, logger, failures);
    }

    private synchronized void admit(
            Host host,
            ZLinkMessageFlowEvent event,
            Long size,
            Level level,
            Instant observed,
            Logger logger,
            AtomicLong failures) {
        if (host.retired) return;
        if (pending.size() == PENDING_LIMIT) {
            overflow++;
            return;
        }
        boolean empty = pending.isEmpty();
        pending.addLast(new Delivery(host, event, size, level, observed, logger, failures));
        if (empty && !executing) {
            wakes++;
            notifyAll();
        }
    }

    private synchronized void retire(Host host) {
        if (host.retired) return;
        host.retired = true;
        pending.removeIf(delivery -> delivery.host() == host);
    }

    private void run() {
        while (true) {
            Delivery delivery;
            long dropped;
            synchronized (this) {
                executing = false;
                while (pending.isEmpty() && overflow == reportedOverflow) {
                    try {
                        wait();
                    } catch (InterruptedException ignored) {
                        // The process resource is never interrupted by host retirement.
                    }
                }
                dropped = overflow - reportedOverflow;
                reportedOverflow = overflow;
                delivery = pending.pollFirst();
                executing = true;
            }
            if (dropped != 0) {
                try {
                    ZLinkRuntimeMetrics.emitMessageFlowOverflow(dropped);
                } catch (Throwable failure) {
                    providerFailed(ProviderRole.TRACE, failure);
                }
            }
            if (delivery != null) deliver(delivery);
            delivery = null;
        }
    }

    private void deliver(Delivery delivery) {
        try {
            LogRecord record =
                    new LogRecord(
                            delivery.level(),
                            ZLinkTraceFormat.flowLine(delivery.event(), delivery.size()));
            record.setInstant(delivery.observed());
            record.setLoggerName(delivery.logger().getName());
            delivery.logger().log(record);
        } catch (Throwable failure) {
            delivery.failures().incrementAndGet();
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
                    "ZLink message-flow "
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
        ZLinkTelemetryOwner owner;
        synchronized (ZLinkTelemetryOwner.class) {
            owner = process;
        }
        if (owner == null) return new State(0, false, 0, 0, 0, 0, 0);
        synchronized (owner) {
            return new State(
                    owner.pending.size(),
                    owner.executing,
                    owner.overflow,
                    owner.wakes,
                    owner.loggerFailures,
                    owner.traceFailures,
                    owner.fallbackFailures);
        }
    }
}
