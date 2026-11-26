package MyGame.table;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Matchmaking {
    private final PlayerApi players;

    public Matchmaking(PlayerApi players) {
        this.players = players;
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> find(
            Entry spot, Player actor, ZLinkMessageContext context, FindMatch message) {
        return actor.loadPlayer(players).thenCompose(ignored -> actor.find());
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> find(
            Table spot, Player actor, ZLinkMessageContext context, FindMatch message) {
        return actor.find();
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> cancel(
            Entry spot, Player actor, ZLinkMessageContext context, CancelMatch message) {
        return actor.cancel(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> cancel(
            Table spot, Player actor, ZLinkMessageContext context, CancelMatch message) {
        return actor.cancel(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> assign(
            Entry spot, Player actor, ZLinkMessageContext context, MatchAssigned message) {
        actor.assigned(message);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> assign(
            Table spot, Player actor, ZLinkMessageContext context, MatchAssigned message) {
        actor.assigned(message);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> cancelled(
            Entry spot, Player actor, ZLinkMessageContext context, MatchCancelled message) {
        return actor.cancelled(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> cancelled(
            Table spot, Player actor, ZLinkMessageContext context, MatchCancelled message) {
        return actor.cancelled(message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> withdraw(
            Entry spot, Player actor, ZLinkMessageContext context, WithdrawRoom message) {
        actor.withdrawn(message);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> withdraw(
            Table spot, Player actor, ZLinkMessageContext context, WithdrawRoom message) {
        actor.withdrawn(message);
        return CompletableFuture.completedFuture(null);
    }
}
