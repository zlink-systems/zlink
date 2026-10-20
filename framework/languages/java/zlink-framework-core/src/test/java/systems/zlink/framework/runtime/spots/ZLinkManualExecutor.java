package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertTimeoutPreemptively;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

final class ZLinkManualExecutor extends java.util.concurrent.AbstractExecutorService {
    private static final Duration COMPLETION_TIMEOUT = Duration.ofSeconds(5);

    public void shutdown() {}

    public java.util.List<Runnable> shutdownNow() {
        return java.util.List.of();
    }

    public boolean isShutdown() {
        return false;
    }

    public boolean isTerminated() {
        return false;
    }

    public boolean awaitTermination(long value, java.util.concurrent.TimeUnit unit) {
        return true;
    }

    private final java.util.concurrent.ConcurrentLinkedQueue<Runnable> tasks =
            new java.util.concurrent.ConcurrentLinkedQueue<>();

    public void execute(Runnable task) {
        tasks.add(task);
    }

    systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn actorDispatch(
            String actorId, Object incarnation, java.util.concurrent.CompletionStage<Void> moving) {
        var target =
                new ZLinkSpotSerialExecutor(
                        new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                this,
                                systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot()),
                        this,
                        this,
                        systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        var activation = target.claimActorQueue(actorId).toCompletableFuture().join();
        activation.relocationLane().enqueueLifecycleBarrier(() -> moving);
        return new systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn(
                actorId, target, incarnation, activation);
    }

    void drain() {
        for (Runnable task; (task = tasks.poll()) != null; ) task.run();
    }

    void drainUntil(CompletionStage<?> completion) {
        var terminal = completion.toCompletableFuture();
        assertTimeoutPreemptively(
                COMPLETION_TIMEOUT,
                () -> {
                    // Infrastructure completions can submit work after an empty drain returns.
                    while (!terminal.isDone() && !Thread.currentThread().isInterrupted()) {
                        drain();
                        Thread.yield();
                    }
                    assertTrue(terminal.isDone(), "manual executor operation did not complete");
                    terminal.join();
                });
    }
}
