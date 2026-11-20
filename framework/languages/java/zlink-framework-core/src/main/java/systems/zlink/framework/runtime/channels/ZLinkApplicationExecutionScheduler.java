package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.execution.ZLinkExecutionSubmission;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.Supplier;

/** Process-shared application drains for gate-free owners using the same execution resource. */
final class ZLinkApplicationExecutionScheduler {
    // Weak associations do not retain an executor after its hosts and accepted records are gone.
    private static final Map<Executor, WeakReference<ZLinkApplicationExecutionScheduler>> SHARED =
            new WeakHashMap<>();

    static ZLinkApplicationExecutionScheduler shared(Executor executor, int workers) {
        synchronized (SHARED) {
            var reference = SHARED.get(executor);
            var scheduler = reference == null ? null : reference.get();
            if (scheduler == null) {
                scheduler = new ZLinkApplicationExecutionScheduler(executor, workers);
                SHARED.put(executor, new WeakReference<>(scheduler));
            }
            return scheduler;
        }
    }

    private final Executor executor;
    private final ArrayDeque<Owner> ready = new ArrayDeque<>();
    private final ArrayDeque<Slot> idle = new ArrayDeque<>();

    ZLinkApplicationExecutionScheduler(Executor executor, int workers) {
        if (workers < 1) throw new IllegalArgumentException("workers must be positive");
        this.executor = java.util.Objects.requireNonNull(executor);
        for (int i = 0; i < workers; i++) idle.addLast(new Slot());
    }

    Owner register() {
        return new Owner();
    }

    final class Owner {
        private final ArrayDeque<Record> pending = new ArrayDeque<>();
        private boolean closingAdmissionSealed;

        CompletionStage<Void> enqueue(
                Supplier<CompletionStage<Void>> operation, CompletableFuture<Void> admission) {
            var record = offer(operation, admission);
            if (record != null) return record.result;
            var failure = new RejectedExecutionException("application owner admission is sealed");
            if (admission != null) admission.completeExceptionally(failure);
            return CompletableFuture.failedFuture(failure);
        }

        CompletionStage<Void> tryEnqueue(Supplier<CompletionStage<Void>> operation) {
            var record = offer(operation, null);
            return record == null ? null : record.result;
        }

        private Record offer(
                Supplier<CompletionStage<Void>> operation, CompletableFuture<Void> admission) {
            Record record;
            List<Slot> wake;
            synchronized (ZLinkApplicationExecutionScheduler.this) {
                if (closingAdmissionSealed) return null;
                record = new Record(operation);
                if (pending.isEmpty()) ready.addLast(this);
                pending.addLast(record);
                wake = reserveLocked();
            }
            if (record.ownership != null) record.ownership.settleAdmission();
            if (admission != null) admission.complete(null);
            submit(wake);
            return record;
        }

        void sealClosingAdmission() {
            synchronized (ZLinkApplicationExecutionScheduler.this) {
                closingAdmissionSealed = true;
            }
        }
    }

    // The head moves to exactly one slot. Other workers may claim the next head immediately.
    private Record takeLocked() {
        if (ready.isEmpty()) return null;
        Owner owner = ready.removeFirst();
        Record record = owner.pending.removeFirst();
        if (!owner.pending.isEmpty()) ready.addLast(owner);
        return record;
    }

    private List<Slot> reserveLocked() {
        if (ready.isEmpty() || idle.isEmpty()) return List.of();
        var wake = new ArrayList<Slot>();
        do {
            Slot slot = idle.removeFirst();
            slot.record = takeLocked();
            wake.add(slot);
        } while (!ready.isEmpty() && !idle.isEmpty());
        return wake;
    }

    private void submit(List<Slot> wake) {
        for (Slot slot : wake) slot.submit();
    }

    private final class Slot implements Runnable {
        private Record record;
        private final java.util.function.Consumer<RuntimeException> rejection = this::reject;

        void submit() {
            ZLinkExecutionSubmission.submit(executor, this, rejection);
        }

        @Override
        public void run() {
            long started = System.nanoTime();
            do {
                record.run();
                synchronized (ZLinkApplicationExecutionScheduler.this) {
                    record = takeLocked();
                    if (record == null) {
                        idle.addLast(this);
                        return;
                    }
                }
            } while (System.nanoTime() - started
                    < ZLinkSerialExecutionQueue.DEFAULT_OWNER_TIME_BUDGET.toNanos());
            submit();
        }

        private void reject(RuntimeException failure) {
            do {
                record.finish(failure);
                synchronized (ZLinkApplicationExecutionScheduler.this) {
                    record = takeLocked();
                    if (record == null) {
                        idle.addLast(this);
                        return;
                    }
                }
            } while (true);
        }
    }

    private static final class Record implements ZLinkSuspendInvocationContext.ApplicationTurn {
        private final Supplier<CompletionStage<Void>> operation;
        private final CompletableFuture<Void> result = new CompletableFuture<>();
        private final ZLinkApplicationJobContext.QueuedOwnership ownership =
                ZLinkApplicationJobContext.transferQueuedOwnership();
        private final ZLinkFlowContext.State flow = ZLinkFlowContext.current();

        Record(Supplier<CompletionStage<Void>> operation) {
            this.operation = java.util.Objects.requireNonNull(operation);
        }

        void run() {
            try (var context = ZLinkSuspendInvocationContext.enterSerialExecutionTurn(this);
                    ZLinkFlowContext.Scope ignored =
                            flow == null ? () -> {} : ZLinkFlowContext.enter(flow);
                    var job = enterApplicationJob()) {
                if (ownership != null) ownership.requireRunnable();
                operation.get().whenComplete((value, failure) -> finish(failure));
            } catch (RuntimeException | Error failure) {
                finish(failure);
            }
        }

        private void finish(Throwable failure) {
            if (ownership != null) ownership.close();
            if (failure == null) result.complete(null);
            else result.completeExceptionally(failure);
        }

        @Override
        public ZLinkApplicationJobContext.Scope enterApplicationJob() {
            return ZLinkApplicationJobContext.enterQueued(ownership);
        }

        @Override
        public <T> T call(Supplier<T> operation) {
            try (var context = ZLinkSuspendInvocationContext.enterSerialExecutionTurn(this);
                    ZLinkFlowContext.Scope ignored =
                            flow == null ? () -> {} : ZLinkFlowContext.enter(flow);
                    var job = enterApplicationJob()) {
                return operation.get();
            }
        }

        @Override
        public <T> CompletionStage<T> manage(CompletionStage<T> stage) {
            if (stage.toCompletableFuture().isDone()) return stage;
            var managed = new CompletableFuture<T>();
            systems.zlink.framework.runtime.internal.ZLinkCompletionBridge.forwardCancellation(
                    managed, stage);
            stage.whenComplete(
                    (value, failure) ->
                            execute(
                                    () -> {
                                        if (failure == null) managed.complete(value);
                                        else managed.completeExceptionally(failure);
                                    }));
            return managed;
        }
    }
}
