package systems.zlink.samples.tictactoe.server.api;
import java.net.URI;

import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.tictactoe.server.api.handlers.AuthenticatePlayerHandler;
import systems.zlink.samples.tictactoe.server.configuration.SampleLogging;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.configuration.ApiSettings;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerReq;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerRes;

public final class ApiServer {
    private ApiServer() {
    }

    public static ZLinkFrameworkConfigurer configure(ApiSettings settings) {
        return options -> {
            SampleLogging.configure(settings, "api");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            options.configureLocations();
            URI apiEndpoint = URI.create(settings.apiChannelEndpoint());
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiEndpoint.getHost())
                .listen(apiEndpoint.getPort())
                // request: AuthenticatePlayerReq에 AuthenticatePlayerRes로 응답한다.
                .addRequestHandler(
                    AuthenticatePlayerHandler.class,
                    AuthenticatePlayerReq.class,
                    AuthenticatePlayerRes.class);
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(SampleNames.SpotMesh);
            mesh.listen(settings.routeEndpoint())
                .setRoutingIdPrefix("tictactoe-api");
            mesh.objects().client();
            for (String endpoint : settings.spotEndpoints()) {
                mesh.peerConnections().connect(endpoint);
            }
        };
    }
}
