package MyGame.table;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.IntUnaryOperator;
import java.util.stream.Collectors;

public final class Table implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final NodeConfig config;
    private final ZLinkActorClient clients;
    private final PlayerApi players;
    private final IntUnaryOperator randomIndex;
    TableState state = new TableState();

    public Table(
            ZLinkSpotContext context,
            NodeConfig config,
            ZLinkActorClient clients,
            PlayerApi players) {
        this(context, config, clients, players, new SecureRandom()::nextInt);
    }

    Table(
            ZLinkSpotContext context,
            NodeConfig config,
            ZLinkActorClient clients,
            PlayerApi players,
            IntUnaryOperator randomIndex) {
        this.context = context;
        this.config = config;
        this.clients = clients;
        this.players = players;
        this.randomIndex = Objects.requireNonNull(randomIndex);
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {
        context.handlers().addHandler(RoomPackets.Prepare.class);
        context.handlers().addHandler(RoomPackets.Commit.class);
        context.handlers().addHandler(RoomPackets.Withdraw.class);
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        var join = request.decode(TableAdmission.class);
        checkJoin(actorId, join.playerId(), join.tableId());
        boolean accepted;
        if (state.matchMembers == null) {
            accepted = admit(actorId, join.chips());
        } else {
            accepted = state.matchMembers.containsKey(actorId);
            if (accepted && state.seats.isEmpty()) state.matchMembers.put(actorId, join.chips());
        }
        return CompletableFuture.completedFuture(
                accepted ? ZLinkSpotActorJoinResult.accept() : ZLinkSpotActorJoinResult.reject());
    }

    void checkJoin(String actorId, String playerId, String tableId) {
        if (!playerId.equals(actorId) || !tableId.equals(context.spotId()))
            throw new IllegalArgumentException("Join identity mismatch");
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        var player = actor.context().actorId();
        int seat = index(player);
        if (seat == Rules.NO_SEAT) return CompletableFuture.completedFuture(null);
        logSeat(player, seat);
        return broadcastViews();
    }

    public CompletionStage<Void> onDisconnectActor(Player actor) {
        return actor.disconnected();
    }

    RoomPrepared prepare(PrepareRoom request) {
        boolean valid =
                context.spotId().equals(request.assignmentId())
                        && request.members() != null
                        && request.members().size() == Rules.MIN_PLAYERS
                        && new HashSet<>(request.members()).size() == Rules.MIN_PLAYERS
                        && request.members().stream().noneMatch(id -> id == null || id.isBlank());
        if (!valid) return new RoomPrepared(request.assignmentId(), false);
        if (state.matchMembers == null && state.seats.isEmpty()) {
            state.matchMembers = new LinkedHashMap<>();
            request.members().forEach(member -> state.matchMembers.put(member, null));
        }
        return new RoomPrepared(
                request.assignmentId(),
                state.matchMembers != null
                        && request.members().equals(new ArrayList<>(state.matchMembers.keySet())));
    }

    CompletionStage<Void> commit(CommitRoom message) {
        if (!uncommitted(message.assignmentId()) || state.matchMembers.isEmpty())
            return CompletableFuture.completedFuture(null);
        var seats = new ArrayList<TableState.Seat>();
        state.matchMembers.forEach(
                (member, chips) ->
                        seats.add(new TableState.Seat(member, Objects.requireNonNull(chips))));
        state.seats = seats;
        state.matchMembers.replaceAll((member, chips) -> null);
        for (int i = 0; i < seats.size(); i++) logSeat(seats.get(i).playerId, i);
        return broadcastViews();
    }

    CompletionStage<Void> withdraw(WithdrawRoom message) {
        if (!uncommitted(message.assignmentId())) return CompletableFuture.completedFuture(null);
        state.matchMembers = Map.of();
        return context.close().thenAccept(closed -> {});
    }

    private boolean uncommitted(String assignmentId) {
        return context.spotId().equals(assignmentId)
                && state.matchMembers != null
                && state.seats.isEmpty();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        if (state.matchMembers == null || !state.matchMembers.isEmpty())
            return CompletableFuture.completedFuture(null);
        // A Spot can close itself from a packet handler, after the leave callback completes.
        return context.outbound()
                .sendToSpot(context.spotId(), new WithdrawRoom(context.spotId()))
                .submit();
    }

    // Seats the player at admission; a seated player is admitted again without a new seat.
    boolean admit(String id, long chips) {
        if (index(id) != Rules.NO_SEAT) return true;
        if (state.seats.size() == Rules.SEATS) return false;
        state.seats.add(new TableState.Seat(id, chips));
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
        if (state.matchMembers != null && i == Rules.NO_SEAT)
            return CompletableFuture.completedFuture(null);
        var response = reply(actor, view(id, seatViews()));
        if (i != Rules.NO_SEAT) {
            var dealt = new HandDealt(state.handNumber, privateHand(id));
            return response.thenCompose(ignored -> reply(actor, dealt));
        }
        return response;
    }

    public CompletionStage<Void> start(Player actor) {
        var player = actor.context().actorId();
        String reason = startDomain(player);
        if (reason != null) {
            log(GameLog.shortId(player) + " start rejected (" + reason + ")");
            return reject(actor, reason);
        }
        log("hand " + state.handNumber + " started, seat " + state.turnSeat + " to act");

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
        var player = actor.context().actorId();
        BetOutcome outcome = betDomain(player, request);
        var bet = GameLog.shortId(player) + " bet " + request.amount();
        if (outcome.rejection() != null) {
            log(bet + " rejected (" + outcome.rejection() + ")");
            return reject(actor, outcome.rejection());
        }
        log(bet + (state.turnSeat == Rules.NO_SEAT ? "" : ", seat " + state.turnSeat + " to act"));
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
        if (reason == null && (request.amount() < 0 || request.amount() > state.seats.get(i).chips))
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
                    && !state.seats.get(next).acted) return next;
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
        var balances = new ArrayList<Long>(state.seats.size());
        for (int i = 0; i < state.seats.size(); i++)
            balances.add(state.seats.get(i).chips - (i == bettor ? betAmount : 0L));
        for (int i = 0; i < winners.size(); i++) {
            int winner = winners.get(i);
            long balance = balances.get(winner);
            var withShare = Rules.tryAddContractIntegers(balance, share);
            if (withShare.isEmpty()) return null;
            balance = withShare.getAsLong();
            if (i == 0 && remainder != 0) {
                var withRemainder = Rules.tryAddContractIntegers(balance, remainder);
                if (withRemainder.isEmpty()) return null;
                balance = withRemainder.getAsLong();
            }
            balances.set(winner, balance);
        }
        return new SettlementPlan(
                pot, List.copyOf(winners), List.copyOf(hands), List.copyOf(balances));
    }

    private CompletionStage<Void> settle(SettlementPlan plan) {
        var results = new LinkedHashMap<String, Long>();
        for (int i = 0; i < state.seats.size(); i++)
            results.put(state.seats.get(i).playerId, plan.balances().get(i));
        return players.commitRound(context.spotId(), state.handNumber, results)
                .thenCompose(ignored -> announceSettlement(applySettlement(plan)));
    }

    private CompletionStage<Void> announceSettlement(HandResult result) {
        var winners =
                result.winners().stream()
                        .map(seat -> "seat " + seat)
                        .collect(Collectors.joining(", "));
        log(
                "hand "
                        + result.handNumber()
                        + " settled, winner "
                        + winners
                        + ", pot "
                        + result.pot());
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
        for (int i = 0; i < state.seats.size(); i++) {
            var seat = state.seats.get(i);
            seat.cards = List.of();
            seat.chips = plan.balances().get(i);
        }
        return new HandResult(
                state.handNumber, plan.pot(), plan.winners(), plan.hands(), seatViews());
    }

    record BetOutcome(String rejection, SettlementPlan settlement) {}

    record SettlementPlan(
            long pot, List<Integer> winners, List<RevealedHand> hands, List<Long> balances) {}

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
        return actor.reply(message);
    }

    private CompletionStage<Void> reject(Player actor, String reason) {
        return reply(actor, new ActionRejected(reason, 0));
    }

    private CompletionStage<Void> send(String id, Object message) {
        return clients.sendToActor(id, message).submit();
    }

    void log(String message) {
        GameLog.table(context.spotId(), message);
    }

    private void logSeat(String player, int seat) {
        log(GameLog.shortId(player) + " joined (seat " + seat + ")");
    }
}
