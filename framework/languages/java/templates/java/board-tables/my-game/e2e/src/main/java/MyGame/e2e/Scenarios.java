package MyGame.e2e;

import MyGame.*;

import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static final long TABLE_FIRST_BET = 100L, CONTINUITY_FIRST_BET = 10L, SECOND_BET = 50L;

    private static void check(boolean condition, String message) {
        Structure.require(condition, message);
    }

    private static void chips(TableView view) {
        check(
                view.seats().stream().mapToLong(SeatView::chips).sum() + view.pot()
                        == Rules.INITIAL_CHIPS * Rules.MIN_PLAYERS,
                "Chip total changed");
    }

    private static void same(TableView before, TableView after) {
        check(
                new TableView(
                                after.tableId(),
                                before.nodeId(),
                                after.yourSeat(),
                                after.handNumber(),
                                after.inHand(),
                                after.turnSeat(),
                                after.pot(),
                                after.seats())
                        .equals(before),
                "Table state changed: " + before + " -> " + after);
    }

    private static TableView view(Client client) throws Exception {
        client.clear();
        client.send(new TableViewRequest());
        return client.next(TableView.class);
    }

    private static void result(HandResult result, List<List<Integer>> cards, List<Long> bets) {
        check(
                result.hands().stream().map(RevealedHand::cards).toList().equals(cards),
                "Revealed cards differ from private hands");
        var scores =
                cards.stream()
                        .map(
                                hand ->
                                        hand.stream()
                                                .mapToInt(c -> c % Rules.RANKS + Rules.MIN_RANK)
                                                .sum())
                        .toList();
        int best = Collections.max(scores);
        long pot = bets.stream().mapToLong(Long::longValue).sum();
        var winners = new ArrayList<Integer>();
        for (int i = 0; i < scores.size(); i++) if (scores.get(i) == best) winners.add(i);
        check(
                result.winners().equals(winners) && result.pot() == pot,
                "Settlement winners or pot differ");
        for (int i = 0; i < bets.size(); i++) {
            long expected = Rules.INITIAL_CHIPS - bets.get(i);
            if (winners.contains(i))
                expected +=
                        pot / winners.size() + (i == winners.getFirst() ? pot % winners.size() : 0);
            var seat = result.seats().get(i);
            var hand = result.hands().get(i);
            check(
                    seat.seat() == i && hand.seat() == i && seat.playerId().equals(hand.playerId()),
                    "Settlement identity mismatch");
            check(
                    seat.chips() == expected && seat.acted(),
                    "Settlement seat differs");
        }
        check(
                result.seats().stream().allMatch(seat -> seat.cardCount() == 0),
                "Settlement retained a private card count");
        check(
                result.seats().stream().mapToLong(SeatView::chips).sum()
                        == Rules.INITIAL_CHIPS * Rules.MIN_PLAYERS,
                "Settlement chip total changed");
    }

    private static void privacy(Client player, HandDealt own) {
        synchronized (player.history) {
            for (var message : player.history) {
                if (message instanceof HandDealt dealt) {
                    if (!dealt.cards().isEmpty())
                        check(dealt.equals(own), "Foreign private hand received");
                }
                else if (message instanceof TableView table)
                    check(
                            table.seats().stream()
                                    .allMatch(
                                            s ->
                                                    s.cardCount() >= 0
                                                            && s.cardCount() <= Rules.CARDS),
                            "Invalid card count");
                else
                    check(
                            message instanceof ActionRejected,
                            "Unexpected pre-settlement payload: " + message);
            }
        }
    }

    static void run(Scenario id, Nodes nodes) throws Exception {
        run(id, nodes, null);
    }

    private static void run(Scenario id, Nodes nodes, String expectedNode) throws Exception {
        String suffix = UUID.randomUUID().toString(), tableId = "table-" + suffix;
        var p1 = new Client(nodes.endpoint, "p1-" + suffix, tableId);
        var p2 = new Client(nodes.endpoint, "p2-" + suffix, tableId);
        Throwable primary = null;
        try {
            var first = p1.connect();
            var second = p2.connect();
            if (expectedNode != null)
                check(
                        first.nodeId().equals(expectedNode) && second.nodeId().equals(expectedNode),
                        "Replacement node did not host new work");
            check(
                    first.yourSeat() == 0
                            && first.seats().size() == 1
                            && first.seats().getFirst().chips() == Rules.INITIAL_CHIPS,
                    "First seat differs");
            check(
                    second.yourSeat() == 1
                            && second.seats().stream()
                                    .map(SeatView::chips)
                                    .toList()
                                    .equals(List.of(Rules.INITIAL_CHIPS, Rules.INITIAL_CHIPS)),
                    "Second seat differs");
            var session1 = p1.sessionInfo();
            var session2 = p2.sessionInfo();
            check(
                    session1.actorId().equals(p1.playerId)
                            && session2.actorId().equals(p2.playerId),
                    "Actor identity differs from player");
            p2.clear();
            p2.send(new HandResult(0L, 0L, List.of(), List.of(), List.of()));
            var packetRejection = p2.next(ActionRejected.class);
            check(
                    packetRejection.reason().equals(Rules.PACKET_NOT_ALLOWED)
                            && packetRejection.retryAfterMs() == 0,
                    "Internal packet was not rejected");
            same(second, view(p2));
            check(
                    p2.next(HandDealt.class).cards().isEmpty(),
                    "Idle table returned private cards");
            p1.clear();
            p2.clear();
            p1.send(new StartHandRequest());
            var cards1 = p1.next(HandDealt.class);
            var cards2 = p2.next(HandDealt.class);
            check(
                    cards1.cards().size() == Rules.CARDS && cards2.cards().size() == Rules.CARDS,
                    "Wrong hand size");
            var allCards = new HashSet<Integer>(cards1.cards());
            allCards.addAll(cards2.cards());
            check(
                    allCards.size() == Rules.CARDS * Rules.MIN_PLAYERS
                            && allCards.stream().allMatch(c -> c >= 0 && c < Rules.DECK_SIZE),
                    "Cards are duplicate or outside deck");
            var start = p1.next(TableView.class, v -> v.inHand() && v.turnSeat() == 0);
            p2.next(TableView.class, v -> v.inHand() && v.turnSeat() == 0);
            if (id == Scenario.TABLE) {
                var beforeOutOfTurn = view(p2);
                p2.send(new BetRequest(cards2.handNumber(), SECOND_BET));
                var outOfTurn = p2.next(ActionRejected.class);
                check(
                        outOfTurn.reason().equals(Rules.NOT_YOUR_TURN)
                                && outOfTurn.retryAfterMs() == 0,
                        "Wrong out-of-turn rejection");
                same(beforeOutOfTurn, view(p2));

                var beforeLimit = view(p1);
                p1.send(new BetRequest(cards1.handNumber(), Rules.MAX_CONTRACT_INTEGER + 1L));
                var overLimit = p1.next(ActionRejected.class);
                check(
                        overLimit.reason().equals(Rules.INTEGER_OUT_OF_RANGE)
                                && overLimit.retryAfterMs() == 0,
                        "Over-limit bet was not rejected");
                same(beforeLimit, view(p1));

                var beforeAmount = view(p1);
                p1.send(
                        new BetRequest(
                                cards1.handNumber(), Rules.INITIAL_CHIPS + 1L));
                var invalidAmount = p1.next(ActionRejected.class);
                check(
                        invalidAmount.reason().equals(Rules.INVALID_AMOUNT)
                                && invalidAmount.retryAfterMs() == 0,
                        "Insufficient chips were not rejected");
                same(beforeAmount, view(p1));
            }
            long bet1 = id == Scenario.TABLE ? TABLE_FIRST_BET : CONTINUITY_FIRST_BET;
            p1.send(new BetRequest(cards1.handNumber(), bet1));
            var before1 = p1.next(TableView.class, v -> v.turnSeat() == 1 && v.pot() == bet1);
            var before2 = p2.next(TableView.class, v -> v.turnSeat() == 1 && v.pot() == bet1);
            chips(before1);
            switch (id) {
                case RECONNECT -> {
                    p2.close();
                    p2 = new Client(nodes.endpoint, p2.playerId, tableId);
                    var rejoined = p2.connect();
                    var session = p2.sessionInfo();
                    check(
                            !session.sessionId().equals(session2.sessionId())
                                    && session.actorId().equals(session2.actorId())
                                    && session.objectGeneration() == session2.objectGeneration(),
                            "Reconnect identity changed");
                    same(before2, rejoined);
                    check(p2.next(HandDealt.class).equals(cards2), "Reconnect hand changed");
                    check(
                            p1.sessionInfo().equals(session1)
                                    && p1.disconnects.get() == 0
                                    && p2.disconnects.get() == 0,
                            "Connection continuity failed");
                }
                case UPDATE -> {
                    nodes.drain(before1.nodeId());
                    var after1 = view(p1);
                    var after2 = view(p2);
                    check(
                            !after1.nodeId().equals(before1.nodeId())
                                    && after2.nodeId().equals(after1.nodeId()),
                            "Table did not relocate");
                    same(before1, after1);
                    same(before2, after2);
                    check(
                            p1.next(HandDealt.class).equals(cards1)
                                    && p2.next(HandDealt.class).equals(cards2),
                            "Relocated cards changed");
                    check(
                            p1.sessionInfo().equals(session1)
                                    && p2.sessionInfo().equals(session2)
                                    && p1.disconnects.get() == 0
                                    && p2.disconnects.get() == 0,
                            "Relocated identity or connection changed");
                }
                default -> {}
            }
            privacy(p1, cards1);
            privacy(p2, cards2);
            p2.send(new BetRequest(cards2.handNumber(), SECOND_BET));
            var result1 = p1.next(HandResult.class);
            var result2 = p2.next(HandResult.class);
            check(
                    result1.equals(result2) && result1.handNumber() == start.handNumber(),
                    "Settlement differs between clients");
            result(result1, List.of(cards1.cards(), cards2.cards()), List.of(bet1, SECOND_BET));
            for (var client : List.of(p1, p2)) {
                var settled = view(client);
                var emptyHand = client.next(HandDealt.class);
                check(
                        !settled.inHand()
                                && settled.handNumber() == result1.handNumber()
                                && settled.turnSeat() == Rules.NO_SEAT
                                && settled.pot() == 0L
                                && settled.seats().equals(result1.seats())
                                && emptyHand.cards().isEmpty(),
                        "Settlement did not clear every private hand");
            }
            if (id == Scenario.UPDATE) {
                nodes.replace(before1.nodeId());
                run(Scenario.TABLE, nodes, before1.nodeId());
            }
        } catch (Exception | Error failure) {
            primary = failure;
            throw failure;
        } finally {
            Exception cleanup = null;
            for (var client : List.of(p2, p1))
                try {
                    client.close();
                } catch (Exception error) {
                    if (cleanup == null) cleanup = error;
                    else cleanup.addSuppressed(error);
                }
            if (cleanup != null) {
                if (primary != null) primary.addSuppressed(cleanup);
                else throw cleanup;
            }
        }
    }
}
