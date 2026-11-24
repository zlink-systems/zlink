package MyGame.table;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Join {
    private final ZLinkSpotManager spots;
    private final PlayerApi players;

    public Join(ZLinkSpotManager spots, PlayerApi players) {
        this.spots = spots;
        this.players = players;
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry entry, Player actor, ZLinkMessageContext dispatch, JoinTableRequest request) {
        if (!actor.context().actorId().equals(request.playerId()))
            throw new IllegalArgumentException("Player identity differs from Actor");
        if (actor.state.joining != null || actor.state.ticketId != null) {
            return actor.reply(new ActionRejected(Rules.TABLE_FULL, 0));
        }
        return actor.loadPlayer(players)
                .thenCompose(
                        ignored ->
                                spots.getOrCreate(request.tableId(), Names.TABLE)
                                        .inMesh(Names.MESH)
                                        .request(request)
                                        .submit())
                .thenAccept(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Table creation rejected");
                            actor.joinTable(request.tableId());
                        });
    }
}
