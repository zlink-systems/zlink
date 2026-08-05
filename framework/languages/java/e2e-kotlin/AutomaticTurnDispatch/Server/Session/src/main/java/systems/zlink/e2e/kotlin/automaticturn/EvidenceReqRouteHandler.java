package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class EvidenceReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.EvidenceReq> {
    private final ZLinkRouteClient routes;

    public EvidenceReqRouteHandler(ZLinkRouteClient routes) {
        this.routes = routes;
    }

    @Override
    public Class<Contracts.EvidenceReq> messageType() {
        return Contracts.EvidenceReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.EvidenceReq request) {
        return routes.requestToNode(
                Contracts.SPOT_MESH,
                SpotMsgRouteHandler.targetNode(dispatch),
                request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EvidenceRes.class)
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
