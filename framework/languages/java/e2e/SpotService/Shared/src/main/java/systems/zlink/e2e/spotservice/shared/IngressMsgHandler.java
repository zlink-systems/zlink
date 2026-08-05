package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.ZLinkMessageContext;

public final class IngressMsgHandler implements ZLinkSendHandler<Contracts.OutboundMsg> {
    private final ScenarioState state;

    public IngressMsgHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.OutboundMsg message,
        ZLinkMessageContext context) {
        state.record("IngressMsg", "channel", message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
