package systems.zlink.samples.bingo.server.matchmaking;

import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.bingo.server.configuration.SampleApplication;
import systems.zlink.samples.bingo.server.configuration.SampleLocationStore;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTopology;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = MatchmakingServerApplication.class)
public final class MatchmakingServerApplication {
    private MatchmakingServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(
            MatchmakingServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer matchmakingFramework(SampleTopology topology) {
        return options -> {
            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
            options.configureLocations();
            options.addLocationStore(SampleLocationStore.create(topology));
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString(topology.redisEndpoint())
                    .setKeyPrefix(topology.redisKeyPrefix() + "relocation:")));
            options.addHandlersFromPackageOf(MatchmakingServerApplication.class);
            ZLinkMeshNodeBuilder node = options
                .addRouteMesh(SampleNames.MatchmakingMesh)
                .setRoutingIdPrefix("matchmaking")
                .listen(topology.matchmakingRouterEndpoint());
            node.objects().server().addInstanceSpotFactory(
                SampleNames.MatchmakerSpotType,
                BingoMatchmaker.class,
                factory -> factory.recreateOnRelocation());
        };
    }

    @Bean(destroyMethod = "close")
    RedisBingoMatchReservationStore reservationStore(SampleTopology topology) {
        return new RedisBingoMatchReservationStore(topology);
    }
}
