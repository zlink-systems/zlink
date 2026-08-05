package systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.OrderWorkflowService;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.OrderWorkflowSpot;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class RebuildOrderProjectionSpotHandler
    implements ZLinkSpotRequestHandler<OrderWorkflowSpot, Messages.RebuildOrderProjectionReq, Messages.RebuildOrderProjectionRes> {
    private final OrderWorkflowService workflow;

    public RebuildOrderProjectionSpotHandler(OrderWorkflowService workflow) {
        this.workflow = workflow;
    }

    @Override
    public CompletionStage<Messages.RebuildOrderProjectionRes> handle(
        OrderWorkflowSpot spot,
        Messages.RebuildOrderProjectionReq request) {
        return CompletableFuture.completedFuture(
            new Messages.RebuildOrderProjectionRes(workflow.rebuildProjectionInSpot(request.orderId())));
    }
}
