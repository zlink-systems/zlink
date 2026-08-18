package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteReply;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteRequest;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

/** Echo handler tagged "|java", mirroring the C++/.NET/Node peer hosts'
 * spot-route-server marker. */
public final class SpotRouteRequestHandler
    implements ZLinkRouteRequestHandler<TestHostSpotRouteRequest, TestHostSpotRouteReply> {
    private final EventSink sink;

    public SpotRouteRequestHandler(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<TestHostSpotRouteReply> handle(
        TestHostSpotRouteRequest request,
        ZLinkRouteMessageContext context) {
        sink.append("spot-route-server|" + request.value() + "|" + context.sourceNodeRid());
        return CompletableFuture.completedFuture(
            new TestHostSpotRouteReply(request.value() + "|java"));
    }
}
