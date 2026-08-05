package systems.zlink.samples.tictactoe.server.api;

import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.tictactoe.server.configuration.SampleLogging;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.configuration.ApiSettings;

public final class ApiServer {
    private ApiServer() {
    }

    public static ZLinkFrameworkConfigurer configure(ApiSettings settings) {
        return options -> {
            SampleLogging.configure(settings, "api");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(SampleLogging.flowLogPath(settings, "api-" + settings.apiHttpPort()))
                .traceLabel("api-" + settings.apiHttpPort());
            options.configureLocations();
            options.addHandlersFromPackageOf(ApiServer.class);
            java.net.URI apiEndpoint = java.net.URI.create(settings.apiChannelEndpoint());
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiEndpoint.getHost())
                .listen(apiEndpoint.getPort())
                .addHandlerGroup("api");
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
