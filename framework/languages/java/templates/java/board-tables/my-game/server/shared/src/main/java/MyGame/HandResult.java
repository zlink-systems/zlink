package MyGame;

import java.util.List;

public record HandResult(
        long handNumber,
        long pot,
        List<Integer> winners,
        List<RevealedHand> hands,
        List<SeatView> seats) {}
