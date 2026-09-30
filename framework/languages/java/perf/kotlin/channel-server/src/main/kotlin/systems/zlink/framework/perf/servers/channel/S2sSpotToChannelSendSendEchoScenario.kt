package systems.zlink.framework.perf.servers.channel

import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication

internal object KotlinS2sSpotToChannelSendSendEchoScenario {
    fun run(config: RoleConfig) {
        require(config.role() == "channel" && !config.source())
        val app = ServerApplication.create(config).configure { options ->
            options.enableKotlinPerfHandlers()
            val mesh = ServerApplication.routeMesh(options, config, "perf-channel")
            mesh.objects().client()
            mesh.channelName(config.channelName()).server().addHandlerGroup(SPOT_RETURN_GROUP)
        }.bean(ChannelSpotReturnHandler::class.java)
        app.start()
    }
}
