package systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.OrderWorkflowService;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.OrderWorkflowSpot;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class RunOrderWorkflowCommandHandler
    implements ZLinkSpotPacketHandler<OrderWorkflowSpot, Messages.RunOrderWorkflowCommand> {
    private final OrderWorkflowService workflow;

    public RunOrderWorkflowCommandHandler(OrderWorkflowService workflow) {
        this.workflow = workflow;
    }

    @Override
    public CompletionStage<Void> handle(
        OrderWorkflowSpot spot,
        Messages.RunOrderWorkflowCommand message) {
        workflow.continueOrderInSpot(message.orderId());
        return CompletableFuture.completedFuture(null);
    }
}
