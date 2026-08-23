package systems.zlink.crosslanguage.host;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.BeginUserSpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinRes;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

/**
 * Source side of the User-Spot JoinSpot scenario: the locally created Entry
 * Spot Actor defers a joinSpot() to the foreign target User Spot. The runtime
 * picks the canonical actorJoin (command 28) transport on its own when the
 * observed Spot route and the admitted peer's service-wire capability allow
 * it (spec 51 section 9); this host never selects a transport explicitly.
 */
public final class BeginUserSpotJoinHandler implements ZLinkEntrySpotActorRequestHandler<
    RelocationEntrySpot, RelocationActor, BeginUserSpotJoinReq, UserSpotJoinRes> {
    private final EventSink sink;

    public BeginUserSpotJoinHandler(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<UserSpotJoinRes> handle(
        RelocationEntrySpot spot,
        RelocationActor actor,
        ZLinkMessageContext context,
        BeginUserSpotJoinReq request) {
        String nodeRid = spot.context().nodeRid().toString();
        sink.append("user-spot-join-deferred|actor=" + actor.actorId()
            + "|spot=" + request.targetSpotId());
        actor.context()
            .joinSpot(request.targetSpotId(), new UserSpotJoinReq(request.marker()))
            .timeout(Duration.ofSeconds(30))
            .defer();
        return CompletableFuture.completedFuture(new UserSpotJoinRes(
            true, actor.actorId(), request.targetSpotId(), nodeRid, request.marker()));
    }
}
