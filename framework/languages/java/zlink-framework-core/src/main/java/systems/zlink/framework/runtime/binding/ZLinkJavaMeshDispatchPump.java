package systems.zlink.framework.runtime.binding;

import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.IntConsumer;
import java.util.function.IntUnaryOperator;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyDomain;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

/**
 * Converts native ready callbacks into serialized pull-dispatch work.
 *
 * <p>The native callback only records readable domains and schedules the pump.
 * It never drains a claim or invokes application code.
 */
final class ZLinkJavaMeshDispatchPump implements AutoCloseable {
    private static final long MAX_DISPATCH_TURN_NANOS =
        ZLinkReceiveBatchBudget.DEFAULT_MAX_ELAPSED_NANOS;
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkJavaMeshDispatchPump.class.getName());
    interface Source extends AutoCloseable {
        void setReadyHandler(IntUnaryOperator handler);

        boolean drain(
            int domains,
            Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
            IntConsumer released);

        @Override
        void close();
    }

    private final Source source;
    private final Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver;
    private final java.util.function.BooleanSupplier canReceiveApplication;
    private final ExecutorService executor;
    private final AtomicInteger pendingDomains = new AtomicInteger();
    private final AtomicBoolean scheduled = new AtomicBoolean();
    private final AtomicBoolean closed = new AtomicBoolean();

    ZLinkJavaMeshDispatchPump(
        Source source,
        Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver) {
        this(
            source,
            receiver,
            () -> true,
            Executors.newSingleThreadExecutor(Thread.ofVirtual()
                .name("zlink-mesh-dispatch-", 0)
                .factory()));
    }

    ZLinkJavaMeshDispatchPump(
        Source source,
        Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
        ExecutorService executor) {
        this(source, receiver, () -> true, executor);
    }

    ZLinkJavaMeshDispatchPump(
        Source source,
        Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
        java.util.function.BooleanSupplier canReceiveApplication,
        ExecutorService executor) {
        this.source = Objects.requireNonNull(source, "source");
        this.receiver = Objects.requireNonNull(receiver, "receiver");
        this.canReceiveApplication = Objects.requireNonNull(
            canReceiveApplication, "canReceiveApplication");
        this.executor = Objects.requireNonNull(executor, "executor");
        source.setReadyHandler(this::onReady);
    }

    private int onReady(int domains) {
        if (closed.get()) {
            return 0;
        }
        requestDrain(domains);
        return domains;
    }

    void requestDrain(int domains) {
        if (closed.get()) {
            return;
        }
        pendingDomains.getAndUpdate(current -> current | domains);
        schedule();
    }

    private void schedule() {
        if (scheduled.compareAndSet(false, true)) {
            executor.execute(this::run);
        }
    }

    private void run() {
        try {
            dispatchLoop:
            while (!closed.get()) {
                int domains = pendingDomains.getAndSet(0);
                if (domains == 0) {
                    break;
                }
                if (!canReceiveApplication.getAsBoolean()) {
                    int application = domains & ReadyDomain.APPLICATION.value();
                    if (application != 0) {
                        pendingDomains.getAndUpdate(current -> current | application);
                        domains &= ~application;
                    }
                }
                if (domains == 0) {
                    break;
                }
                long turnStartedNanos = System.nanoTime();
                boolean residue;
                do {
                    if (!canReceiveApplication.getAsBoolean()) {
                        int application = domains & ReadyDomain.APPLICATION.value();
                        if (application != 0) {
                            pendingDomains.getAndUpdate(current -> current | application);
                            domains &= ~application;
                        }
                        if (domains == 0) {
                            break;
                        }
                    }
                    residue = source.drain(domains, receiver, this::requestDrain);
                    if (residue
                        && !closed.get()
                        && System.nanoTime() - turnStartedNanos
                            >= MAX_DISPATCH_TURN_NANOS) {
                        int turnDomains = domains;
                        pendingDomains.getAndUpdate(
                            current -> current | turnDomains);
                        break dispatchLoop;
                    }
                } while (residue && !closed.get());
            }
        } catch (RuntimeException error) {
            LOGGER.log(Level.SEVERE, "MeshNode dispatch pump failed", error);
        } finally {
            scheduled.set(false);
            if (!closed.get() && pendingDomains.get() != 0) {
                schedule();
            }
        }
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        source.setReadyHandler(null);
        executor.shutdown();
        boolean interrupted = false;
        try {
            if (!executor.awaitTermination(5, TimeUnit.SECONDS)) {
                executor.shutdownNow();
                executor.awaitTermination(5, TimeUnit.SECONDS);
            }
        } catch (InterruptedException interruption) {
            interrupted = true;
            executor.shutdownNow();
        }
        source.close();
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }
}
