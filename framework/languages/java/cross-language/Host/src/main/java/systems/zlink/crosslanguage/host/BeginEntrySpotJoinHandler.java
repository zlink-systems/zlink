package systems.zlink.crosslanguage.host;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.BeginEntrySpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.EntrySpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinRes;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;

/**
 * User Spot side of the remote Entry Spot join scenario: the Actor in this
 * node's User Spot defers a joinEntrySpot(). The runtime selects the target
 * Entry Spot and the request generation; this host passes neither.
 */
public final class BeginEntrySpotJoinHandler implements ZLinkSpotActorRequestHandler<
    RelocationUserSpot, RelocationActor, BeginEntrySpotJoinReq, UserSpotJoinRes> {
    @Override
    public CompletionStage<UserSpotJoinRes> handle(
        RelocationUserSpot spot,
        RelocationActor actor,
        ZLinkMessageContext context,
        BeginEntrySpotJoinReq request) {
        actor.context()
            .joinEntrySpot(new EntrySpotJoinReq(request.marker()))
            .timeout(Duration.ofSeconds(30))
            .defer();
        return CompletableFuture.completedFuture(new UserSpotJoinRes(
            true, actor.actorId(), spot.context().spotId(), spot.context().nodeRid().toString(),
            request.marker()));
    }
}
