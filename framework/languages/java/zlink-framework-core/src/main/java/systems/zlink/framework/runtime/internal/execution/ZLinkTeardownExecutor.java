package systems.zlink.framework.runtime.internal.execution;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class ZLinkTeardownExecutor {
    private ZLinkTeardownExecutor() {}

    public static void execute(Runnable teardown) {
        Thread.ofVirtual().name("zlink-framework-teardown").start(teardown);
    }

    public static CompletionStage<Void> submit(Runnable teardown) {
        CompletableFuture<Void> completion = new CompletableFuture<>();
        execute(
                () -> {
                    try {
                        teardown.run();
                        completion.complete(null);
                    } catch (Throwable failure) {
                        completion.completeExceptionally(failure);
                    }
                });
        return completion;
    }
}
