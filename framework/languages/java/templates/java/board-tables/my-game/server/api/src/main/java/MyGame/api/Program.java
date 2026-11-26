package MyGame.api;

import MyGame.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.*;

import java.net.URI;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public class Program {
    @Bean
    public ZLinkFrameworkConfigurer framework(
            NodeConfig config, ZLinkRedisLocationStore locations) {
        return options -> {
            var endpoint = URI.create(config.apiEndpoint());
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            options.addLocationStore(locations);
            options.addHandlersFromPackageOf(Program.class);
            options.addClientServerChannel(Names.API)
                    .server()
                    .setBindHost(endpoint.getHost())
                    .listen(endpoint.getPort())
                    .addHandlerGroup(Names.API);
        };
    }

    @Bean(destroyMethod = "close")
    public PlayerStore players(NodeConfig config) {
        return new PlayerStore(config.redisEndpoint(), config.keyPrefix());
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(Program.class, args);
    }
}
