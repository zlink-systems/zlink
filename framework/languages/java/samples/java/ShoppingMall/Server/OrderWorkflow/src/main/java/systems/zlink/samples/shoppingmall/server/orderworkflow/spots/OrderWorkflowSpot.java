package systems.zlink.samples.shoppingmall.server.orderworkflow.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.ContinueOrderWorkflowSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.PrepareInventoryReservedCheckpointSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.RebuildOrderProjectionSpotHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.RunOrderWorkflowMsgHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers.StartOrderWorkflowSpotHandler;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

public final class OrderWorkflowSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;

    public OrderWorkflowSpot(ZLinkInstanceSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    // --8<-- [start:doc-sm-close-terminal]
    public CompletionStage<Void> closeIfTerminal(Messages.OrderState state) {
        if (Messages.OrderStatuses.Confirmed.equals(state.status())
            || Messages.OrderStatuses.Failed.equals(state.status())) {
            return context.close().thenApply(closed -> null);
        }
        return CompletableFuture.completedFuture(null);
    }
    // --8<-- [end:doc-sm-close-terminal]

}
