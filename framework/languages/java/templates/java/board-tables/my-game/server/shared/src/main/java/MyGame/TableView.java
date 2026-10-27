package MyGame;

import java.util.List;

public record TableView(
        String tableId,
        String nodeId,
        int yourSeat,
        long handNumber,
        boolean inHand,
        int turnSeat,
        long pot,
        List<SeatView> seats) {}
