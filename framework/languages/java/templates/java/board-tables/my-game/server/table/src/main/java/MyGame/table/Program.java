package MyGame.table;

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
            options.addHandlersFromPackageOf(Program.class);
            options.addRouteMesh(Names.MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.id())
                    .objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addSpotFactory(
                            Names.TABLE,
                            Table.class,
                            b -> {
                                if (config.tableCapacity() > 0)
                                    b.stableTypeLimit(config.tableCapacity());
                                b.preserveStateWith(TableTransfer.class);
                            })
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
