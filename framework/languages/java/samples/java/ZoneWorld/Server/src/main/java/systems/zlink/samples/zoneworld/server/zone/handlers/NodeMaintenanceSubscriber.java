package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
public final class NodeMaintenanceSubscriber
    implements ZLinkFanoutHandler<Messages.NodeMaintenanceChangedEvent> {
    private final NodeMaintenanceState state;
    private final MaintenanceStore store;
    private final SampleTopology topology;

    public NodeMaintenanceSubscriber(
        NodeMaintenanceState state,
        MaintenanceStore store,
        SampleTopology topology) {
        this.state = state;
        this.store = store;
        this.topology = topology;
    }

    @Override
    public CompletionStage<Void> handle(
        Messages.NodeMaintenanceChangedEvent message,
        ZLinkPublishMessageContext context) {
        state.apply(message.nodeId(), message.enabled());
        store.set(message.nodeId(), message.enabled());
        if (topology.nodeId().equals(message.nodeId())) {
            System.out.println("maintenance state node=" + message.nodeId()
                + " enabled=" + message.enabled());
        }
        return CompletableFuture.completedFuture(null);
    }
}
