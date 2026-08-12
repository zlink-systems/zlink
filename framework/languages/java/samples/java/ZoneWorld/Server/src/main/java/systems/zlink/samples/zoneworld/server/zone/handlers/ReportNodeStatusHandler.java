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
public final class ReportNodeStatusHandler
    implements ZLinkSendHandler<Messages.ReportNodeStatusMsg> {
    private final NodeRegistry registry;

    public ReportNodeStatusHandler(NodeRegistry registry) {
        this.registry = registry;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.ReportNodeStatusMsg message,
        ZLinkMessageContext context) {
        registry.report(message);
        return CompletableFuture.completedFuture(null);
    }
}
