package systems.zlink.e2e.resiliencelifecycle.provider.handlers;

import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.e2e.resiliencelifecycle.provider.infrastructure.ScenarioState;
import systems.zlink.framework.channels.ZLinkSendContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class WorkMsgHandler implements ZLinkSendHandler<Contracts.WorkMsg> {
    private final ScenarioState state;

    public WorkMsgHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.WorkMsg message,
        ZLinkSendContext context) {
        state.record("WorkMsg", message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
