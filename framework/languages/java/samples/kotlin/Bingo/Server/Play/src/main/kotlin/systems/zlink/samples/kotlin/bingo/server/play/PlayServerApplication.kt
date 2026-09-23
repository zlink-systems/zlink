package systems.zlink.samples.kotlin.bingo.server.play

import com.fasterxml.jackson.databind.MapperFeature
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.json.JsonMapper
import io.micrometer.core.instrument.MeterRegistry
import java.nio.file.Path
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkSpotRelocationCoordinationMode
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.bingo.server.configuration.BingoMetricsReporter
import systems.zlink.samples.kotlin.bingo.server.configuration.BingoReadinessReporter
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActorFactory
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActorRelocationAdapter
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomRelocationAdapter
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [PlayServerApplication::class],
)
class PlayServerApplication {
    @Bean
    fun playFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.addHandlersFromPackageOf<PlayServerApplication>()
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch { messageFlow(ZLinkMessageFlowLogMode.NORMAL) }
            options.codecs().use(ZLinkProtobufCodec.defaultCodec())
            options.configureLocations()
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(topology.redisEndpoint)
                        .setKeyPrefix(topology.redisKeyPrefix + "relocation:")
                )
            )
            val node = options.addRouteMesh(SampleNames.Mesh)
            node.listen(topology.selectedPlaySpotRouterEndpoint()).setRoutingIdPrefix("play")
            options.addClientServerChannel(SampleNames.ApiChannel).client()
            node.channelName(SampleNames.RoomRewardChannel).server()
            node
                .objects()
                .server()
                .addEntrySpot<BingoEntrySpot>()
                // --8<-- [start:doc-execution-mode]
                // SPOT_WIDE is the default. Naming it here keeps the choice visible:
                // every callback of this room runs through one gate.
                // --8<-- [start:doc-bingo-play-register]
                .addSpotFactory<BingoRoomSpot>(SampleNames.RoomSpotType) {
                    executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                    relocationCoordinationMode(
                        ZLinkSpotRelocationCoordinationMode.APPLICATION_SIGNALED
                    )
                    preserveStateWith<BingoRoomSpot, BingoRoomRelocationAdapter>()
                }
                // --8<-- [end:doc-bingo-play-register]
                // --8<-- [end:doc-execution-mode]
                .addActorFactory<PlayerActor, PlayerActorFactory>(SampleNames.PlayerActorType) {
                    preserveStateWith<PlayerActor, PlayerActorRelocationAdapter>()
                }
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore =
        SampleLocationStore.create(topology)

    @Bean
    fun bingoRoomSettingsInitializer(): BingoRoomSettingsInitializer =
        BingoRoomSettingsInitializer()

    @Bean
    fun bingoJsonMapper(): ObjectMapper =
        JsonMapper.builder()
            .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
            .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
            .findAndAddModules()
            .build()

    @Bean(destroyMethod = "close")
    fun bingoMetricsReporter(registry: MeterRegistry): BingoMetricsReporter =
        BingoMetricsReporter(registry, "play")

    @Bean(destroyMethod = "close")
    fun bingoReadinessReporter(
        topology: SampleTopology,
        meshes: ZLinkRouteMeshRuntime,
    ): BingoReadinessReporter = BingoReadinessReporter.play(topology, meshes)

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val environment =
                StandardEnvironment().apply {
                    propertySources.remove(
                        StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME
                    )
                    propertySources.remove(
                        StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME
                    )
                }
            val builder =
                SpringApplicationBuilder(PlayServerApplication::class.java)
                    .environment(environment)
                    .properties(
                        "spring.config.location=${Path.of(SampleTopology.configPath(args)).toAbsolutePath().toUri()}"
                    )
                    .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
