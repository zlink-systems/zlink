package systems.zlink.framework.spring.sessionfixtures;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

public final class SubpackageDiscoveredPacketSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher;

    public SubpackageDiscoveredPacketSession(
        ZLinkSessionContext context,
        ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher) {
        this.context = context;
        this.dispatcher = dispatcher;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return dispatcher.tryHandle(context, dispatch, payload).thenApply(ignored -> null);
    }
}
