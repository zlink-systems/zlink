package systems.zlink.samples.deliverydispatch.server.tracking;

import java.net.URI;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.deliverydispatch.server.configuration.EvidenceStore;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleLocationStore;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleApplication;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTopology;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = TrackingServerApplication.class)
public final class TrackingServerApplication {
    private TrackingServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(TrackingServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer trackingFramework(SampleTopology topology) {
        return options -> {
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-tracking.log")
                .traceLabel("tracking");
            options.addHandlersFromPackageOf(TrackingServerApplication.class);
            ZLinkMeshNodeBuilder trackingSpot = options.addRouteMesh(SampleNames.CustomerSpotDiscovery);
            trackingSpot.listen(topology.trackingSpotEndpoint())
                .setRoutingIdPrefix("delivery-tracking");
            trackingSpot.objects().client();
            URI trackingEndpoint = URI.create(topology.trackingChannelEndpoint());
            options.addClientServerChannel(SampleNames.TrackingChannel)
                .server()
                .setBindHost(trackingEndpoint.getHost())
                .setAdvertiseHost(trackingEndpoint.getHost())
                .listen(trackingEndpoint.getPort())
                .addHandlerGroup("tracking");
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    EvidenceStore evidenceStore() {
        return new EvidenceStore();
    }
}
