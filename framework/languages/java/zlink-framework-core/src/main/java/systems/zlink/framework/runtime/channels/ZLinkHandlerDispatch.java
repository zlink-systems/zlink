package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;

final class ZLinkHandlerDispatch {
    private ZLinkHandlerDispatch() {}

    static CompletionStage<Void> enqueue(
            ZLinkSerialExecutionQueue queue,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission,
            ZLinkMeshDrainCoordinator.Claim claim,
            Consumer<Throwable> completion) {
        var result = new CompletableFuture<Void>();
        CompletionStage<Void> terminal = observe(result, claim, completion);
        try {
            queue.enqueue(() -> start(operation, result), admission)
                    .whenComplete(
                            (ignored, error) -> {
                                if (error != null) result.completeExceptionally(error);
                            });
        } catch (RuntimeException | Error failure) {
            result.completeExceptionally(failure);
            throw failure;
        }
        return terminal;
    }

    static boolean tryEnqueue(
            ZLinkSerialExecutionQueue queue,
            Supplier<CompletionStage<Void>> operation,
            ZLinkMeshDrainCoordinator.Claim claim,
            Consumer<Throwable> completion) {
        var result = new CompletableFuture<Void>();
        observe(result, claim, completion);
        return queue.tryEnqueue(() -> start(operation, result));
    }

    private static CompletionStage<Void> start(
            Supplier<CompletionStage<Void>> operation, CompletableFuture<Void> result) {
        try {
            operation
                    .get()
                    .whenComplete(
                            (ignored, error) -> {
                                if (error == null) result.complete(null);
                                else result.completeExceptionally(error);
                            });
        } catch (RuntimeException | Error failure) {
            result.completeExceptionally(failure);
        }
        return CompletableFuture.completedFuture(null);
    }

    private static CompletionStage<Void> observe(
            CompletionStage<Void> result,
            ZLinkMeshDrainCoordinator.Claim claim,
            Consumer<Throwable> completion) {
        return result.whenComplete(
                (ignored, error) -> {
                    try {
                        if (completion != null) completion.accept(error);
                    } finally {
                        if (claim != null) claim.close();
                    }
                });
    }
}
