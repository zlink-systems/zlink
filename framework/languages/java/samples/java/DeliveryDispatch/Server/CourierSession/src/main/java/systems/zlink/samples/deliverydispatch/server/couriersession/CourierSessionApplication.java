package systems.zlink.samples.deliverydispatch.server.couriersession;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleLocationStore;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleApplication;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;
import systems.zlink.samples.deliverydispatch.server.couriersession.sessions.CourierSession;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = CourierSessionApplication.class)
public final class CourierSessionApplication {
    private CourierSessionApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(CourierSessionApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer courierSessionFramework(SampleTopology topology) {
        return options -> {
            options.addHandlersFromPackageOf(CourierSessionApplication.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-courier-session.log")
                .traceLabel("courier-session");
            options.addClientServerChannel(SampleNames.CourierChannel)
                .client();
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.CourierSpotDiscovery);
            node.listen(topology.courierSessionSpotEndpoint())
                .setRoutingIdPrefix("delivery-session");
            node.objects().client();
            options.addStreamNode(SampleNames.CourierStreamNode)
                .bind(topology.courierStreamEndpoint())
                .enableActorDispatch()
                .registerSession(CourierSession.class);
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }
}
