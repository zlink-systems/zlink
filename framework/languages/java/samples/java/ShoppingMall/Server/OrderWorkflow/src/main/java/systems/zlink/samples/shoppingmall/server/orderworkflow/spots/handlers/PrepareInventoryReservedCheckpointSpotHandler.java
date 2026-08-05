package systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.OrderWorkflowService;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.OrderWorkflowSpot;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class PrepareInventoryReservedCheckpointSpotHandler
    implements ZLinkSpotRequestHandler<OrderWorkflowSpot, Messages.PrepareInventoryReservedCheckpointReq, Messages.ContinueOrderWorkflowRes> {
    private final OrderWorkflowService workflow;

    public PrepareInventoryReservedCheckpointSpotHandler(OrderWorkflowService workflow) {
        this.workflow = workflow;
    }

    @Override
    public CompletionStage<Messages.ContinueOrderWorkflowRes> handle(
        OrderWorkflowSpot spot,
        Messages.PrepareInventoryReservedCheckpointReq request) {
        return CompletableFuture.completedFuture(new Messages.ContinueOrderWorkflowRes(
            workflow.prepareInventoryReservedInSpot(request.request())));
    }
}
