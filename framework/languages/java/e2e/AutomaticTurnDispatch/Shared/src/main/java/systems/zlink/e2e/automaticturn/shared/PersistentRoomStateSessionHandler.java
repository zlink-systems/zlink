package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class PersistentRoomStateSessionHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext,
        Contracts.PersistentRoomStateReq> {
    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public PersistentRoomStateSessionHandler(
        ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public Class<Contracts.PersistentRoomStateReq> messageType() {
        return Contracts.PersistentRoomStateReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.PersistentRoomStateReq request) {
        RoutingId spotRid = RoutingId.from(dispatch.metadata().get(Contracts.SPOT_RID_METADATA));
        return spots.resolveSpotHandle(spotRid)
            .thenCompose(handle -> routes.requestToSpot(
                    handle.orElseThrow(() -> new IllegalStateException("spot not found: " + spotRid)),
                    request)
                .timeout(Duration.ofSeconds(30))
                .submit(Contracts.PersistentRoomStateRes.class))
            .thenAccept(reply -> context.client().reply(reply).submit());
    }
}
