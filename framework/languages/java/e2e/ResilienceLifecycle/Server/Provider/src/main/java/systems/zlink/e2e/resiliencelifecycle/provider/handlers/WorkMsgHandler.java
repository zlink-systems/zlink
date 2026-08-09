package systems.zlink.e2e.resiliencelifecycle.provider.handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.e2e.resiliencelifecycle.provider.infrastructure.ScenarioState;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class WorkMsgHandler implements ZLinkSendHandler<Contracts.WorkMsg> {
    private final ScenarioState state;

    public WorkMsgHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.WorkMsg message,
        ZLinkMessageContext context) {
        state.record("WorkMsg", message.value());
        return CompletableFuture.completedFuture(null);
    }
}
