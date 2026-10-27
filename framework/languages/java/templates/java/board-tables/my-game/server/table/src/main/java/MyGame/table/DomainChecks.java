package MyGame.table;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;
import java.security.SecureRandom;
import java.util.*;
import java.util.function.IntUnaryOperator;
import java.util.stream.IntStream;

final class DomainChecks {
    private DomainChecks() {}

    static void verifyJsonContract() {
        JsonNode json = Json.MAPPER.valueToTree(new ActionRejected("sample", Integer.MAX_VALUE));
        require(
                json.size() == 2
                        && json.has("reason")
                        && json.has("retryAfterMs")
                        && !json.has("RetryAfterMs")
                        && !json.has("retry_after_ms")
                        && json.get("retryAfterMs").isInt()
                        && json.get("retryAfterMs").intValue() == Integer.MAX_VALUE,
                "ActionRejected JSON does not use a signed 32-bit retryAfterMs field");
    }

    static int run() {
        int passed = 0;
        shuffledDeckContainsEveryCardOnce();
        passed++;
        injectedRandomProducesDeterministicOrder();
        passed++;
        settlementClearsAllPrivateHands();
        passed++;
        maximumBetAndOverLimitRejectionAreAtomic();
        passed++;
        potOverflowRejectionIsAtomic();
        passed++;
        payoutOverflowRejectionIsAtomic();
        passed++;
        handNumberOverflowRejectionIsAtomic();
        passed++;
        return passed;
    }

    private static void shuffledDeckContainsEveryCardOnce() {
        var table = table(secureRandomIndex());
        require(
                table.admit("p1") && table.admit("p2"),
                "Could not prepare shuffled deck");
        require(table.startDomain("p1") == null, "Deck shuffle failed");

        var cards = new ArrayList<Integer>(table.state.deck);
        for (var seat : table.state.seats) cards.addAll(seat.cards);
        var unique = new TreeSet<>(cards);
        var expected = new TreeSet<>(IntStream.range(0, Rules.DECK_SIZE).boxed().toList());
        require(
                cards.size() == Rules.DECK_SIZE
                        && unique.size() == Rules.DECK_SIZE
                        && unique.equals(expected),
                "Shuffled deck did not contain every card exactly once");
    }

    private static void injectedRandomProducesDeterministicOrder() {
        var table = table(bound -> bound - 1);
        require(
                table.admit("p1") && table.admit("p2"),
                "Could not prepare deterministic shuffle");
        require(table.startDomain("p1") == null, "Injected random source failed");
        require(
                table.state.deck.equals(IntStream.range(0, 48).boxed().toList())
                        && table.state.seats.get(0).cards.equals(List.of(51, 50))
                        && table.state.seats.get(1).cards.equals(List.of(49, 48)),
                "Injected random source produced a different deterministic order");
    }

    private static void settlementClearsAllPrivateHands() {
        var table = table(bound -> bound - 1);
        for (int i = 0; i < Rules.SEATS; i++)
            require(table.admit("p" + i), "Could not prepare settlement cards");
        require(table.startDomain("p0") == null, "Could not start settlement hand");

        HandResult result = null;
        for (int i = 0; i < Rules.SEATS; i++) {
            var outcome = table.betDomain("p" + i, new BetRequest(1, 0));
            require(outcome.rejection() == null, "Valid settlement bet was rejected");
            if (i == Rules.SEATS - 1) result = table.settleDomain(outcome);
            else
                require(
                        outcome.settlement() == null,
                        "Settlement ended before every player acted");
        }
        require(
                result != null
                        && result.seats().size() == Rules.SEATS
                        && result.seats().stream().allMatch(seat -> seat.cardCount() == 0)
                        && table.state.seats.stream().allMatch(seat -> seat.cards.isEmpty())
                        && IntStream.range(0, Rules.SEATS)
                                .allMatch(i -> table.privateHand("p" + i).isEmpty()),
                "Settlement retained private cards");
    }

    private static void maximumBetAndOverLimitRejectionAreAtomic() {
        var table = playingTable();
        table.state.seats.get(0).chips = Rules.MAX_CONTRACT_INTEGER;
        var accepted = table.betDomain("p1", new BetRequest(1, Rules.MAX_CONTRACT_INTEGER));
        require(
                accepted.rejection() == null
                        && accepted.settlement() == null
                        && table.state.pot == Rules.MAX_CONTRACT_INTEGER
                        && table.state.seats.get(0).chips == 0,
                "Maximum in-range bet was not accepted");

        JsonNode before = snapshot(table);
        var rejected = table.betDomain("p2", new BetRequest(1, Rules.MAX_CONTRACT_INTEGER + 1L));
        require(
                Rules.INTEGER_OUT_OF_RANGE.equals(rejected.rejection()),
                "Over-limit bet amount did not return integer-out-of-range");
        unchanged(table, before, "Over-limit bet rejection changed state");
    }

    private static void potOverflowRejectionIsAtomic() {
        var table = playingTable();
        table.state.pot = Rules.MAX_CONTRACT_INTEGER;
        JsonNode before = snapshot(table);
        var rejected = table.betDomain("p1", new BetRequest(1, 1));
        require(
                Rules.INTEGER_OUT_OF_RANGE.equals(rejected.rejection()),
                "Pot overflow did not return integer-out-of-range");
        unchanged(table, before, "Pot overflow rejection changed state");
    }

    private static void payoutOverflowRejectionIsAtomic() {
        var table = playingTable();
        table.state.seats.get(0).chips = Rules.MAX_CONTRACT_INTEGER;
        table.state.seats.get(0).cards = List.of(11, 24);
        table.state.seats.get(1).cards = List.of(0, 13);
        var first = table.betDomain("p1", new BetRequest(1, 0));
        require(
                first.rejection() == null && first.settlement() == null,
                "Could not prepare payout boundary");

        JsonNode before = snapshot(table);
        var rejected = table.betDomain("p2", new BetRequest(1, 1));
        require(
                Rules.INTEGER_OUT_OF_RANGE.equals(rejected.rejection()),
                "Payout overflow did not return integer-out-of-range");
        unchanged(table, before, "Payout overflow rejection changed state");
    }

    private static void handNumberOverflowRejectionIsAtomic() {
        var table = table(secureRandomIndex());
        require(table.admit("p1") && table.admit("p2"), "Could not prepare hand-number boundary");
        table.state.handNumber = Rules.MAX_CONTRACT_INTEGER;
        JsonNode before = snapshot(table);
        require(
                Rules.INTEGER_OUT_OF_RANGE.equals(table.startDomain("p1")),
                "Maximum hand number did not reject the next hand");
        unchanged(table, before, "Hand number overflow rejection changed state");
    }

    private static Table playingTable() {
        var table = table(secureRandomIndex());
        require(
                table.admit("p1") && table.admit("p2"), "Could not prepare bet boundary");
        table.state.seats.get(0).cards = List.of(11, 24);
        table.state.seats.get(1).cards = List.of(0, 13);
        table.state.handNumber = 1;
        table.state.inHand = true;
        table.state.turnSeat = 0;
        return table;
    }

    private static Table table(IntUnaryOperator randomIndex) {
        return new Table(null, null, null, randomIndex);
    }

    private static IntUnaryOperator secureRandomIndex() {
        var random = new SecureRandom();
        return random::nextInt;
    }

    private static JsonNode snapshot(Table table) {
        return Json.MAPPER.valueToTree(table.state);
    }

    private static void unchanged(Table table, JsonNode before, String message) {
        require(snapshot(table).equals(before), message);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

}
