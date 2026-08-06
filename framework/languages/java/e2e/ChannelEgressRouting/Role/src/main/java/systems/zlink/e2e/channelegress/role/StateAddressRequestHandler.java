package systems.zlink.e2e.channelegress.role;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteClient;

public final class StateAddressRequestHandler
    implements ZLinkRequestHandler<Contracts.StateAddressReq, Contracts.StateAddressRes> {
    private final ZLinkRouteClient routes;
    private final ZLinkActorClient actors;
    private final EvidenceState evidence;

    public StateAddressRequestHandler(
        ZLinkRouteClient routes,
        ZLinkActorClient actors,
        EvidenceState evidence) {
        this.routes = routes;
        this.actors = actors;
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.StateAddressRes> handle(
        Contracts.StateAddressReq request,
        ZLinkMessageContext context) {
        evidence.add("state-address-start", "id=" + request.id());
        return routes.requestToSpot(
                request.spotId(), new Contracts.ObjectProbeReq(request.id()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ObjectProbeRes.class)
            .thenCompose(spot -> actors.requestToActor(
                    request.actorId(), new Contracts.ObjectProbeReq(request.id()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ObjectProbeRes.class)
                .thenApply(actor -> {
                    evidence.add("state-address-end", "id=" + request.id());
                    return new Contracts.StateAddressRes(request.id(), List.of(
                        "spot:" + spot.objectId() + ":" + spot.role(),
                        "actor:" + actor.objectId() + ":" + actor.role()));
                }));
    }
}
