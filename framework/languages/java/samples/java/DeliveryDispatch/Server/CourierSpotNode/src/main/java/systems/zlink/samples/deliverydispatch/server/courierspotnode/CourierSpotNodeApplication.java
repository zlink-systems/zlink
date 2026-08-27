package systems.zlink.samples.deliverydispatch.server.courierspotnode;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.samples.deliverydispatch.server.configuration.DeliveryDispatchReadinessReporter;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleLocationStore;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleApplication;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = CourierSpotNodeApplication.class)
public final class CourierSpotNodeApplication {
    private CourierSpotNodeApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(CourierSpotNodeApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer courierSpotNodeFramework(SampleTopology topology) {
        return options -> {
            String node = topology.courierNode();
            NodeOptions selected = NodeOptions.resolve(node, topology);
            options.addHandlersFromPackageOf(CourierSpotNodeApplication.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkMeshNodeBuilder spotNode = options.addRouteMesh(SampleNames.CourierSpotDiscovery);
            spotNode.listen(selected.spotEndpoint())
                .setRoutingId(RoutingId.from(selected.nodeName()));
            spotNode.objects()
                .server()
                .addEntrySpot(CourierEntrySpot.class)
                .addActorFactory(
                    SampleNames.CourierActorType,
                    CourierActor.class,
                    CourierActorFactory.class,
                    factory -> factory.disableRelocation());
            // The courier's decision goes back to dispatch as its own one-way message, so this
            // node needs a way to speak to the dispatch channel (common sample spec section 7.4).
            options.addClientServerChannel(SampleNames.DispatchChannel)
                .client();
        };
    }

    @Bean
    ActorDirectory actorDirectory() {
        return new ActorDirectory();
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    DeliveryDispatchReadinessReporter readinessReporter(
        SampleTopology topology,
        ZLinkRouteMeshRuntime routeMeshRuntime) {
        return new DeliveryDispatchReadinessReporter(
            routeMeshRuntime,
            NodeOptions.resolve(topology.courierNode(), topology).nodeName(),
            SampleNames.CourierSpotDiscovery);
    }

    private record NodeOptions(String nodeName, String spotEndpoint) {
        static NodeOptions resolve(String node, SampleTopology topology) {
            return switch (node) {
                case "node2" -> new NodeOptions(
                    SampleNames.CourierNode2,
                    topology.courierActorNode2SpotEndpoint());
                default -> new NodeOptions(
                    SampleNames.CourierNode1,
                    topology.courierActorNode1SpotEndpoint());
            };
        }
    }
}
