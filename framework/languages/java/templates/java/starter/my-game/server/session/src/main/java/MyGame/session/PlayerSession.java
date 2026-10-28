package MyGame.session;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.*;

import java.util.concurrent.*;

public final class PlayerSession implements ZLinkSession {
    private final ZLinkSessionContext context;

    public PlayerSession(ZLinkSessionContext context) {
        this.context = context;
    }

    public ZLinkSessionContext context() {
        return context;
    }

    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onError(ZLinkStreamError error) {
        System.err.println("Session error: " + error.error() + ": " + error.message());
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        return CompletableFuture.failedFuture(
                new IllegalStateException("No application packets are registered yet."));
    }
}
