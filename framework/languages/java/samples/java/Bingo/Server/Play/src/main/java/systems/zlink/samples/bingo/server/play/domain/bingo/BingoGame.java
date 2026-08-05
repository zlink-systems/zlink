package systems.zlink.samples.bingo.server.play.domain.bingo;

public final class BingoGame {
    private BingoGame() {
    }

    public static BingoRoomGame room(String roomId, BingoRoomModels.BingoRoomSettings settings) {
        return new BingoRoomGame(roomId, settings);
    }
}
