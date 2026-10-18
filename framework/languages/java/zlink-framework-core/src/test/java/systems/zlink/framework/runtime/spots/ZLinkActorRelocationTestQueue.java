package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/** Test adapters use the same prepared-Actor lifecycle barrier and FIFO as production. */
final class ZLinkActorRelocationTestQueue {
    private final CompletableFuture<Void> opened = new CompletableFuture<>();
    private ZLinkSerialExecutionQueue fifo;
    private ZLinkApplicationJobQueue jobs;
    private CompletionStage<List<CompletableFuture<Void>>> admission;

    CompletionStage<List<CompletableFuture<Void>>> admit(
            List<ZLinkSerialExecutionQueue.QueuedRecord> records,
            java.util.function.BiFunction<
                            ZLinkSerialExecutionQueue.QueuedRecord,
                            CompletableFuture<Void>,
                            Supplier<CompletionStage<Void>>>
                    replay) {
        if (fifo != null)
            throw new IllegalStateException(
                    "one fixture queue belongs to one prepared Actor prefix");
        fifo =
                new ZLinkSerialExecutionQueue(
                        new InlineExecutor(), ZLinkExecutionLanePolicy.actorDelivery());
        jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        fifo.enqueueBarrierNext(() -> opened, null);
        admission =
                fifo.admitDurablePrefix(
                                records,
                                (record, terminal) -> {
                                    var operation = replay.apply(record, terminal);
                                    return () -> {
                                        ZLinkApplicationJobContext
                                                .beforeFirstApplicationInstruction();
                                        return operation.get();
                                    };
                                },
                                jobs)
                        .whenComplete(
                                (accepted, failure) -> {
                                    if (failure != null) {
                                        opened.completeExceptionally(failure);
                                        closeWhenQuiescent();
                                    } else {
                                        CompletableFuture.allOf(
                                                        accepted.toArray(CompletableFuture[]::new))
                                                .whenComplete(
                                                        (ignored, terminalFailure) ->
                                                                closeWhenQuiescent());
                                    }
                                });
        return admission;
    }

    private void closeWhenQuiescent() {
        fifo.awaitQuiescence()
                .thenRun(
                        () -> {
                            fifo.close();
                            jobs.close();
                        });
    }

    CompletionStage<Void> discard() {
        var failure =
                new java.util.concurrent.CancellationException("prepared fixture Actor discarded");
        if (fifo == null) {
            opened.completeExceptionally(failure);
            return CompletableFuture.completedFuture(null);
        }
        return admission
                .handle(
                        (accepted, admissionFailure) -> {
                            if (accepted != null) accepted.forEach(entry -> entry.cancel(false));
                            opened.completeExceptionally(failure);
                            return fifo.awaitQuiescence();
                        })
                .thenCompose(quiet -> quiet);
    }

    void open() {
        opened.complete(null);
    }

    private static final class InlineExecutor extends AbstractExecutorService {
        private boolean shutdown;

        public void execute(Runnable command) {
            command.run();
        }

        public void shutdown() {
            shutdown = true;
        }

        public List<Runnable> shutdownNow() {
            throw new UnsupportedOperationException();
        }

        public boolean isShutdown() {
            return shutdown;
        }

        public boolean isTerminated() {
            return shutdown;
        }

        public boolean awaitTermination(long ignored, TimeUnit unit) {
            return shutdown;
        }
    }
}
