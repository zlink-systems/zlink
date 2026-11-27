package MyGame.table;

import MyGame.Rules;

import java.util.*;

public final class TableState {
    public Map<String, Long> matchMembers;
    public List<Seat> seats = new ArrayList<>();
    public List<Integer> deck = new ArrayList<>();
    public long handNumber, pot;
    public boolean inHand;
    public int turnSeat = Rules.NO_SEAT;

    public static final class Seat {
        public String playerId;
        public long chips;
        public List<Integer> cards = List.of();
        public boolean acted;

        public Seat() {}

        public Seat(String playerId, long chips) {
            this.playerId = playerId;
            this.chips = chips;
        }
    }
}
