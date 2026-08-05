package systems.zlink.e2e.kotlin.automaticturn;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ClientServerChannelBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.kotlin.automaticturn.delay")
public final class DelayApplication {
    private DelayApplication() {
    }

    public static AutoCloseable run(String... args) {
        SpringApplicationBuilder builder = new SpringApplicationBuilder(DelayApplication.class)
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run(args)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer framework() {
        return options -> {
            String nodeRid = Env.get("nodeRid", "delay-a");
            String logDir = Env.get("logDirectory", "logs");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/delay-flow.log")
                .traceLabel("kotlin-atd-delay");
            ClientServerChannelBuilder delay = options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .enableServer(Env.get("delayEndpoint"))
                .setRoutingId(RoutingId.from(nodeRid));
            delay.addRequestHandler(
                DelayHandler.class,
                Contracts.DelayReq.class,
                Contracts.DelayRes.class,
                "DelayReq");
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore() {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(Env.get("redisLocationEndpoint"))
            .setKeyPrefix(Env.get("locationKeyPrefix")));
    }
}
