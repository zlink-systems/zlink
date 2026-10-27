package MyGame.session;

import MyGame.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.spring.*;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public class Program {
    @Bean
    public ZLinkFrameworkConfigurer framework(
            NodeConfig config,
            ZLinkRedisLocationStore locations,
            ZLinkRedisRelocationStore relocations) {
        return options -> {
            Hosting.configure(options, locations, relocations);
            // The mesh listener is the starting point for adding nodes; no objects are registered.
            options.addRouteMesh(Names.MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id());
            options.addStreamNode(Names.STREAM)
                    .bind(config.streamEndpoint())
                    .registerSession(PlayerSession.class);
        };
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(Program.class, args);
    }
}
