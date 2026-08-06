package systems.zlink.e2e.channelegress.role;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;

public final class ChannelProbeSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final EvidenceState evidence;

    public ChannelProbeSession(
        ZLinkSessionContext context,
        EvidenceState evidence) {
        this.context = context;
        this.evidence = evidence;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        evidence.add("stream-connected", context.sessionId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        evidence.add("stream-disconnected", context.sessionId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        evidence.add("stream-error", error.error().name());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        evidence.add("stream", "packet=" + dispatch.packetName());
        return CompletableFuture.completedFuture(null);
    }
}
