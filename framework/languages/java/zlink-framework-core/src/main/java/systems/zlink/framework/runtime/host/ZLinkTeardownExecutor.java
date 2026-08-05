package systems.zlink.framework.runtime.host;

final class ZLinkTeardownExecutor {
    private ZLinkTeardownExecutor() {
    }

    static void execute(Runnable teardown) {
        Thread.ofVirtual()
            .name("zlink-framework-teardown")
            .start(teardown);
    }

    static java.util.concurrent.CompletionStage<Void> submit(Runnable teardown) {
        java.util.concurrent.CompletableFuture<Void> completion =
            new java.util.concurrent.CompletableFuture<>();
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
