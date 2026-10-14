package MyGame.field;

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
            if (config.fieldCapacity() <= 0)
                throw new IllegalArgumentException("Field capacity must be positive");
            Hosting.configure(options, locations, relocations);
            options.addHandlersFromPackageOf(Program.class);
            var mesh =
                    options.addRouteMesh(Names.MESH)
                            .listen(config.meshEndpoint())
                            .setRoutingIdPrefix(config.id());
            mesh.objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addSpotFactory(
                            Names.FIELD,
                            Field.class,
                            b ->
                                    b.stableTypeLimit(config.fieldCapacity())
                                            .executionMode(
                                                    systems.zlink.framework.configuration
                                                            .ZLinkUserSpotExecutionMode.SPOT_WIDE)
                                            .preserveStateWith(FieldTransfer.class))
                    .addActorFactory(
                            Names.PLAYER,
                            Player.class,
                            PlayerFactory.class,
                            b -> b.preserveStateWith(PlayerTransfer.class));
            mesh.channelName(Names.BORDER).server();
        };
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(Program.class, args);
    }
}
