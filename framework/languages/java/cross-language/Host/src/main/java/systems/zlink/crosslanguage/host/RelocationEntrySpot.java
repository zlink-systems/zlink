package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangActorCreateReq;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

/** Entry spot for the cross-language relocation scenario. onCreateActor is
 * the JoinEntrySpot admission-free placement (spec 15:489): no OnActorJoin
 * roundtrip, so this is the only join shape usable cross-language today. */
public final class RelocationEntrySpot implements ZLinkEntrySpot<RelocationActor> {
    private final ZLinkEntrySpotContext context;

    public RelocationEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(RelocationProbeHandler.class);
        // User-Spot JoinSpot scenario: the source Entry Spot both starts the
        // deferred join and answers the target's owner probe until the Actor
        // has actually moved into the target User Spot.
        context.handlers().addHandler(BeginUserSpotJoinHandler.class);
        context.handlers().addHandler(SourceUserSpotProbeHandler.class);
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        RelocationActor actor, ZLinkMessage createRequest) {
        if (!createRequest.isEmpty()) {
            CrossLangActorCreateReq request = createRequest.decode(CrossLangActorCreateReq.class);
            actor.setStateVersion(request.stateVersion());
            byte[] state = new byte[Math.max(0, request.applicationStateBytes())];
            for (int index = 0; index < state.length; index++) {
                state[index] = (byte) (index % 251);
            }
            actor.setApplicationState(state);
        }
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(RelocationActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(RelocationActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
