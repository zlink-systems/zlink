package systems.zlink.e2e.registrationcodec.main.Handlers;

import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;

public final class ManualSendHandler
    implements ZLinkSendHandler<Contracts.EchoManualMsg> {
    private final EvidenceStore state;

    public ManualSendHandler(EvidenceStore state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.EchoManualMsg message,
        ZLinkMessageContext context) {
        state.record("Send", context.packetName(), message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
