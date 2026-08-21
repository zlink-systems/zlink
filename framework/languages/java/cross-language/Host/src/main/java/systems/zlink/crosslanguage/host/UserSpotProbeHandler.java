package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeRes;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;

/** Post-join liveness probe served by the target User Spot: the answering
 * node's routing id proves the Actor now lives on the target. */
public final class UserSpotProbeHandler implements ZLinkSpotActorRequestHandler<
    RelocationUserSpot, RelocationActor, UserSpotProbeReq, UserSpotProbeRes> {
    private final EventSink sink;

    public UserSpotProbeHandler(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<UserSpotProbeRes> handle(
        RelocationUserSpot spot,
        RelocationActor actor,
        ZLinkMessageContext context,
        UserSpotProbeReq request) {
        String nodeRid = spot.context().nodeRid().toString();
        sink.append("user-spot-probe-served|nodeRid=" + nodeRid + "|actor=" + actor.actorId());
        return CompletableFuture.completedFuture(new UserSpotProbeRes(
            actor.actorId(),
            spot.context().spotId(),
            nodeRid,
            actor.stateVersion(),
            request.marker()));
    }
}
