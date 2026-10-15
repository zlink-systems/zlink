package MyGame.town;

import MyGame.*;
import java.util.*;
import java.util.concurrent.*;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

public final class Town implements ZLinkSpot<Player> {
  private final ZLinkSpotContext context;
  private final NodeConfig config;
  private final ZLinkActorClient actors;
  private final ZLinkSpotManager spots;
  TownState state = new TownState();

  public Town(
      ZLinkSpotContext context,
      NodeConfig config,
      ZLinkActorClient actors,
      ZLinkSpotManager spots) {
    this.context = context;
    this.config = config;
    this.actors = actors;
    this.spots = spots;
  }

  public ZLinkSpotContext context() {
    return context;
  }

  public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
    var join = payload.decode(JoinTownRequest.class);
    if (!id.equals(join.playerId()) || !Names.townId(join.channel()).equals(context.spotId()))
      return CompletableFuture.completedFuture(
          ZLinkSpotActorJoinResult.reject(new ActionRejected(Rules.INVALID_PARTY)));
    state.players.add(id);
    return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
  }

  public CompletionStage<Void> onJoinedActor(Player actor) {
    return broadcast();
  }

  public CompletionStage<Void> onLeaveActor(Player actor) {
    state.leave(actor.context().actorId());
    return broadcast();
  }

  // Disconnect preserves town and party membership until an explicit move.
  public CompletionStage<Void> onDisconnectActor(Player actor) {
    return CompletableFuture.completedFuture(null);
  }

  private TownView view() {
    return state.view(context.spotId(), config.id());
  }

  public CompletionStage<Void> show(Player actor) {
    return actor.context().boundSession().send(view()).submit();
  }

  private CompletionStage<Void> broadcast() {
    var view = view();
    CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
    for (var id : state.players)
      sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
    return sent;
  }

  public CompletionStage<Void> party(Player actor, String partyId, boolean create) {
    if (!state.party(actor.context().actorId(), partyId, create))
      return actor.context().boundSession().send(new ActionRejected(Rules.INVALID_PARTY)).submit();
    return broadcast();
  }

  public CompletionStage<Void> enter(Player actor) {
    var party = state.ledParty(actor.context().actorId());
    if (party == null)
      return actor.context().boundSession().send(new ActionRejected(Rules.NOT_LEADER)).submit();
    String id = Names.DUNGEON_PREFIX + party.getKey();
    var members = List.copyOf(party.getValue());
    var partyId = party.getKey();
    return spots
        .getOrCreate(id, Names.DUNGEON)
        .inMesh(Names.MESH)
        .submit()
        .thenCompose(
            result -> {
              if (result.state() == ZLinkSpotCreateState.REJECTED)
                throw new IllegalStateException("Dungeon creation rejected");
              return context
                  .outbound()
                  .requestToSpot(id, new PrepareDungeon(partyId, members))
                  .submit(DungeonPrepared.class);
            })
        .thenCompose(
            prepared -> {
              if (!id.equals(prepared.dungeonId()))
                throw new IllegalStateException("Dungeon preparation identity mismatch");
              CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
              var travel = new JoinDungeon(id, partyId);
              for (var member : members)
                sent = sent.thenCompose(ignored -> actors.sendToActor(member, travel).submit());
              return sent;
            });
  }
}
