package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class Zone implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient actors;
    private final Set<String> playerIds = new TreeSet<>();

    public Zone(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
        this.context = context;
        this.config = config;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage request) {
        return CompletableFuture.completedFuture(
                id.equals(request.decode(EnterWorldRequest.class).playerId())
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    private PlayerView view(Player actor) {
        return new PlayerView(
                config.worldId(), Names.channel(context.spotId()), config.id(), actor.gold());
    }

    public CompletionStage<Void> show(Player actor) {
        return actor.context().boundSession().send(view(actor)).submit();
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        playerIds.add(actor.context().actorId());
        return actors.sendToActor(actor.context().actorId(), view(actor))
                .submit()
                .thenCompose(ignored -> publishPlayers());
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return playerIds.remove(actor.context().actorId())
                ? publishPlayers()
                : CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return onLeaveActor(actor);
    }

    private CompletionStage<Void> publishPlayers() {
        var view = new ChannelView(Names.channel(context.spotId()), List.copyOf(playerIds));
        CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
        for (var playerId : List.copyOf(playerIds)) {
            sent = sent.thenCompose(ignored -> actors.sendToActor(playerId, view).submit());
        }
        return sent;
    }

    public CompletionStage<Void> showChannelView(Player actor) {
        return actor.context()
                .boundSession()
                .send(new ChannelView(Names.channel(context.spotId()), List.copyOf(playerIds)))
                .submit();
    }

    public List<String> snapshot() {
        return List.copyOf(playerIds);
    }

    public void restore(Collection<String> ids) {
        playerIds.clear();
        playerIds.addAll(ids);
    }
}
