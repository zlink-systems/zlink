package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.zoneworld.server.configuration.NodeRegistry;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
public final class ReportSpotEventHandler
    implements ZLinkSendHandler<Messages.ReportSpotEventMsg> {
    private final NodeRegistry registry;

    public ReportSpotEventHandler(NodeRegistry registry) {
        this.registry = registry;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.ReportSpotEventMsg message,
        ZLinkMessageContext context) {
        registry.alert(message);
        return CompletableFuture.completedFuture(null);
    }
}
