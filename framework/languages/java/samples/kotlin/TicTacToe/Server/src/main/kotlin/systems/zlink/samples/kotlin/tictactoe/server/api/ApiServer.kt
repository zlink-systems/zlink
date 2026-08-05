package systems.zlink.samples.kotlin.tictactoe.server.api

import kotlinx.coroutines.Dispatchers
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLogging
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.api.handlers.AuthenticatePlayerHandler
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerRes

object ApiServer {
    fun configure(settings: SampleSettings): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            SampleLogging.configure(settings, "api")
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(ApiServer::class.java)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(SampleLogging.flowLogPath(settings, "api-${settings.apiHttpPort}"))
                traceLabel("api-${settings.apiHttpPort}")
            }
            options.configureLocations()
            val apiEndpoint = java.net.URI.create(settings.apiChannelEndpoint)
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiEndpoint.host)
                .listen(apiEndpoint.port)
                .addHandlerGroup("api")
            val mesh = options.addRouteMesh(SampleNames.SpotMesh)
            mesh.listen(settings.routeEndpoint)
                .setRoutingIdPrefix("tictactoe-api")
            mesh.objects().client()
            settings.spotEndpoints.forEach { endpoint ->
                mesh.peerConnections().connect(endpoint)
            }
        }
}
