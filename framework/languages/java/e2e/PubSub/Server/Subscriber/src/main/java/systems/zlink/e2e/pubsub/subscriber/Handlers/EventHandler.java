package systems.zlink.e2e.pubsub.subscriber.Handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class EventHandler
    implements ZLinkFanoutHandler<Contracts.Event> {
    private final EvidenceStore evidence;

    public EventHandler(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        Contracts.Event message,
        ZLinkPublishMessageContext context) {
        if (!evidence.accepts(context.topic())) {
            return CompletableFuture.completedFuture(null);
        }
        evidence.delayIfConfigured(message.scenario());
        evidence.record(
            "Event",
            context.topic(),
            message.scenario(),
            message.sequence(),
            message.value());
        return CompletableFuture.completedFuture(null);
    }
}
