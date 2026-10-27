package MyGame.channel;

import MyGame.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
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
            if (config.zoneCapacity() < 1)
                throw new IllegalArgumentException("Zone capacity must be positive");
            Hosting.configure(options, locations, relocations);
            options.addHandlersFromPackageOf(Program.class);
            options.addRouteMesh(Names.mesh(config.worldId()))
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id())
                    .objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addSpotFactory(
                            Names.ZONE,
                            Zone.class,
                            b ->
                                    b.stableTypeLimit(config.zoneCapacity())
                                            .executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                                            .preserveStateWith(ZoneTransfer.class))
                    .addActorFactory(
                            Names.PLAYER,
                            Player.class,
                            PlayerFactory.class,
                            b -> b.preserveStateWith(PlayerTransfer.class));
        };
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(Program.class, args);
    }
}
