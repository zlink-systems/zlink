package MyGame.instance;

import MyGame.*;
import MyGame.admission.EnterWorld;
import MyGame.admission.Entry;

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
            if (config.dungeonCapacity() <= 0)
                throw new IllegalArgumentException("Dungeon capacity must be positive");
            Hosting.configure(options, locations, relocations);
            options.addHandlersFromPackageOf(Program.class);
            options.addHandlersFromPackageOf(EnterWorld.class);
            options.addRouteMesh(Names.MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id())
                    .objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addSpotFactory(
                            Names.DUNGEON,
                            Dungeon.class,
                            b ->
                                    b.stableTypeLimit(config.dungeonCapacity())
                                            .executionMode(
                                                    systems.zlink.framework.configuration
                                                            .ZLinkUserSpotExecutionMode.SPOT_WIDE)
                                            .preserveStateWith(DungeonTransfer.class))
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
