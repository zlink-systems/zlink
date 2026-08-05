package systems.zlink.e2e.storefailure.provider;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ProviderOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class ProviderApplication {
    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    ProviderEvidenceStore providerEvidenceStore(ProviderOptions options) {
        return new ProviderEvidenceStore(options);
    }

    @Bean
    ZLinkFrameworkConfigurer providerFramework(
        ProviderOptions options,
        ProviderEvidenceStore evidence) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/" + options.rid() + "-flow.log")
                .traceLabel(options.rid());
            framework.addHandlersFromPackageOf(ProfileReqHandler.class);
            framework.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(options.heartbeatMillis()));
            framework.configureLocations().setOwnerLeaseTtl(Duration.ofMillis(options.leaseTtlMillis()));
            framework.configureLocations().setPollingInterval(Duration.ofMillis(options.pollingMillis()));
            framework.configureLocations().setStoreFailureGrace(Duration.ofMillis(options.storeFailureGraceMillis()));
            //  참조 lane인 `.NET` provider와 같은 topology다. ClientServer channel이
            //  아니라 RouteMesh node 위의 channel server이며, routing id는 mesh node의
            //  prefix로 정한다(ClientServer server builder에는 routing id 설정이 없다).
            framework.addRouteMesh(Contracts.CHANNEL)
                .listen(options.channelEndpoint())
                .setRoutingIdPrefix(options.rid())
                .channelName(Contracts.CHANNEL)
                .server()
                .addHandlerGroup(Contracts.HANDLER_GROUP);
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ProviderOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis())));
    }

    @Bean
    ProfileReqHandler profileRequestHandler(ProviderEvidenceStore evidence) {
        return new ProfileReqHandler(evidence);
    }

    @Bean
    ProviderEndpoints providerEndpoints(
        ProviderOptions options,
        ProviderEvidenceStore evidence,
        ObjectMapper json) {
        return new ProviderEndpoints(options, evidence, json);
    }
}
