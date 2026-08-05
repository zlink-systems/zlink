package systems.zlink.e2e.registrymessaging.workflow.Handlers;

import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ProfileMsgHandler
    implements ZLinkSendHandler<Contracts.ProfileMsg> {
    private final ScenarioState state;

    public ProfileMsgHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.ProfileMsg message,
        ZLinkMessageContext context) {
        state.record("ProfileMsg", message.commandId());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
