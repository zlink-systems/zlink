package systems.zlink.samples.bingo.server.api;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.samples.bingo.server.configuration.SampleLocationStore;
import systems.zlink.samples.bingo.server.configuration.SampleApplication;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTopology;



@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = ApiServerApplication.class)
public final class ApiServerApplication {
    private ApiServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(ApiServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer apiFramework(SampleTopology topology) {
        return options -> {
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-api.log")
                .traceLabel("api");
            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
            options.configureLocations();
            options.addHandlersFromPackageOf(ApiServerApplication.class);
            ZLinkMeshNodeBuilder api = options.addRouteMesh(SampleNames.Mesh)
                .setRoutingIdPrefix("api")
                .listen(topology.selectedApiMeshEndpoint());
            api.objects().client();
            java.net.URI apiChannelEndpoint = java.net.URI.create(
                topology.selectedApiChannelEndpoint());
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiChannelEndpoint.getHost())
                .listen(apiChannelEndpoint.getPort())
                .addHandlerGroup(SampleNames.ApiChannel);
            ZLinkMeshNodeBuilder matchmaking = options
                .addRouteMesh(SampleNames.MatchmakingMesh)
                .setRoutingIdPrefix("api-matchmaking")
                .listen(topology.apiMatchmakingRouterEndpoint());
            matchmaking.objects().client();
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }
}
