package systems.zlink.samples.deliverydispatch.server.dispatch;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.deliverydispatch.server.configuration.DeliveryDispatchReadinessReporter;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleApplication;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleLocationStore;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

import java.io.IOException;
import java.net.URI;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
        proxyBeanMethods = false,
        scanBasePackageClasses = DispatchServerApplication.class)
public final class DispatchServerApplication {
    private DispatchServerApplication() {}

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(DispatchServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer dispatchFramework(SampleTopology topology) {
        return options -> {
            options.addHandlersFromPackageOf(DispatchServerApplication.class);
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            options.addClientServerChannel(SampleNames.CourierChannel).client();
            // --8<-- [start:doc-dd-dispatch-register]
            // The courier's decision comes back here as its own one-way message, so dispatch has
            // to be a channel server (common sample spec section 7.4).
            URI dispatchEndpoint = URI.create(topology.dispatchChannelEndpoint());
            options.addClientServerChannel(SampleNames.DispatchChannel).client();
            options.addClientServerChannel(SampleNames.DispatchChannel)
                    .server()
                    .setBindHost(dispatchEndpoint.getHost())
                    .setAdvertiseHost(dispatchEndpoint.getHost())
                    .listen(dispatchEndpoint.getPort())
                    .addHandlerGroup(SampleNames.DispatchChannel);
            options.addClientServerChannel(SampleNames.TrackingChannel).client();
            ZLinkMeshNodeBuilder courierRoutes =
                    options.addRouteMesh(SampleNames.CourierSpotDiscovery);
            courierRoutes
                    .listen(topology.dispatchSpotEndpoint())
                    .setRoutingId(RoutingId.from(SampleNames.DispatchNode));
            courierRoutes.objects().client();
            // --8<-- [end:doc-dd-dispatch-register]
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    DeliveryDispatchReadinessReporter readinessReporter(ZLinkRouteMeshRuntime routeMeshRuntime) {
        return new DeliveryDispatchReadinessReporter(
                routeMeshRuntime,
                SampleNames.DispatchNode,
                SampleNames.CourierSpotDiscovery,
                SampleNames.CourierNode1,
                SampleNames.CourierNode2);
    }

    @Bean
    DispatchWorkQueue dispatchWorkQueue(DispatchWorker worker) {
        return new DispatchWorkQueue(worker);
    }

    @Bean
    DeliveryOfferStore deliveryOfferStore() {
        return new DeliveryOfferStore();
    }

    @Bean
    DispatchWorker dispatchWorker(
            ZLinkClient channels, ZLinkActorClient actors, DeliveryOfferStore offers) {
        return new DispatchWorker(channels, actors, offers);
    }

    @Bean(destroyMethod = "close")
    OfferDeadlineSweeper offerDeadlineSweeper(DeliveryOfferStore offers, DispatchWorker worker) {
        return new OfferDeadlineSweeper(offers, worker);
    }

    @Bean
    DispatchHttpServer dispatchHttpServer(
            ObjectMapper json,
            ZLinkClient channels,
            DispatchWorkQueue queue,
            SampleTopology topology)
            throws IOException {
        return new DispatchHttpServer(json, channels, queue, topology.dispatchHttpEndpoint());
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }
}
