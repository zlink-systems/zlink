package MyGame.instance;

import MyGame.*;
import java.util.*;
import java.util.concurrent.*;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

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
    state.prepare(context.spotId(), request);
    return new DungeonPrepared(context.spotId());
  }

  public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
    var join = payload.decode(JoinDungeon.class);
    return CompletableFuture.completedFuture(
        join.dungeonId().equals(context.spotId())
                && join.partyId().equals(state.partyId)
                && state.members.contains(id)
            ? ZLinkSpotActorJoinResult.accept()
            : ZLinkSpotActorJoinResult.reject(new ActionRejected(Rules.INVALID_PARTY)));
  }

  public CompletionStage<Void> onJoinedActor(Player actor) {
    return actors.sendToActor(actor.context().actorId(), view()).submit();
  }

  public CompletionStage<Void> onLeaveActor(Player actor) {
    return CompletableFuture.completedFuture(null);
  }

  private DungeonView view() {
    return new DungeonView(
        context.spotId(), config.id(), state.partyId, state.members, state.stage);
  }

  public CompletionStage<Void> show(Player actor) {
    return actor.context().boundSession().send(view()).submit();
  }

  public CompletionStage<Void> advance() {
    state.stage = Math.incrementExact(state.stage);
    var view = view();
    CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
    for (var id : state.members)
      sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
    return sent;
  }
}
