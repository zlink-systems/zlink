package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangProbeReq;
import systems.zlink.crosslanguage.host.EntryRelocationContracts.CrossLangProbeRes;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

/** Post-relocation liveness probe: whichever node currently owns the actor
 * answers with its own routing id, confirming the owner transition. */
public final class RelocationProbeHandler implements ZLinkEntrySpotActorRequestHandler<
    RelocationEntrySpot, RelocationActor, CrossLangProbeReq, CrossLangProbeRes> {
    private final EventSink sink;

    public RelocationProbeHandler(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<CrossLangProbeRes> handle(
        RelocationEntrySpot spot,
        RelocationActor actor,
        ZLinkMessageContext context,
        CrossLangProbeReq request) {
        String nodeRid = spot.context().nodeRid().toString();
        sink.append("entry-spot-probe-served|node=" + nodeRid + "|actor=" + actor.actorId());
        return CompletableFuture.completedFuture(new CrossLangProbeRes(
            nodeRid, actor.stateVersion(), actor.applicationState().length, request.marker()));
    }
}
