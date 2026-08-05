package systems.zlink.samples.bingo.server.play;

import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActorFactory;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActorRelocationAdapter;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomRelocationAdapter;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot;
import systems.zlink.samples.bingo.server.configuration.SampleLocationStore;
import systems.zlink.samples.bingo.server.configuration.SampleApplication;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTopology;
import systems.zlink.samples.bingo.server.configuration.BingoMetricsReporter;
import io.micrometer.core.instrument.MeterRegistry;



@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = PlayServerApplication.class)
public final class PlayServerApplication {
    private PlayServerApplication() {
    }

    public static AutoCloseable run(String configPath) {
        return SampleApplication.start(PlayServerApplication.class, configPath)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer playFramework(SampleTopology topology) {
        return options -> {
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(topology.logDirectory() + "/flow-play.log")
                .traceLabel("play");
            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
            options.configureLocations();
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString(topology.redisEndpoint())
                    .setKeyPrefix(topology.redisKeyPrefix() + "relocation:")));
            options.addHandlersFromPackageOf(PlayServerApplication.class);
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.Mesh);
            node.listen(topology.selectedPlaySpotRouterEndpoint())
                .setRoutingIdPrefix("play");
            options.addClientServerChannel(SampleNames.ApiChannel).client();
            node.channelName(SampleNames.RoomRewardChannel).server();
            node.objects()
                .server()
                .addEntrySpot(BingoEntrySpot.class)
                .addSpotFactory(
                    SampleNames.RoomSpotType,
                    BingoRoomSpot.class,
                    factory -> {
                        factory.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE);
                        factory.relocationReadiness(
                            ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED);
                        factory.preserveStateWith(BingoRoomRelocationAdapter.class);
                    })
                .addActorFactory(
                    SampleNames.PlayerActorType,
                    PlayerActor.class,
                    PlayerActorFactory.class,
                    factory -> factory.preserveStateWith(
                        PlayerActorRelocationAdapter.class));
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    BingoRoomSettingsInitializer bingoRoomSettingsInitializer() {
        return new BingoRoomSettingsInitializer();
    }

    @Bean
    ObjectMapper bingoJsonMapper() {
        return JsonMapper.builder()
            .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
            .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
            .findAndAddModules()
            .build();
    }

    @Bean(destroyMethod = "close")
    BingoMetricsReporter bingoMetricsReporter(MeterRegistry registry) {
        return new BingoMetricsReporter(registry, "play");
    }
}
