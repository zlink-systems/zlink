package systems.zlink.samples.kotlin.tictactoe.client

import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) = runBlocking {
    val clientOptions = TicTacToeClientArguments.parse(args)
    TicTacToeClientScenario().run(clientOptions)
}
