package systems.zlink.e2e.pubsub.subscriber.Handlers;

import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class EventMsgHandler
    implements ZLinkFanoutHandler<Contracts.EventMsg> {
    private final EvidenceStore evidence;

    public EventMsgHandler(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        Contracts.EventMsg message,
        ZLinkPublishMessageContext context) {
        if (!evidence.accepts(context.topic())) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        evidence.delayIfConfigured(message.scenario());
        evidence.record(
            "EventMsg",
            context.topic(),
            message.scenario(),
            message.sequence(),
            message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
