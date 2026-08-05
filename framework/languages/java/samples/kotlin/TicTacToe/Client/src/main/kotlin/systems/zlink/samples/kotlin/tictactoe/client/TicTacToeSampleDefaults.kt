package systems.zlink.samples.kotlin.tictactoe.client

import java.time.Duration

object TicTacToeSampleDefaults {
    const val ApiUrl: String = "http://127.0.0.1:18081"
    const val GameName: String = "tictactoe-game"
    const val XActorId: String = "player-x"
    const val OActorId: String = "player-o"
    const val ObserverActorId: String = "player-observer"
    val RequestTimeout: Duration = Duration.ofSeconds(10)
}
