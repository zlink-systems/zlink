package systems.zlink.framework.perf.servers.channel

import systems.zlink.framework.perf.ServerApplication

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    when (config.scenario()) {
        "channel-echo-only" -> KotlinChannelEchoOnlyScenario.run(config)
        "s2s-channel-to-spot-request-echo" -> KotlinS2sChannelToSpotRequestEchoScenario.run(config)
        "s2s-channel-to-spot-send-send-echo" -> KotlinS2sChannelToSpotSendSendEchoScenario.run(config)
        "s2s-spot-to-channel-request-echo" -> KotlinS2sSpotToChannelRequestEchoScenario.run(config)
        "s2s-spot-to-channel-send-send-echo" -> KotlinS2sSpotToChannelSendSendEchoScenario.run(config)
        else -> error("Unsupported Kotlin Channel scenario: ${config.scenario()}")
    }
}
