package systems.zlink.e2e.channelegress.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.FANOUT_HANDLER_GROUP)
public final class ChannelProbePublishHandler
    implements ZLinkFanoutHandler<Contracts.FanoutProbeEvent> {
    private final EvidenceState evidence;

    public ChannelProbePublishHandler(EvidenceState evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.FanoutProbeEvent message,
        ZLinkPublishMessageContext context) {
        evidence.add("fanout", message.id());
        return CompletableFuture.completedFuture(null);
    }
}
