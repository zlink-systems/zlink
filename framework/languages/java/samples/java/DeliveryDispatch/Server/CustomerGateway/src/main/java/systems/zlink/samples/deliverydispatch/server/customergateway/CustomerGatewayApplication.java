package systems.zlink.samples.deliverydispatch.server.customergateway;

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
import systems.zlink.samples.deliverydispatch.server.customergateway.sessions.CustomerSession;
import systems.zlink.samples.deliverydispatch.server.customergateway.spots.CustomerEntrySpot;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = CustomerGatewayApplication.class)
public final class CustomerGatewayApplication {
    private CustomerGatewayApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(CustomerGatewayApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer customerGatewayFramework(SampleTopology topology) {
        return options -> {
            options.addHandlersFromPackageOf(CustomerGatewayApplication.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-customer-gateway.log")
                .traceLabel("customer-gateway");
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.CustomerSpotDiscovery);
            node.listen(topology.customerSpotRouterEndpoint())
                .setRoutingIdPrefix("delivery-customer");
            node.objects()
                .server()
                .addEntrySpot(CustomerEntrySpot.class)
                .addActorFactory(
                    SampleNames.CustomerActorType,
                    CustomerActor.class,
                    CustomerActorFactory.class,
                    factory -> factory.disableRelocation());
            options.addStreamNode(SampleNames.CustomerStreamNode)
                .bind(topology.customerStreamEndpoint())
                .enableActorDispatch()
                .registerSession(CustomerSession.class);
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    CustomerActorDirectory customerActorDirectory() {
        return new CustomerActorDirectory();
    }
}
