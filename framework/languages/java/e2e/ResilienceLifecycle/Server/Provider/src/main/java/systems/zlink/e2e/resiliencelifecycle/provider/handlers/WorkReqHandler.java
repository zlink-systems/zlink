package systems.zlink.e2e.resiliencelifecycle.provider.handlers;

import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.e2e.resiliencelifecycle.provider.infrastructure.ScenarioState;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class WorkReqHandler
    implements ZLinkRequestHandler<Contracts.WorkReq, Contracts.WorkRes> {
    private final ScenarioState state;

    public WorkReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.WorkRes> handle(
        Contracts.WorkReq request,
        ZLinkMessageContext context) {
        return java.util.concurrent.CompletableFuture.supplyAsync(() -> {
            if (state.grayFailure() && request.value().startsWith("b6-gray-")) {
                state.record("GrayFailureInjected", request.value());
                throw new IllegalStateException("gray failure");
            } else if ("slow".equals(request.value())) {
                state.record("SlowStarted", request.value());
                state.awaitSlowRelease();
                state.record("SlowCompleted", request.value());
            } else if ("timeout".equals(request.value())) {
                state.record("TimeoutStarted", request.value());
                sleep(1500);
                state.record("TimeoutCompleted", request.value());
            } else {
                state.record("WorkReq", request.value());
            }
            return new Contracts.WorkRes("work:" + request.value(), state.providerRid());
        });
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }
}
