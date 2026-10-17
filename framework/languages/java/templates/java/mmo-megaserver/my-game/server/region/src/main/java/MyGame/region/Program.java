package MyGame.region;

import MyGame.*;
import MyGame.region.infrastructure.zlink.*;

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
            if (config.capacity() <= 0 || config.regionLimit() <= 0)
                throw new IllegalArgumentException("Capacity and Region limit must be positive");
            Hosting.configure(options, locations, relocations);
            options.addHandlersFromPackageOf(Program.class);
            options.addRouteMesh(Names.MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id())
                    .objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addSpotFactory(
                            Names.REGION,
                            Region.class,
                            b ->
                                    b.stableTypeLimit(config.regionLimit())
                                            .executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                                            .preserveStateWith(RegionTransfer.class))
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
