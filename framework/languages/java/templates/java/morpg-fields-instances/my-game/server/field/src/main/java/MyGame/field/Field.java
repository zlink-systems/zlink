package MyGame.field;

import MyGame.*;
import MyGame.border.BorderRoutes;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.*;

public final class Field implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkActorClient actors;
    private final List<String> borderTopics;
    private final ZLinkSpotManager spots;
    private final NodeConfig config;
    FieldState state = new FieldState();

    public Field(
            ZLinkSpotContext context,
            ZLinkActorClient actors,
            ZLinkSpotManager spots,
            NodeConfig config) {
        this.spots = spots;
        this.config = config;
        this.context = context;
        this.actors = actors;
        borderTopics =
                Rules.adjacentFields(context.spotId()).stream()
                        .map(target -> Rules.borderTopic(context.spotId(), target))
                        .toList();
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {
        for (var source : Rules.adjacentFields(context.spotId()))
            BorderRoutes.register(context.handlers(), source, context.spotId());
    }

    public CompletionStage<Void> onInitialize() {
        return context.addTimer(
                        Names.BORDER_TIMER,
                        Duration.ofMillis(Rules.TICK_MILLIS),
                        BorderTick.class,
                        new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                .thenApply(timer -> null);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var arrival = request.decode(EnterField.class);
        if (!Rules.fieldId(arrival.x()).equals(context.spotId()))
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        if (state.arrivals.putIfAbsent(actorId, arrival) != null)
            throw new IllegalStateException("Duplicate Field arrival");
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var arrival = state.arrivals.remove(actor.context().actorId());
        if (arrival == null)
            throw new IllegalStateException("Field admission has no movement intent");
        actor.apply(arrival);
        remember(actor);
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        state.visiblePlayers.remove(actor.context().actorId());
        var parties = state.parties.entrySet().iterator();
        while (parties.hasNext()) {
            var party = parties.next();
            if (party.getValue().remove(actor.context().actorId())) {
                if (party.getValue().isEmpty()) parties.remove();
                break;
            }
        }
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        state.visiblePlayers.remove(actor.context().actorId());
        return CompletableFuture.completedFuture(null);
    }

    public void remember(Player actor) {
        state.visiblePlayers.put(actor.context().actorId(), actor.position());
    }

    public CompletionStage<Void> publishBorder() {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var entry : state.visiblePlayers.entrySet()) {
            var position = entry.getValue();
            int offset = position.x() % Rules.FIELD_WIDTH;
            if (offset >= Rules.BORDER_RADIUS && offset < Rules.FIELD_WIDTH - Rules.BORDER_RADIUS)
                continue;
            var presence =
                    new BorderPresence(
                            context.spotId(), entry.getKey(), position.x(), position.sequence());
            for (var topic : borderTopics)
                sends =
                        sends.thenCompose(
                                ignored ->
                                        context.outbound()
                                                .publish(Names.BORDER, topic, presence)
                                                .submit());
        }
        return sends;
    }

    public CompletionStage<Void> showBorder(BorderPresence presence) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var observer : state.visiblePlayers.entrySet()) {
            if (Math.abs(observer.getValue().x() - presence.x()) > Rules.BORDER_RADIUS) continue;
            var event =
                    new BorderVisible(
                            context.spotId(),
                            presence.sourceFieldId(),
                            presence.playerId(),
                            presence.x(),
                            presence.sequence());
            var id = observer.getKey();
            sends = sends.thenCompose(ignored -> actors.sendToActor(id, event).submit());
        }
        return sends;
    }

    private FieldView view() {
        return new FieldView(
                context.spotId(),
                config.id(),
                state.visiblePlayers.keySet().stream().sorted().toList(),
                state.parties.entrySet().stream()
                        .sorted(java.util.Map.Entry.comparingByKey())
                        .map(e -> new PartyView(e.getKey(), List.copyOf(e.getValue())))
                        .toList());
    }

    private CompletionStage<Void> broadcast(Player requester) {
        var view = view();
        CompletionStage<Void> sent = requester.context().boundSession().send(view).submit();
        for (var id : state.visiblePlayers.keySet())
            if (!id.equals(requester.context().actorId()))
                sent = sent.thenCompose(ignored -> actors.sendToActor(id, view).submit());
        return sent;
    }

    public CompletionStage<Void> party(Player actor, String partyId, boolean create) {
        var id = actor.context().actorId();
        var members = state.parties.get(partyId);
        if (partyId == null
                || partyId.isBlank()
                || !state.visiblePlayers.containsKey(id)
                || state.parties.values().stream().anyMatch(p -> p.contains(id))
                || (create
                        ? members != null
                        : members == null || members.size() >= Rules.MAXIMUM_PARTY_MEMBERS))
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rejection.INVALID_PARTY))
                    .submit();
        if (create) state.parties.put(partyId, new java.util.ArrayList<>(List.of(id)));
        else members.add(id);
        return broadcast(actor);
    }

    public CompletionStage<Void> enter(Player actor) {
        var party =
                state.parties.entrySet().stream()
                        .filter(e -> e.getValue().getFirst().equals(actor.context().actorId()))
                        .findFirst();
        if (party.isEmpty())
            return actor.context()
                    .boundSession()
                    .send(new ActionRejected(Rejection.NOT_LEADER))
                    .submit();
        var partyId = party.get().getKey();
        var members = List.copyOf(party.get().getValue());
        var dungeonId = Rules.dungeonId(partyId);
        return spots.getOrCreate(dungeonId, Names.DUNGEON)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Dungeon creation rejected");
                            return context.outbound()
                                    .requestToSpot(dungeonId, new PrepareDungeon(partyId, members))
                                    .submit(DungeonPrepared.class);
                        })
                .thenCompose(
                        prepared -> {
                            if (!prepared.dungeonId().equals(dungeonId))
                                throw new IllegalStateException(
                                        "Dungeon preparation identity mismatch");
                            CompletionStage<Void> sent = CompletableFuture.completedFuture(null);
                            for (var id : members)
                                sent =
                                        sent.thenCompose(
                                                ignored ->
                                                        actors.sendToActor(
                                                                        id,
                                                                        new JoinDungeon(
                                                                                dungeonId, partyId))
                                                                .submit());
                            return sent;
                        });
    }
}
