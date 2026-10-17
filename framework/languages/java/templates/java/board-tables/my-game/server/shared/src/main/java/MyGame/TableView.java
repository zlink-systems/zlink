package MyGame;

import java.util.List;

public record TableView(
        String tableId,
        String nodeId,
        int yourSeat,
        int handNumber,
        boolean inHand,
        int turnSeat,
        int pot,
        List<SeatView> seats) {}
