package MyGame.lobby;

import MyGame.*;
import MyGame.lobby.application.*;
import MyGame.lobby.infrastructure.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.ZLinkSpotManager;
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
            options.addRouteMesh(Names.MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id())
                    .objects()
                    .client();
        };
    }

    @Bean(destroyMethod = "close")
    public HttpLobby http(NodeConfig config, ZLinkSpotManager spots, ZLinkRouteClient routes) {
        if (config.sessionEndpoint() == null || config.sessionEndpoint().isBlank())
            throw new IllegalArgumentException("Session endpoint is required");
        try {
            return new HttpLobby(config, new Matchmaker(new ZLinkRooms(spots, routes, config)));
        } catch (java.io.IOException error) {
            throw new java.io.UncheckedIOException(error);
        }
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(Program.class, args);
    }
}
