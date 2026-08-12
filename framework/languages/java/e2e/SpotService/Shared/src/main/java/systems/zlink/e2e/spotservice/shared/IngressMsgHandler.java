package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.ZLinkMessageContext;

public final class IngressMsgHandler implements ZLinkSendHandler<Contracts.OutboundMsg> {
    private final ScenarioState state;

    public IngressMsgHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.OutboundMsg message,
        ZLinkMessageContext context) {
        state.record("IngressMsg", "channel", message.value());
        return CompletableFuture.completedFuture(null);
    }
}
