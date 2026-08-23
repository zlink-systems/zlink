package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.zoneworld.server.configuration.NodeRegistry;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
public final class ReportNodeStatusHandler
    implements ZLinkRouteSendHandler<Messages.ReportNodeStatusMsg> {
    private final NodeRegistry registry;

    public ReportNodeStatusHandler(NodeRegistry registry) {
        this.registry = registry;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.ReportNodeStatusMsg message,
        ZLinkRouteMessageContext context) {
        registry.report(message, context.sourceNodeRid().toString());
        return CompletableFuture.completedFuture(null);
    }
}
