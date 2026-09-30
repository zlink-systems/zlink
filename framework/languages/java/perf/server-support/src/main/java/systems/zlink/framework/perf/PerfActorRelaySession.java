package systems.zlink.framework.perf;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// §10.1/§10.2: the session relays every packet to the Actor bound to it; the Actor handler's return value is the reply
// of the original STREAM request (Session binding §5), so the session writes no reply.
public final class PerfActorRelaySession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final Measurement measurement;
    private final SessionActorSetup setup;
    private CompletableFuture<ZLinkSessionActor> binding;

    public PerfActorRelaySession(ZLinkSessionContext context, Measurement measurement, SessionActorSetup setup) {
        this.context = context;
        this.measurement = measurement;
        this.setup = setup;
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
        return actor(dispatch, payload).thenCompose(actor -> actor.relay(dispatch, payload)).whenComplete((ignored, error) -> {
            if (error != null) {
                measurement.recordDiagnostic(error);
            }
        });
    }

    private synchronized CompletionStage<ZLinkSessionActor> actor(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        if (dispatch.actor() != null) {
            return CompletableFuture.completedFuture(dispatch.actor());
        }
        if (binding == null) {
            binding = setup.prepare(context, payload).toCompletableFuture();
        }
        return binding;
    }
}
