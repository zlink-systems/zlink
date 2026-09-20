package systems.zlink.samples.shoppingmall.server.orderworkflow.spots.handlers;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.shoppingmall.server.orderworkflow.OrderWorkflowService;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.OrderWorkflowSpot;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

import java.util.concurrent.CompletionStage;

public final class RunOrderWorkflowMsgHandler
        implements ZLinkSpotPacketHandler<OrderWorkflowSpot, Messages.RunOrderWorkflowMsg> {
    private final OrderWorkflowService workflow;

    public RunOrderWorkflowMsgHandler(OrderWorkflowService workflow) {
        this.workflow = workflow;
    }

    @Override
    public CompletionStage<Void> handle(
            OrderWorkflowSpot spot, Messages.RunOrderWorkflowMsg message) {
        Messages.OrderState state = workflow.continueOrderInSpot(spot, message.orderId());
        return spot.closeIfTerminal(state);
    }
}
