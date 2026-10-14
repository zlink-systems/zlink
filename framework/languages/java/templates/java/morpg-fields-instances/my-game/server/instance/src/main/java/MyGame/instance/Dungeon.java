package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

// Game instances use User Spots so that Player Actors retain their membership.
public final class Dungeon implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    DungeonState state = new DungeonState();

    public Dungeon(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public DungeonPrepared prepare(PrepareDungeon request) {
        var members = request.members();
        if (!state.partyId.isEmpty()
                || request.partyId() == null
                || request.partyId().isBlank()
                || members == null
                || members.isEmpty()
                || members.size() > Rules.MAXIMUM_PARTY_MEMBERS
                || members.stream().anyMatch(id -> id == null || id.isBlank())
                || new HashSet<>(members).size() != members.size()
                || !Rules.dungeonId(request.partyId()).equals(context.spotId()))
            throw new IllegalArgumentException("Invalid dungeon party preparation");
        state.partyId = request.partyId();
        state.members = List.copyOf(members);
        return new DungeonPrepared(context.spotId());
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(JoinDungeon.class);
        return CompletableFuture.completedFuture(
                context.spotId().equals(join.dungeonId())
                                && state.partyId.equals(join.partyId())
                                && state.members.contains(actorId)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return actors.sendToActor(actor.context().actorId(), view()).submit();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private DungeonView view() {
        return new DungeonView(
                context.spotId(), config.id(), state.partyId, state.members, state.stage);
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view()).submit();
    }

    public CompletionStage<Void> advance(Player requester) {
        state.stage = Math.incrementExact(state.stage);
        var view = view();
        CompletionStage<Void> sent = requester.context().boundSession().send(view).submit();
        for (var id : state.members)
            if (!id.equals(requester.context().actorId()))
                sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }
}
