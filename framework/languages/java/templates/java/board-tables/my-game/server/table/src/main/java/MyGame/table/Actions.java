package MyGame.table;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Actions {
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Table table, Player actor, ZLinkMessageContext context, JoinTableRequest request) {
        table.checkJoin(actor.context().actorId(), request);
        return table.current(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> view(
            Table table, Player actor, ZLinkMessageContext context, TableViewRequest request) {
        return table.current(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> start(
            Table table, Player actor, ZLinkMessageContext context, StartHandRequest request) {
        return table.start(actor);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> bet(
            Table table, Player actor, ZLinkMessageContext context, BetRequest request) {
        return table.bet(actor, request);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Table table, Player actor, ZLinkMessageContext context, TableView message) {
        return Table.reply(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Table table, Player actor, ZLinkMessageContext context, HandDealt message) {
        return Table.reply(actor, message);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> forward(
            Table table, Player actor, ZLinkMessageContext context, HandResult message) {
        return Table.reply(actor, message);
    }
}
