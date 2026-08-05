package systems.zlink.samples.kotlin.bingo.server.play.domain.bingo

object BingoGame {
    fun room(
        roomId: String,
        settings: BingoRoomSettings,
    ): BingoRoomGame = BingoRoomGame(roomId, settings)
}
