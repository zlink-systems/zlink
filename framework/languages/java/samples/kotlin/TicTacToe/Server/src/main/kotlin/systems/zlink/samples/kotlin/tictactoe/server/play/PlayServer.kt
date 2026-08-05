package systems.zlink.samples.kotlin.tictactoe.server.play

import kotlinx.coroutines.Dispatchers
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLogging
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActorFactory
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActorRelocationAdapter
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.sessions.PlaySession

object PlayServer {
    fun configure(settings: SampleSettings): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            SampleLogging.configure(settings, "play")
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(PlayServer::class.java)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(SampleLogging.flowLogPath(settings, "play-${settings.playIndex}"))
                traceLabel("play-${settings.playIndex}")
            }
            options.addClientServerChannel(SampleNames.ApiChannel)
                .client()
                .connect(settings.apiChannelEndpoint)
            val node = options.addRouteMesh(SampleNames.SpotMesh)
            val routeEndpoint = settings.routeEndpoint.ifBlank { settings.spotEndpoint }

            node.listen(routeEndpoint)
                .setRoutingIdPrefix("tictactoe-play")
            node.channelName(SampleNames.PlayNode).server()
            node.peerConnections().connect(settings.peerSpotEndpoint)
            node.objects().server()
                .addEntrySpot(PlayEntrySpot::class.java)
                .addSpotFactory(
                    "tictactoe.game",
                    TicTacToeGame::class.java,
                ) { factory -> factory.disableRelocation() }
                .addActorFactory(
                    SampleNames.PlayActor,
                    PlayActor::class.java,
                    PlayActorFactory::class.java,
                ) { factory ->
                    factory.preserveStateWith(PlayActorRelocationAdapter::class.java)
                }
            options.addStreamNode(SampleNames.PlayStream)
                .bind(settings.playEndpoint)
                .enableActorDispatch()
                .registerSession(PlaySession::class.java)
        }
}
