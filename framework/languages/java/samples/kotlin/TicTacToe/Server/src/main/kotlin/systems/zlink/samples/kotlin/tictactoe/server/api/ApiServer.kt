package systems.zlink.samples.kotlin.tictactoe.server.api


import java.net.URI
import kotlinx.coroutines.Dispatchers
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.tictactoe.server.api.handlers.AuthenticatePlayerHandler
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLogging
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticatePlayerRes

object ApiServer {
    fun configure(settings: SampleSettings): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            SampleLogging.configure(settings, "api")
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.NORMAL)
            }
            options.configureLocations()
            val apiEndpoint = URI.create(settings.apiChannelEndpoint)
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiEndpoint.host)
                .listen(apiEndpoint.port)
                // request: AuthenticatePlayerReq에 AuthenticatePlayerRes로 응답한다.
                .addRequestHandler(
                    AuthenticatePlayerHandler::class.java,
                    AuthenticatePlayerReq::class.java,
                    AuthenticatePlayerRes::class.java,
                )
            val mesh = options.addRouteMesh(SampleNames.SpotMesh)
            mesh.listen(settings.routeEndpoint)
                .setRoutingIdPrefix("tictactoe-api")
            mesh.objects().client()
            settings.spotEndpoints.forEach { endpoint ->
                mesh.peerConnections().connect(endpoint)
            }
        }
}
