package MyGame;

public final class Rules {
    private Rules() {}

    public static final int SEATS = 4,
            INITIAL_CHIPS = 1000,
            DECK_SIZE = 52,
            RANKS = 13,
            MIN_RANK = 2,
            CARDS = 2,
            MIN_PLAYERS = 2,
            NO_SEAT = -1;
    public static final String TABLE_FULL = "table-full",
            NO_HAND = "no-hand",
            HAND_IN_PROGRESS = "hand-in-progress",
            NOT_ENOUGH_PLAYERS = "not-enough-players",
            NOT_YOUR_TURN = "not-your-turn",
            INVALID_AMOUNT = "invalid-amount",
            NOT_SEATED = "not-seated";
}
