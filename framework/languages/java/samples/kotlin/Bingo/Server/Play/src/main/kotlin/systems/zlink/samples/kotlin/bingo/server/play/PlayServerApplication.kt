package systems.zlink.samples.kotlin.bingo.server.play

import com.fasterxml.jackson.databind.MapperFeature
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.json.JsonMapper
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import java.nio.file.Path
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActorFactory
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActorRelocationAdapter
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomRelocationAdapter
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.bingo.server.configuration.BingoMetricsReporter
import io.micrometer.core.instrument.MeterRegistry



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
            options.addHandlersFromPackageOf(PlayServerApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(topology.logDirectory + "/flow-play.log")
                traceLabel("play")
            }
            options.codecs().use(ZLinkProtobufCodec.defaultCodec())
            options.configureLocations()
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(topology.redisEndpoint)
                        .setKeyPrefix(topology.redisKeyPrefix + "relocation:"),
                ),
            )
            val node = options.addRouteMesh(SampleNames.Mesh)
            node.listen(topology.selectedPlaySpotRouterEndpoint())
                .setRoutingIdPrefix("play")
            options.addClientServerChannel(SampleNames.ApiChannel).client()
            node.channelName(SampleNames.RoomRewardChannel).server()
            node.objects().server()
                .addEntrySpot(BingoEntrySpot::class.java)
                .addSpotFactory(
                    SampleNames.RoomSpotType,
                    BingoRoomSpot::class.java,
                ) { factory ->
                    factory.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                    factory.relocationReadiness(
                        ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED,
                    )
                    factory.preserveStateWith(BingoRoomRelocationAdapter::class.java)
                }
                .addActorFactory(
                    SampleNames.PlayerActorType,
                    PlayerActor::class.java,
                    PlayerActorFactory::class.java,
                ) { factory ->
                    factory.preserveStateWith(PlayerActorRelocationAdapter::class.java)
                }
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

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

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(PlayServerApplication::class.java)
                .environment(environment)
                .properties("spring.config.location=${Path.of(SampleTopology.configPath(args)).toAbsolutePath().toUri()}")
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
