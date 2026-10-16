package MyGame;

import java.util.List;

public record HandResult(
        int handNumber,
        int pot,
        List<Integer> winners,
        List<RevealedHand> hands,
        List<SeatView> seats) {}
