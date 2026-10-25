package MyGame.table;

import MyGame.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import java.util.Arrays;
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
        if (Arrays.equals(args, new String[] {"--check-domain"})) {
            int passed = DomainChecks.run();
            System.out.println("Domain checks: " + passed + "/7 passed");
            if (passed != 7) System.exit(1);
            DomainChecks.verifyJsonContract();
            System.out.println("JSON contract: passed");
            return;
        }
        Hosting.run(Program.class, args);
    }
}
