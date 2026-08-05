package systems.zlink.samples.shoppingmall.server.orderworkflow.spots;

import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.ContinueOrderWorkflowSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.PrepareInventoryReservedCheckpointSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.RebuildOrderProjectionSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.RunOrderWorkflowCommandHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.StartOrderWorkflowSpotHandler;

public final class OrderWorkflowSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;

    public OrderWorkflowSpot(ZLinkInstanceSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

}
