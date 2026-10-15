package MyGame.town;

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
      NodeConfig config, ZLinkRedisLocationStore locations, ZLinkRedisRelocationStore relocations) {
    return options -> {
      if (config.townCapacity() <= 0)
        throw new IllegalArgumentException("Town capacity must be positive");
      Hosting.configure(options, locations, relocations);
      options.addHandlersFromPackageOf(Program.class);
      options
          .addRouteMesh(Names.MESH)
          .listen(config.meshEndpoint())
          .setRoutingIdPrefix(config.id())
          .objects()
          .server()
          .addActorFactory(
              Names.PLAYER,
              Player.class,
              PlayerFactory.class,
              b -> b.preserveStateWith(PlayerTransfer.class))
          .addEntrySpot(Entry.class)
          .addSpotFactory(
              Names.TOWN,
              Town.class,
              b ->
                  b.stableTypeLimit(config.townCapacity())
                      .executionMode(
                          systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
                              .SPOT_WIDE)
                      .preserveStateWith(TownTransfer.class));
    };
  }

  public static void main(String[] args) throws Exception {
    Hosting.run(Program.class, args);
  }
}
