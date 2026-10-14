package MyGame;

import java.io.File;
import org.springframework.context.annotation.*;
import systems.zlink.framework.locations.redis.*;

@Configuration(proxyBeanMethods = false)
public class Stores {
  @Bean
  public NodeConfig config() throws Exception {
    return Json.MAPPER.readValue(new File(System.getProperty("MyGame.config")), NodeConfig.class);
  }

  @Bean(destroyMethod = "close")
  public ZLinkRedisLocationStore locations(NodeConfig config) {
    return new ZLinkRedisLocationStore(
        new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisEndpoint())
            .setKeyPrefix(config.keyPrefix() + "locations:"));
  }

  @Bean(destroyMethod = "close")
  public ZLinkRedisRelocationStore relocations(NodeConfig config) {
    return new ZLinkRedisRelocationStore(
        new ZLinkRedisRelocationOptions()
            .setConnectionString(config.redisEndpoint())
            .setKeyPrefix(config.keyPrefix() + "relocation:"));
  }
}
