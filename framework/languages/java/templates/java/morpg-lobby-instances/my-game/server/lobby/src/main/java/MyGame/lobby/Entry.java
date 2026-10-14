package MyGame.lobby;

import MyGame.*;

import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Entry implements ZLinkEntrySpot<Player> {
    private final ZLinkEntrySpotContext context;
    private final ZLinkSpotManager spots;

    public Entry(ZLinkEntrySpotContext context, ZLinkSpotManager spots) {
        this.context = context;
        this.spots = spots;
    }

    public ZLinkEntrySpotContext context() {
        return context;
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> join(Player actor, String partyId, boolean create) {
        if (partyId == null || partyId.isBlank())
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rules.INVALID_PARTY))
                    .submit();
        String id = Names.PARTY_PREFIX + partyId;
        return spots.getOrCreate(id, Names.PARTY)
                .inMesh(Names.MESH)
                .submit()
                .thenAccept(
                        result -> {
                            if (result.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Party creation rejected");
                            actor.context().joinSpot(id, new PartyJoin(partyId, create)).defer();
                        });
    }
}
