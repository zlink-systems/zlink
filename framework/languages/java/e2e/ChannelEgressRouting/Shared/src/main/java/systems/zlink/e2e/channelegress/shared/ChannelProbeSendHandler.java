package systems.zlink.e2e.channelegress.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ChannelProbeSendHandler
    implements ZLinkSendHandler<Contracts.ChannelProbeMsg> {
    private final EvidenceState evidence;

    public ChannelProbeSendHandler(EvidenceState evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.ChannelProbeMsg message,
        ZLinkMessageContext context) {
        evidence.add(
            "send",
            "channel=" + context.channelName().orElse("<none>") + "|id=" + message.id());
        return CompletableFuture.completedFuture(null);
    }
}
