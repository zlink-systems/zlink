package systems.zlink.samples.deliverydispatch.server.dispatch;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.net.URI;
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

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = DispatchServerApplication.class)
public final class DispatchServerApplication {
    private DispatchServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(DispatchServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer dispatchFramework(SampleTopology topology) {
        return options -> {
            options.addHandlersFromPackageOf(DispatchServerApplication.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-dispatch.log")
                .traceLabel("dispatch");
            options.addClientServerChannel(SampleNames.CourierChannel)
                .client();
            // The courier's decision comes back here as its own one-way message, so dispatch has
            // to be a channel server (common sample spec section 7.4).
            URI dispatchEndpoint = URI.create(topology.dispatchChannelEndpoint());
            options.addClientServerChannel(SampleNames.DispatchChannel)
                .server()
                .setBindHost(dispatchEndpoint.getHost())
                .setAdvertiseHost(dispatchEndpoint.getHost())
                .listen(dispatchEndpoint.getPort())
                .addHandlerGroup(SampleNames.DispatchChannel);
            options.addClientServerChannel(SampleNames.TrackingChannel)
                .client();
            ZLinkMeshNodeBuilder courierRoutes = options.addRouteMesh(SampleNames.CourierSpotDiscovery);
            courierRoutes
                .listen(topology.dispatchSpotEndpoint())
                .setRoutingIdPrefix("delivery-dispatch");
            courierRoutes.objects().client();
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
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
        systems.zlink.framework.channels.ZLinkClient channels,
        systems.zlink.framework.actors.ZLinkActorClient actors,
        DeliveryOfferStore offers) {
        return new DispatchWorker(channels, actors, offers);
    }

    @Bean(destroyMethod = "close")
    OfferDeadlineSweeper offerDeadlineSweeper(DeliveryOfferStore offers, DispatchWorker worker) {
        return new OfferDeadlineSweeper(offers, worker);
    }

    @Bean
    DispatchHttpServer dispatchHttpServer(
        ObjectMapper json,
        DispatchWorkQueue queue,
        SampleTopology topology) throws IOException {
        return new DispatchHttpServer(json, queue, topology.dispatchHttpEndpoint());
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }
}
