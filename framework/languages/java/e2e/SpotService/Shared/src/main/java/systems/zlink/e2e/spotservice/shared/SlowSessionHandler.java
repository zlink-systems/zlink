package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class SlowSessionHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.SlowSessionReq> {
    private final ScenarioState evidence;

    public SlowSessionHandler(ScenarioState evidence) {
        this.evidence = evidence;
    }

    @Override
    public Class<Contracts.SlowSessionReq> messageType() {
        return Contracts.SlowSessionReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.SlowSessionReq request) {
        try {
            Thread.sleep(Math.max(0, request.delayMilliseconds()));
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            evidence.record("SlowSessionInterrupted", "session", request.value());
        }
        evidence.record("SlowSessionHandled", "session", request.value());
        try {
            context.client().reply(new Contracts.SlowSessionRes(request.value())).submit();
        } catch (Exception error) {
            evidence.record("SlowSessionReplyFailed", "session", request.value() + "/" + error.getClass().getSimpleName());
        }
        return CompletableFuture.completedFuture(null);
    }
}
