package systems.zlink.e2e.instancespot.client;
import systems.zlink.framework.channels.ZLinkRouteClient;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkSpotManager;

@EnableZLinkFramework
@EnableConfigurationProperties(ClientOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class ClientApplication {
    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    ZLinkFrameworkConfigurer clientFramework(ClientOptions options) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var locations = framework.configureLocations();
            locations.setOwnerLeaseRenewInterval(Duration.ofMillis(options.heartbeatMillis()));
            locations.setOwnerLeaseTtl(Duration.ofMillis(options.leaseTtlMillis()));
            locations.setPollingInterval(Duration.ofMillis(options.pollingMillis()));
            locations.setStoreFailureGrace(Duration.ofMillis(options.storeFailureGraceMillis()));
            var mesh = framework.addRouteMesh(Contracts.MESH);
            mesh.listen(options.meshEndpoint())
                .setRoutingIdPrefix(options.rid());
            mesh.channelName(Contracts.MESH).client();
            // The caller uses the public Spot lookup API for Ready evidence;
            // no Spot factory is registered on this process.
            mesh.objects().server();
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ClientOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis())));
    }

    @Bean
    ClientEndpoints clientEndpoints(
        ClientOptions options,
        ZLinkRouteClient routes,
        ZLinkSpotManager spots,
        ObjectMapper json) {
        return new ClientEndpoints(options, routes, spots, json);
    }
}
