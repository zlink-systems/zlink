package MyGame;

import java.util.OptionalLong;

public final class Rules {
    private Rules() {}

    public static final int SEATS = 4,
            DECK_SIZE = 52,
            RANKS = 13,
            MIN_RANK = 2,
            CARDS = 2,
            MIN_PLAYERS = 2,
            NO_SEAT = -1;
    public static final long INITIAL_CHIPS = 1000L, MAX_CONTRACT_INTEGER = 9_007_199_254_740_991L;
    public static final String TABLE_FULL = "table-full",
            NO_HAND = "no-hand",
            HAND_IN_PROGRESS = "hand-in-progress",
            NOT_ENOUGH_PLAYERS = "not-enough-players",
            NOT_YOUR_TURN = "not-your-turn",
            INVALID_AMOUNT = "invalid-amount",
            NOT_SEATED = "not-seated",
            INTEGER_OUT_OF_RANGE = "integer-out-of-range",
            PACKET_NOT_ALLOWED = "packet-not-allowed";

    public static boolean isContractInteger(long value) {
        return value >= 0 && value <= MAX_CONTRACT_INTEGER;
    }

    public static OptionalLong tryAddContractIntegers(long left, long right) {
        if (!isContractInteger(left)
                || !isContractInteger(right)
                || right > MAX_CONTRACT_INTEGER - left) return OptionalLong.empty();
        return OptionalLong.of(left + right);
    }
}
