package systems.zlink.samples.bingo.server.session;

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
import systems.zlink.samples.bingo.server.session.sessions.BingoSession;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTopology;
import systems.zlink.samples.bingo.server.configuration.BingoMetricsReporter;
import io.micrometer.core.instrument.MeterRegistry;



@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = SessionServerApplication.class)
public final class SessionServerApplication {
    private SessionServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(SessionServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer sessionFramework(SampleTopology topology) {
        return options -> {
            options.addHandlersFromPackageOf(SessionServerApplication.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-session.log")
                .traceLabel("session");
            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
            options.configureLocations();
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.Mesh);
            node.listen(topology.selectedSessionRouterEndpoint())
                .setRoutingIdPrefix("session");
            node.objects().client();
            options.addClientServerChannel(SampleNames.ApiChannel).client();
            node.channelName(SampleNames.RoomSpotDiscovery).client();
            options.addStreamNode(SampleNames.StreamNode)
                .bind(topology.selectedStreamEndpoint())
                .enableActorDispatch()
                .registerSession(BingoSession.class);
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean(destroyMethod = "close")
    BingoMetricsReporter bingoMetricsReporter(MeterRegistry registry) {
        return new BingoMetricsReporter(registry, "session");
    }
}
