package MyGame.matchmaker;

import MyGame.*;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.spots.*;
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
                    .addSpotFactory(
                            Names.POOL,
                            Pool.class,
                            builder ->
                                    builder.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                                            .disableRelocation());
        };
    }

    public static void main(String[] args) throws Exception {
        Hosting.run(
                Program.class,
                args,
                app -> {
                    var pool =
                            app.getBean(ZLinkSpotManager.class)
                                    .getOrCreate(Names.POOL_KEY, Names.POOL)
                                    .inMesh(Names.MESH)
                                    .submit()
                                    .toCompletableFuture()
                                    .join();
                    if (pool.state() == ZLinkSpotCreateState.REJECTED)
                        throw new IllegalStateException("Pool creation rejected");
                });
    }
}
