package systems.zlink.framework.perf.servers.session;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// §11.1: STREAM-only receiver; no Object Server, Actor, Store or automatic discovery.
public final class PerfSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
    private final Measurement measurement;

    public PerfSession(ZLinkSessionContext context, ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers,
            Measurement measurement) {
        this.context = context;
        this.handlers = handlers;
        this.measurement = measurement;
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
        measurement.recordDiagnostic(new IllegalStateException("STREAM " + error.error() + ": " + error.message()));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        return handlers.tryHandle(context, dispatch, payload).thenAccept(handled -> {
            if (!handled) {
                throw new IllegalStateException("No typed perf session handler was registered for the packet.");
            }
        });
    }
}
