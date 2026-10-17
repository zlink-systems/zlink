package MyGame.table;

import MyGame.Rules;

import java.util.*;

public final class TableState {
    public List<Seat> seats = new ArrayList<>();
    public List<Integer> deck = new ArrayList<>();
    public int handNumber, pot;
    public boolean inHand;
    public int turnSeat = Rules.NO_SEAT;

    public static final class Seat {
        public String playerId;
        public int chips = Rules.INITIAL_CHIPS;
        public List<Integer> cards = List.of();
        public boolean acted;

        public Seat() {}

        public Seat(String playerId) {
            this.playerId = playerId;
        }
    }
}
