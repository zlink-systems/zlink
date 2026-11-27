package MyGame.api;

import MyGame.*;

import systems.zlink.framework.handlers.*;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.API)
public final class PlayerRequests {
    private final PlayerStore players;

    public PlayerRequests(PlayerStore players) {
        this.players = players;
    }

    @ZLinkRequest
    public CompletionStage<PlayerRecord> loadPlayer(LoadPlayer request) {
        return players.loadPlayer(request.playerId())
                .thenApply(chips -> new PlayerRecord(request.playerId(), chips));
    }

    @ZLinkRequest
    public CompletionStage<RoundCommitted> commitRound(CommitRound request) {
        return players.commitRound(request)
                .thenApply(ignored -> new RoundCommitted(request.tableId(), request.handNumber()));
    }
}
