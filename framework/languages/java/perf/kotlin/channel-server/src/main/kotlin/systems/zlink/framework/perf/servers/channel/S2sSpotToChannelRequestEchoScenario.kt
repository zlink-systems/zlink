package systems.zlink.framework.perf.servers.channel

import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication

internal object KotlinS2sSpotToChannelRequestEchoScenario {
    fun run(config: RoleConfig) {
        require(config.role() == "channel" && !config.source())
        val app = ServerApplication.create(config).configure { options ->
            options.enableKotlinPerfHandlers()
            ServerApplication.routeMesh(options, config, "perf-channel").channelName(config.channelName())
                .server().addHandlerGroup(BASELINE_GROUP)
        }.bean(KotlinChannelEchoHandler::class.java)
        app.start()
    }
}
