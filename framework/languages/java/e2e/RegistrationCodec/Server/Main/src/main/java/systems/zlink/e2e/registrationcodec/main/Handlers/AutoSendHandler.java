package Handlers;
import systems.zlink.e2e.registrationcodec.main.Handlers;
import systems.zlink.e2e.registrationcodec.main.Infrastructure;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.AUTO_GROUP)
public final class AutoSendHandler
    implements ZLinkSendHandler<Contracts.EchoAutoMsg> {
    private final EvidenceStore state;

    public AutoSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.EchoAutoMsg message,
        ZLinkMessageContext context) {
        state.record("Send", "EchoAuto", message.value());
        return CompletableFuture.completedFuture(null);
    }
}
