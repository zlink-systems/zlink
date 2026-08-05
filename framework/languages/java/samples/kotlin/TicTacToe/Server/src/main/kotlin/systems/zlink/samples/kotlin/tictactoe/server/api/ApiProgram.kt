package systems.zlink.samples.kotlin.tictactoe.server.api

import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings

fun main(args: Array<String>) {
    ApiServerApplication.run(SampleSettings.configPath(args))
}
