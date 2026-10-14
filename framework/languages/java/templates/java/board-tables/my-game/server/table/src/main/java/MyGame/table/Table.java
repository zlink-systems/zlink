package MyGame.table;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class Table implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient clients;
    TableState state = new TableState();

    public Table(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient clients) {
        this.context = context;
        this.config = config;
        this.clients = clients;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(JoinTableRequest.class);
        checkJoin(actorId, join);
        return CompletableFuture.completedFuture(
                index(actorId) == Rules.NO_SEAT && state.seats.size() == Rules.SEATS
                        ? ZLinkSpotActorJoinResult.reject(new ActionRejected(Rules.TABLE_FULL))
                        : ZLinkSpotActorJoinResult.accept());
    }

    void checkJoin(String actorId, JoinTableRequest request) {
        if (!request.playerId().equals(actorId) || !request.tableId().equals(context.spotId()))
            throw new IllegalArgumentException("Join identity mismatch");
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var id = actor.context().actorId();
        if (index(id) == Rules.NO_SEAT) state.seats.add(new TableState.Seat(id));
        return broadcastViews();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    private int index(String id) {
        for (int i = 0; i < state.seats.size(); i++)
            if (state.seats.get(i).playerId.equals(id)) return i;
        return Rules.NO_SEAT;
    }

    private TableView view(String id, List<SeatView> seats) {
        return new TableView(
                context.spotId(),
                config.id(),
                index(id),
                state.handNumber,
                state.inHand,
                state.turnSeat,
                state.pot,
                seats);
    }

    public CompletionStage<Void> current(Player actor) {
        int i = index(actor.context().actorId());
        var response = reply(actor, view(actor.context().actorId(), seatViews()));
        if (state.inHand && i != Rules.NO_SEAT && !state.seats.get(i).cards.isEmpty()) {
            var dealt = new HandDealt(state.handNumber, state.seats.get(i).cards);
            return response.thenCompose(ignored -> reply(actor, dealt));
        }
        return response;
    }

    public CompletionStage<Void> start(Player actor) {
        String reason =
                index(actor.context().actorId()) == Rules.NO_SEAT
                        ? Rules.NOT_SEATED
                        : state.inHand
                                ? Rules.HAND_IN_PROGRESS
                                : state.seats.size() < Rules.MIN_PLAYERS
                                        ? Rules.NOT_ENOUGH_PLAYERS
                                        : null;
        if (reason != null) return reply(actor, new ActionRejected(reason));
        var deck = new ArrayList<Integer>(Rules.DECK_SIZE);
        for (int i = 0; i < Rules.DECK_SIZE; i++) deck.add(i);
        Collections.shuffle(deck);
        for (var seat : state.seats) {
            var cards = new ArrayList<Integer>(Rules.CARDS);
            for (int i = 0; i < Rules.CARDS; i++) cards.add(deck.removeLast());
            seat.cards = List.copyOf(cards);
            seat.acted = false;
        }
        state.deck = deck;
        state.handNumber++;
        state.inHand = true;
        state.turnSeat = 0;
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var seat : state.seats) {
            var dealt = new HandDealt(state.handNumber, seat.cards);
            sends = sends.thenCompose(ignored -> send(seat.playerId, dealt));
        }
        return sends.thenCompose(ignored -> broadcastViews());
    }

    public CompletionStage<Void> bet(Player actor, BetRequest request) {
        int i = index(actor.context().actorId());
        String reason =
                i == Rules.NO_SEAT
                        ? Rules.NOT_SEATED
                        : !state.inHand || request.handNumber() != state.handNumber
                                ? Rules.NO_HAND
                                : i != state.turnSeat
                                        ? Rules.NOT_YOUR_TURN
                                        : request.amount() < 0
                                                        || request.amount()
                                                                > state.seats.get(i).chips
                                                ? Rules.INVALID_AMOUNT
                                                : null;
        if (reason != null) return reply(actor, new ActionRejected(reason));
        var seat = state.seats.get(i);
        seat.chips -= request.amount();
        state.pot += request.amount();
        seat.acted = true;
        state.turnSeat = Rules.NO_SEAT;
        for (int next = 0; next < state.seats.size(); next++)
            if (!state.seats.get(next).cards.isEmpty() && !state.seats.get(next).acted) {
                state.turnSeat = next;
                break;
            }
        return broadcastViews()
                .thenCompose(
                        ignored ->
                                state.turnSeat == Rules.NO_SEAT
                                        ? settle()
                                        : CompletableFuture.completedFuture(null));
    }

    private CompletionStage<Void> settle() {
        var hands = new ArrayList<RevealedHand>();
        var winners = new ArrayList<Integer>();
        int best = Integer.MIN_VALUE;
        for (int i = 0; i < state.seats.size(); i++) {
            var seat = state.seats.get(i);
            if (seat.cards.isEmpty()) continue;
            hands.add(new RevealedHand(i, seat.playerId, seat.cards));
            int score =
                    seat.cards.stream().mapToInt(card -> card % Rules.RANKS + Rules.MIN_RANK).sum();
            if (score > best) {
                winners.clear();
                best = score;
            }
            if (score == best) winners.add(i);
        }
        int pot = state.pot;
        for (int winner : winners) state.seats.get(winner).chips += pot / winners.size();
        state.seats.get(winners.getFirst()).chips += pot % winners.size();
        state.inHand = false;
        state.pot = 0;
        for (var seat : state.seats) seat.cards = List.of();
        var result =
                new HandResult(
                        state.handNumber,
                        pot,
                        List.copyOf(winners),
                        List.copyOf(hands),
                        seatViews());
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var seat : state.seats)
            sends = sends.thenCompose(ignored -> send(seat.playerId, result));
        return sends.thenCompose(ignored -> broadcastViews());
    }

    private List<SeatView> seatViews() {
        var views = new ArrayList<SeatView>(state.seats.size());
        for (int i = 0; i < state.seats.size(); i++) {
            var s = state.seats.get(i);
            views.add(new SeatView(i, s.playerId, s.chips, s.acted, s.cards.size()));
        }
        return List.copyOf(views);
    }

    private CompletionStage<Void> broadcastViews() {
        var seats = seatViews();
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var seat : state.seats) {
            var view = view(seat.playerId, seats);
            sends = sends.thenCompose(ignored -> send(seat.playerId, view));
        }
        return sends;
    }

    static CompletionStage<Void> reply(Player actor, Object message) {
        return actor.context().boundSession().send(message).submit();
    }

    private CompletionStage<Void> send(String id, Object message) {
        return clients.sendToActor(id, message).submit();
    }
}
