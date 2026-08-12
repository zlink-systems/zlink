package systems.zlink.e2e.observabilityops.a5.server;
import org.springframework.beans.factory.ObjectProvider;
import java.net.URI;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import java.time.Duration;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import java.util.concurrent.CountDownLatch;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(Options.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: observability-ops-a5-server --config <path>");
        }
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(args[1]).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        builder.run();
        try {
            new CountDownLatch(1).await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    FlowEvidence flowEvidence() {
        return new FlowEvidence();
    }

    @Bean
    ProbeHandler probeHandler() {
        return new ProbeHandler();
    }

    @Bean
    HttpServer httpServer(
        ObjectMapper json,
        ObjectProvider<
            ZLinkFrameworkRuntime> runtimeProvider,
        ZLinkRouteClient routes,
        FlowEvidence evidence,
        Options config) {
        return new HttpServer(json, runtimeProvider, routes, evidence, config);
    }

    @Bean
    ZLinkRedisLocationStore locationStore(Options config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(500)));
    }

    @Bean
    ZLinkFrameworkConfigurer framework(Options config, FlowEvidence evidence) {
        return options -> {
            evidence.install();
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500));
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(3));
            options.configureLocations().setPollingInterval(Duration.ofMillis(250));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            URI endpoint = URI.create(config.routeEndpoint());
            var channel = options.addClientServerChannel(Contracts.CHANNEL);
            channel
                .server()
                .setBindHost(endpoint.getHost())
                .setAdvertiseHost(endpoint.getHost())
                .listen(endpoint.getPort())
                .addRequestHandler(
                ProbeHandler.class,
                Contracts.ProbeReq.class,
                Contracts.ProbeRes.class);
            channel.client().connect(config.routeEndpoint());
        };
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
