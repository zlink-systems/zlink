package systems.zlink.samples.tictactoe.server.play;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.tictactoe.server.configuration.SampleLogging;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.configuration.PlaySettings;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActorFactory;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActorRelocationAdapter;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.sessions.handlers.AuthenticatePlaySessionHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.PlayEntrySpot;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.sessions.PlaySession;

public final class PlayServer {
    private PlayServer() {
    }

    public static ZLinkFrameworkConfigurer configure(PlaySettings settings) {
        return options -> {
            SampleLogging.configure(settings, "play");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.DETAILED);
            var apiClient = options.addClientServerChannel(SampleNames.ApiChannel)
                .client();
            for (String endpoint : settings.apiChannelEndpoints()) {
                // Api A와 Api B를 모두 수동 등록하고 request마다 가용 endpoint를 선택한다.
                apiClient.connect(endpoint);
            }
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.SpotMesh);
            String routeEndpoint = settings.routeEndpoint().isBlank()
                ? settings.spotEndpoint()
                : settings.routeEndpoint();
            node.listen(routeEndpoint)
                .setRoutingId(RoutingId.from("tictactoe-" + settings.nodeId()));
            node.channelName(SampleNames.PlayNode).server();
            node.peerConnections().connect(settings.peerSpotEndpoint());
            node.objects()
                .server()
                .addEntrySpot(PlayEntrySpot.class)
                .addSpotFactory(
                    "tictactoe.game",
                    TicTacToeGame.class,
                    factory -> factory.disableRelocation())
                .addActorFactory(
                    SampleNames.PlayActor,
                    PlayActor.class,
                    PlayActorFactory.class,
                    factory -> factory.preserveStateWith(
                        PlayActorRelocationAdapter.class));
            options.addStreamNode(SampleNames.PlayStream)
                .bind(settings.playEndpoint())
                .enableActorDispatch()
                .registerSession(PlaySession.class)
                // request: STREAM AuthenticateReq를 처리하고 AuthenticateRes를 reply한다.
                .addSessionPacketHandler(AuthenticatePlaySessionHandler.class);
        };
    }
}
