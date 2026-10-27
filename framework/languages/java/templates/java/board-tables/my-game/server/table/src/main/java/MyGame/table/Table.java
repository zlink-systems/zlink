package MyGame.table;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.IntUnaryOperator;

public final class Table implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient clients;
    private final IntUnaryOperator randomIndex;
    TableState state = new TableState();

    public Table(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient clients) {
        this(context, config, clients, new SecureRandom()::nextInt);
    }

    Table(
            ZLinkSpotContext context,
            NodeConfig config,
            ZLinkActorClient clients,
            IntUnaryOperator randomIndex) {
        this.context = context;
        this.config = config;
        this.clients = clients;
        this.randomIndex = Objects.requireNonNull(randomIndex);
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(JoinTableRequest.class);
        checkJoin(actorId, join);
        return CompletableFuture.completedFuture(
                admit(actorId)
                        ? ZLinkSpotActorJoinResult.accept()
                        : ZLinkSpotActorJoinResult.reject());
    }

    void checkJoin(String actorId, JoinTableRequest request) {
        if (!request.playerId().equals(actorId) || !request.tableId().equals(context.spotId()))
            throw new IllegalArgumentException("Join identity mismatch");
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return broadcastViews();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    // Seats the player at admission; a seated player is admitted again without a new seat.
    boolean admit(String id) {
        if (index(id) != Rules.NO_SEAT) return true;
        if (state.seats.size() == Rules.SEATS) return false;
        state.seats.add(new TableState.Seat(id));
        return true;
    }

    int index(String id) {
        for (int i = 0; i < state.seats.size(); i++)
            if (state.seats.get(i).playerId.equals(id)) return i;
        return Rules.NO_SEAT;
    }

    List<Integer> privateHand(String id) {
        int i = index(id);
        return i == Rules.NO_SEAT ? List.of() : state.seats.get(i).cards;
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
        String id = actor.context().actorId();
        int i = index(id);
        var response = reply(actor, view(id, seatViews()));
        if (i != Rules.NO_SEAT) {
            var dealt = new HandDealt(state.handNumber, privateHand(id));
            return response.thenCompose(ignored -> reply(actor, dealt));
        }
        return response;
    }

    public CompletionStage<Void> start(Player actor) {
        String reason = startDomain(actor.context().actorId());
        if (reason != null) return reject(actor, reason);

        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var seat : state.seats) {
            var dealt = new HandDealt(state.handNumber, privateHand(seat.playerId));
            sends = sends.thenCompose(ignored -> send(seat.playerId, dealt));
        }
        return sends.thenCompose(ignored -> broadcastViews());
    }

    String startDomain(String playerId) {
        String reason =
                index(playerId) == Rules.NO_SEAT
                        ? Rules.NOT_SEATED
                        : state.inHand
                                ? Rules.HAND_IN_PROGRESS
                                : state.seats.size() < Rules.MIN_PLAYERS
                                        ? Rules.NOT_ENOUGH_PLAYERS
                                        : null;
        if (reason != null) return reason;
        var nextHandNumber = Rules.tryAddContractIntegers(state.handNumber, 1L);
        if (nextHandNumber.isEmpty()) return Rules.INTEGER_OUT_OF_RANGE;
        var deck = new ArrayList<Integer>(Rules.DECK_SIZE);
        for (int i = 0; i < Rules.DECK_SIZE; i++) deck.add(i);
        for (int i = deck.size() - 1; i > 0; i--) {
            int selected = randomIndex.applyAsInt(i + 1);
            if (selected < 0 || selected > i)
                throw new IllegalArgumentException("Random index is outside its bound");
            Collections.swap(deck, i, selected);
        }
        for (var seat : state.seats) {
            var cards = new ArrayList<Integer>(Rules.CARDS);
            for (int i = 0; i < Rules.CARDS; i++) cards.add(deck.removeLast());
            seat.cards = List.copyOf(cards);
            seat.acted = false;
        }
        state.deck = deck;
        state.handNumber = nextHandNumber.getAsLong();
        state.inHand = true;
        state.turnSeat = 0;
        state.pot = 0L;
        return null;
    }

    public CompletionStage<Void> bet(Player actor, BetRequest request) {
        BetOutcome outcome = betDomain(actor.context().actorId(), request);
        if (outcome.rejection() != null) return reject(actor, outcome.rejection());
        return broadcastViews()
                .thenCompose(
                        ignored ->
                                outcome.settlement() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : settle(outcome.settlement()));
    }

    BetOutcome betDomain(String playerId, BetRequest request) {
        int i = index(playerId);
        String reason = i == Rules.NO_SEAT ? Rules.NOT_SEATED : null;
        if (reason == null
                && (request.handNumber() > Rules.MAX_CONTRACT_INTEGER
                        || request.amount() > Rules.MAX_CONTRACT_INTEGER))
            reason = Rules.INTEGER_OUT_OF_RANGE;
        if (reason == null && (!state.inHand || request.handNumber() != state.handNumber))
            reason = Rules.NO_HAND;
        if (reason == null && i != state.turnSeat) reason = Rules.NOT_YOUR_TURN;
        if (reason == null
                && (request.amount() < 0 || request.amount() > state.seats.get(i).chips))
            reason = Rules.INVALID_AMOUNT;
        if (reason != null) return new BetOutcome(reason, null);

        var seat = state.seats.get(i);
        var nextPot = Rules.tryAddContractIntegers(state.pot, request.amount());
        if (nextPot.isEmpty()) return new BetOutcome(Rules.INTEGER_OUT_OF_RANGE, null);

        int nextSeat = nextSeat(i);
        SettlementPlan settlement =
                nextSeat == Rules.NO_SEAT
                        ? createSettlement(i, request.amount(), nextPot.getAsLong())
                        : null;
        if (nextSeat == Rules.NO_SEAT && settlement == null)
            return new BetOutcome(Rules.INTEGER_OUT_OF_RANGE, null);

        seat.chips -= request.amount();
        state.pot = nextPot.getAsLong();
        seat.acted = true;
        state.turnSeat = nextSeat;
        return new BetOutcome(null, settlement);
    }

    private int nextSeat(int currentSeat) {
        for (int next = 0; next < state.seats.size(); next++)
            if (next != currentSeat
                    && !state.seats.get(next).cards.isEmpty()
                    && !state.seats.get(next).acted)
                return next;
        return Rules.NO_SEAT;
    }

    private SettlementPlan createSettlement(int bettor, long betAmount, long pot) {
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
        if (winners.isEmpty()) return null;

        long share = pot / winners.size(), remainder = pot % winners.size();
        var winnerBalances = new ArrayList<Long>(winners.size());
        for (int i = 0; i < winners.size(); i++) {
            int winner = winners.get(i);
            long balance =
                    state.seats.get(winner).chips - (winner == bettor ? betAmount : 0L);
            var withShare = Rules.tryAddContractIntegers(balance, share);
            if (withShare.isEmpty()) return null;
            balance = withShare.getAsLong();
            if (i == 0 && remainder != 0) {
                var withRemainder = Rules.tryAddContractIntegers(balance, remainder);
                if (withRemainder.isEmpty()) return null;
                balance = withRemainder.getAsLong();
            }
            winnerBalances.add(balance);
        }
        return new SettlementPlan(
                pot, List.copyOf(winners), List.copyOf(hands), List.copyOf(winnerBalances));
    }

    private CompletionStage<Void> settle(SettlementPlan plan) {
        HandResult result = applySettlement(plan);
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var seat : state.seats)
            sends = sends.thenCompose(ignored -> send(seat.playerId, result));
        return sends.thenCompose(ignored -> broadcastViews());
    }

    HandResult settleDomain(BetOutcome outcome) {
        return outcome.settlement() == null ? null : applySettlement(outcome.settlement());
    }

    private HandResult applySettlement(SettlementPlan plan) {
        state.inHand = false;
        state.turnSeat = Rules.NO_SEAT;
        state.pot = 0L;
        for (var seat : state.seats) seat.cards = List.of();
        for (int i = 0; i < plan.winners().size(); i++)
            state.seats.get(plan.winners().get(i)).chips = plan.winnerBalances().get(i);
        return new HandResult(
                state.handNumber, plan.pot(), plan.winners(), plan.hands(), seatViews());
    }

    record BetOutcome(String rejection, SettlementPlan settlement) {}

    record SettlementPlan(
            long pot, List<Integer> winners, List<RevealedHand> hands, List<Long> winnerBalances) {}

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

    private CompletionStage<Void> reject(Player actor, String reason) {
        return reply(actor, new ActionRejected(reason, 0));
    }

    private CompletionStage<Void> send(String id, Object message) {
        return clients.sendToActor(id, message).submit();
    }
}
