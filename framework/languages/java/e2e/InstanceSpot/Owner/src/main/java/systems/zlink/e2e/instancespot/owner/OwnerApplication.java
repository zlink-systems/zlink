package systems.zlink.e2e.instancespot.owner;

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

@EnableZLinkFramework
@EnableConfigurationProperties(OwnerOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class OwnerApplication {
    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceStore evidenceStore(OwnerOptions options) {
        return new EvidenceStore(options);
    }

    @Bean
    GateController gateController() {
        return new GateController();
    }

    @Bean
    ZLinkFrameworkConfigurer ownerFramework(
        OwnerOptions options) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/" + options.rid() + "-flow.log")
                .traceLabel(options.rid());
            framework.addHandlersFromPackageOf(ProbeRequestHandler.class);
            var locations = framework.configureLocations();
            locations.setOwnerLeaseRenewInterval(Duration.ofMillis(options.heartbeatMillis()));
            locations.setOwnerLeaseTtl(Duration.ofMillis(options.leaseTtlMillis()));
            locations.setPollingInterval(Duration.ofMillis(options.pollingMillis()));
            locations.setStoreFailureGrace(Duration.ofMillis(options.storeFailureGraceMillis()));
            var mesh = framework.addRouteMesh(Contracts.MESH)
                .listen(options.meshEndpoint())
                .setRoutingIdPrefix(options.rid());
            mesh.channelName(Contracts.MESH)
                .server()
                .addHandlerGroup(Contracts.HANDLER_GROUP);
            mesh.objects().server().addInstanceSpotFactory(
                Contracts.STABLE_TYPE,
                ProbeSpot.class,
                factory -> {
                    if (options.stableTypeLimit() > 0) {
                        factory.stableTypeLimit(options.stableTypeLimit());
                    }
                    if (options.disableRelocation()) {
                        factory.disableRelocation();
                    }
                });
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(OwnerOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis())));
    }

    @Bean
    OwnerEndpoints ownerEndpoints(
        OwnerOptions options,
        EvidenceStore evidence,
        GateController gates,
        ObjectMapper json) {
        return new OwnerEndpoints(options, evidence, gates, json);
    }
}
