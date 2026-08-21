package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotCreateReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotJoinRes;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

/**
 * Fixed target User Spot for the cross-language JoinSpot scenario (spec
 * 15-spot-actor.ko.md section 4.2): a foreign Actor arrives through the
 * canonical actorJoin admission leg, onActorJoin admits it, and onJoinedActor
 * records the completed membership.
 */
public final class RelocationUserSpot implements ZLinkSpot<RelocationActor> {
    public static final String SPOT_TYPE = "cross-lang-relocation-user-spot-type";

    private final ZLinkSpotContext context;
    private final EventSink sink;
    private final UserSpotJoinObserver observer;

    public RelocationUserSpot(
        ZLinkSpotContext context, EventSink sink, UserSpotJoinObserver observer) {
        this.context = context;
        this.sink = sink;
        this.observer = observer;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(UserSpotProbeHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        if (!request.isEmpty()) {
            request.decode(UserSpotCreateReq.class);
        }
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId, ZLinkMessage request) {
        String marker;
        try {
            marker = request.isEmpty() ? "" : request.decode(UserSpotJoinReq.class).marker();
        } catch (RuntimeException error) {
            // The admission payload is the foreign peer's application packet;
            // record a decode failure as evidence rather than swallowing it,
            // but still admit so the lifecycle assertion can distinguish
            // "payload shape" from "admission refused".
            sink.append("user-spot-admission-decode-failed|actor=" + actorId
                + "|error=" + error);
            marker = "undecoded";
        }
        String nodeRid = context.nodeRid().toString();
        sink.append("user-spot-admission|accepted=true|actor=" + actorId
            + "|spot=" + context.spotId());
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept(
            new UserSpotJoinRes(true, actorId, context.spotId(), nodeRid, marker)));
    }

    @Override
    public CompletionStage<Void> onJoinedActor(RelocationActor actor) {
        sink.append("user-spot-joined|actor=" + actor.actorId()
            + "|spot=" + context.spotId()
            + "|nodeRid=" + context.nodeRid());
        observer.complete();
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(RelocationActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
