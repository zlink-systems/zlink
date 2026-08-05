package systems.zlink.stream.connector;

import java.util.concurrent.CompletionException;

final class ConnectorTestAwait {
    private ConnectorTestAwait() { }

    static void await(ZLinkStreamLifecycleCall call) throws Exception {
        try {
            call.submit().toCompletableFuture().join();
        } catch (CompletionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof Exception exception) {
                throw exception;
            }
            throw error;
        }
    }
}
