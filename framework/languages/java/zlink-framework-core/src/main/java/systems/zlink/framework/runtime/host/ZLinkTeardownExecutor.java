package systems.zlink.framework.runtime.host;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkTeardownExecutor {
    private ZLinkTeardownExecutor() {
    }

    static void execute(Runnable teardown) {
        Thread.ofVirtual()
            .name("zlink-framework-teardown")
            .start(teardown);
    }

    static CompletionStage<Void> submit(Runnable teardown) {
        CompletableFuture<Void> completion =
            new CompletableFuture<>();
        execute(() -> {
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
