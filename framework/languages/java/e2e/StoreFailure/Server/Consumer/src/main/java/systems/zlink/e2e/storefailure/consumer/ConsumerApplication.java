package systems.zlink.e2e.storefailure.consumer;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ConsumerOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class ConsumerApplication {
    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    ZLinkFrameworkConfigurer consumerFramework(ConsumerOptions options) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/" + options.rid() + "-flow.log")
                .traceLabel(options.rid());
            framework.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(options.heartbeatMillis()));
            framework.configureLocations().setOwnerLeaseTtl(Duration.ofMillis(options.leaseTtlMillis()));
            framework.configureLocations().setPollingInterval(Duration.ofMillis(options.pollingMillis()));
            framework.configureLocations().setStoreFailureGrace(Duration.ofMillis(options.storeFailureGraceMillis()));
            //  참조 lane(`.NET` ConsumerHostFactory.JoinConsumerMesh)과 같다.
            //  Client membership은 provider가 서비스하는 channel 이름을 그대로 써야 한다.
            //  다른 이름으로 걸면 select-one 호출에 process-local client가 없다.
            framework.addRouteMesh(Contracts.CHANNEL)
                .listen("tcp://127.0.0.1:0")
                .setRoutingIdPrefix(options.rid())
                .channelName(Contracts.CHANNEL)
                .client();
        };
    }

    @Bean
    LocationStoreDelayState locationStoreDelayState(ConsumerOptions options) {
        return new LocationStoreDelayState(options.storeDelayControlFile());
    }

    @Bean
    ZLinkLocationStore locationStore(ConsumerOptions options) {
        ZLinkRedisLocationStore redisStore = new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis())));
        // The unified provider SPI is polled by the runtime; it no longer
        // exposes a separate change-notification capability to suppress here.
        return redisStore;
    }

    @Bean
    ConsumerEndpoints consumerEndpoints(
        ConsumerOptions options,
        systems.zlink.framework.channels.ZLinkClient client,
        systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle lifecycle,
        LocationStoreDelayState delayState,
        ObjectMapper json) {
        return new ConsumerEndpoints(
            options,
            client,
            lifecycle,
            lifecycle::monitoringLocationRuntimeQuery,
            delayState,
            json);
    }
}
