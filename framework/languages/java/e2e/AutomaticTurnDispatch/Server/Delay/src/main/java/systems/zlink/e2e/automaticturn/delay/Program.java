package systems.zlink.e2e.automaticturn.delay;

import java.nio.file.Path;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.shared.DelayHandler;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(DelayOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.automaticturn.delay")
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        builder.run();
    }

    @Bean
    ZLinkFrameworkConfigurer framework(DelayOptions config) {
        return options -> {
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(config.logDirectory() + "/delay-flow.log")
                .traceLabel("java-atd-delay");
            options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .enableServer(config.delayEndpoint())
                .setRoutingId(RoutingId.from("delay-a"))
                .addRequestHandler(
                    DelayHandler.class,
                    Contracts.DelayReq.class,
                    Contracts.DelayRes.class,
                    "DelayReq");
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(DelayOptions config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix()));
    }

    @Bean
    DelayHandler delayHandler() {
        return new DelayHandler();
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: automatic-turn-dispatch-delay --config <path>");
        }
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
