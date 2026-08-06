package systems.zlink.e2e.registrymessaging.shared;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class IdentityOperations {
    private static final Duration TIMEOUT = Duration.ofSeconds(15);

    private IdentityOperations() {
    }

    public static CompletionStage<Contracts.IdentityCreateRes> create(
        String role,
        Contracts.IdentityCreateReq request,
        ZLinkActorManager actors,
        ZLinkSpotManager spots) {
        CompletionStage<ZLinkActorCreateResult> actor = actors
            .getOrCreate(request.actorId(), Contracts.OBJECT_ACTOR_TYPE)
            .inMesh(request.meshName())
            .request(request.marker())
            .timeout(TIMEOUT)
            .submit();
        CompletionStage<ZLinkSpotCreateResult> spot = spots
            .getOrCreate(request.spotId(), Contracts.OBJECT_SPOT_TYPE)
            .inMesh(request.meshName())
            .request(request.marker())
            .timeout(TIMEOUT)
            .submit();
        return actor.thenCombine(spot, (actorResult, spotResult) ->
            new Partial(role, actorResult, spotResult))
            .thenCompose(partial -> actors.find(request.actorId())
                .thenCombine(spots.find(request.spotId()), (actorFound, spotFound) ->
                    new Contracts.IdentityCreateRes(
                        partial.role,
                        actorState(partial.actorResult),
                        actorRef(partial.actorResult),
                        actorFound.map(IdentityOperations::actorRef).orElse(null),
                        spotState(partial.spotResult),
                        spotRef(partial.spotResult),
                        spotFound.map(IdentityOperations::spotRef).orElse(null))));
    }

    public static CompletionStage<Contracts.IdentityPingRes> ping(
        Contracts.IdentityPingReq request,
        ZLinkActorClient actorClient,
        ZLinkRouteClient routes) {
        CompletionStage<Contracts.IdentityActorPingRes> actor = actorClient
            .requestToActor(request.actorId(), new Contracts.IdentityActorPingReq(request.marker()))
            .timeout(TIMEOUT)
            .submit(Contracts.IdentityActorPingRes.class);
        CompletionStage<Contracts.IdentitySpotPingRes> spot = routes
            .requestToSpot(request.spotId(), new Contracts.IdentitySpotPingReq(request.marker()))
            .inMesh(request.meshName())
            .timeout(TIMEOUT)
            .submit(Contracts.IdentitySpotPingRes.class);
        return actor.thenCombine(spot, Contracts.IdentityPingRes::new);
    }

    public static CompletionStage<Contracts.IdentityActorPingRes> pingActor(
        Contracts.IdentityActorDirectReq request,
        ZLinkActorClient actorClient) {
        return actorClient
            .requestToActor(request.actorId(), new Contracts.IdentityActorPingReq(request.marker()))
            .timeout(TIMEOUT)
            .submit(Contracts.IdentityActorPingRes.class);
    }

    public static CompletionStage<Contracts.IdentitySpotPingRes> pingSpot(
        Contracts.IdentitySpotDirectReq request,
        ZLinkRouteClient routes) {
        return routes
            .requestToSpot(request.spotId(), new Contracts.IdentitySpotPingReq(request.marker()))
            .inMesh(request.meshName())
            .timeout(TIMEOUT)
            .submit(Contracts.IdentitySpotPingRes.class);
    }

    private static String actorState(ZLinkActorCreateResult result) {
        return switch (result) {
            case ZLinkActorCreateResult.Created ignored -> "CREATED";
            case ZLinkActorCreateResult.Existing ignored -> "EXISTING";
            case ZLinkActorCreateResult.Rejected ignored -> "REJECTED";
        };
    }

    private static String spotState(ZLinkSpotCreateResult result) {
        return switch (result.state()) {
            case CREATED -> "CREATED";
            case EXISTING -> "EXISTING";
            case REJECTED -> "REJECTED";
        };
    }

    private static Contracts.IdentityRef actorRef(ZLinkActorCreateResult result) {
        return switch (result) {
            case ZLinkActorCreateResult.Created created -> actorRef(created.actor());
            case ZLinkActorCreateResult.Existing existing -> actorRef(existing.actor());
            case ZLinkActorCreateResult.Rejected ignored -> null;
        };
    }

    private static Contracts.IdentityRef actorRef(ActorRef ref) {
        return new Contracts.IdentityRef(
            ref.actorId(), ref.objectGeneration(), ref.meshName(), ref.nodeRid().toString());
    }

    private static Contracts.IdentityRef spotRef(ZLinkSpotCreateResult result) {
        return result.spot() == null ? null : spotRef(result.spot());
    }

    private static Contracts.IdentityRef spotRef(SpotRef ref) {
        return new Contracts.IdentityRef(
            ref.spotId(), ref.objectGeneration(), ref.meshName(), ref.nodeRid().toString());
    }

    private record Partial(
        String role,
        ZLinkActorCreateResult actorResult,
        ZLinkSpotCreateResult spotResult) {
    }
}
