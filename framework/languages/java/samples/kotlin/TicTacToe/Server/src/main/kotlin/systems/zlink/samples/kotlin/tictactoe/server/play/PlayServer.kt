package systems.zlink.samples.kotlin.tictactoe.server.play

import kotlinx.coroutines.Dispatchers
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLogging
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActorFactory
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActorRelocationAdapter
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions.PlaySession
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions.handlers.AuthenticatePlaySessionHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame

object PlayServer {
    fun configure(settings: SampleSettings): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            SampleLogging.configure(settings, "play")
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch { messageFlow(ZLinkMessageFlowLogMode.NORMAL) }
            // --8<-- [start:doc-ttt-play-register]
            val apiClient = options.addClientServerChannel(SampleNames.ApiChannel).client()
            settings.apiChannelEndpoints.forEach { endpoint ->
                // Api A와 Api B를 모두 수동 등록하고 request마다 가용 endpoint를 선택한다.
                apiClient.connect(endpoint)
            }
            val node = options.addRouteMesh(SampleNames.SpotMesh)
            val routeEndpoint = settings.routeEndpoint.ifBlank { settings.spotEndpoint }
            val peerNodeId = if (settings.nodeId == "play-a") "play-b" else "play-a"

            node
                .setRoutingId(RoutingId.from("tictactoe-play-${settings.nodeId}"))
                .listen(routeEndpoint)
            node.channelName(SampleNames.PlayNode).server()
            node
                .peerConnections()
                .connect(RoutingId.from("tictactoe-play-$peerNodeId"), settings.peerSpotEndpoint)
            node
                .objects()
                .server()
                // #895: entry-spot registration has no Kotlin form in the spec.
                .addEntrySpot(PlayEntrySpot::class.java)
                .addSpotFactory("tictactoe.game", TicTacToeGame::class.java) { factory ->
                    factory.disableRelocation()
                }
                .addActorFactory(
                    SampleNames.PlayActor,
                    PlayActor::class.java,
                    PlayActorFactory::class.java,
                ) { factory ->
                    // #895: state preservation configuration has no Kotlin form in the spec.
                    factory.preserveStateWith(PlayActorRelocationAdapter::class.java)
                }
            options
                .addStreamNode(SampleNames.PlayStream)
                .bind(settings.playEndpoint)
                .enableActorDispatch()
                // #895: session registration has no Kotlin form in the spec.
                .registerSession(PlaySession::class.java)
                // request: STREAM AuthenticateReq를 처리하고 AuthenticateRes를 reply한다.
                // #895: session packet registration has no Kotlin form in the spec.
                .addSessionPacketHandler(AuthenticatePlaySessionHandler::class.java)
            // --8<-- [end:doc-ttt-play-register]
        }
}
