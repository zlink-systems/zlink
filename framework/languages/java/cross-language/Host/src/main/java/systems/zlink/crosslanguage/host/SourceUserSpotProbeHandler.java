package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotProbeRes;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

/** Source-side answer to the target's owner probe while the Actor still lives
 * in the local Entry Spot: an explicit "still on the source" reply instead of
 * a NOT_FOUND retry storm (mirrors Node's SourceProbeHandler). */
public final class SourceUserSpotProbeHandler implements ZLinkEntrySpotActorRequestHandler<
    RelocationEntrySpot, RelocationActor, UserSpotProbeReq, UserSpotProbeRes> {
    @Override
    public CompletionStage<UserSpotProbeRes> handle(
        RelocationEntrySpot spot,
        RelocationActor actor,
        ZLinkMessageContext context,
        UserSpotProbeReq request) {
        return CompletableFuture.completedFuture(new UserSpotProbeRes(
            actor.actorId(),
            "",
            spot.context().nodeRid().toString(),
            actor.stateVersion(),
            request.marker()));
    }
}
