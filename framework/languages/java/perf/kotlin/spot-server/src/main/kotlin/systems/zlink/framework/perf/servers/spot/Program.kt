package systems.zlink.framework.perf.servers.spot

import systems.zlink.framework.perf.ServerApplication

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.role() == "spot") { "spot-server runs the Spot role." }
    when (config.scenario()) {
        "s2s-channel-to-spot-request-echo", "s2s-channel-to-spot-send-send-echo" ->
            if (!config.source()) S2sChannelToSpotEchoTarget.run(config) else error("Channel is the source for this scenario.")
        "s2s-spot-to-channel-request-echo" -> S2sSpotToChannelRequestEchoScenario.run(config)
        "s2s-spot-to-channel-send-send-echo" -> S2sSpotToChannelSendSendEchoScenario.run(config)
        "spot-no-await-echo" -> SpotNoAwaitEchoScenario.run(config)
        "spot-worker-offload-echo" -> SpotWorkerOffloadEchoScenario.run(config)
        else -> error("Unsupported Kotlin Spot scenario: ${config.scenario()}")
    }
}
