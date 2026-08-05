package systems.zlink.samples.kotlin.tictactoe.server.play

import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings

fun main(args: Array<String>) {
    PlayServerApplication.run(SampleSettings.configPath(args))
}
