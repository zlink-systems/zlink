package systems.zlink.samples.shoppingmall.server.orderworkflow.spots;

import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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
        if (isTerminal(state)) {
            context.close();
        }
        return CompletableFuture.completedFuture(null);
    }

    // --8<-- [end:doc-sm-close-terminal]

    public boolean isTerminal(Messages.OrderState state) {
        return Messages.OrderStatuses.Confirmed.equals(state.status())
                || Messages.OrderStatuses.Failed.equals(state.status());
    }
}
