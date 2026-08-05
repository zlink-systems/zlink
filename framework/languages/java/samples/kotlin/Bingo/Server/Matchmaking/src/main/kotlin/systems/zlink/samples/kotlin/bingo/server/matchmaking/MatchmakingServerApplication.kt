package systems.zlink.samples.kotlin.bingo.server.matchmaking

import java.nio.file.Path
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [MatchmakingServerApplication::class],
)
class MatchmakingServerApplication {
    @Bean
    fun framework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.codecs().use(ZLinkProtobufCodec.defaultCodec())
            options.configureLocations()
            options.addLocationStore(SampleLocationStore.create(topology))
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(topology.redisEndpoint)
                        .setKeyPrefix(topology.redisKeyPrefix + "relocation:"),
                ),
            )
            options.addHandlersFromPackageOf(MatchmakingServerApplication::class.java)
            options.addRouteMesh(SampleNames.MatchmakingMesh)
                .setRoutingIdPrefix("matchmaking")
                .listen(topology.matchmakingRouterEndpoint)
                .objects().server()
                .addInstanceSpotFactory(
                    SampleNames.MatchmakerSpotType,
                    BingoMatchmaker::class.java,
                ) { factory -> factory.recreateOnRelocation() }
        }

    @Bean(destroyMethod = "close")
    fun reservations(topology: SampleTopology) =
        RedisBingoMatchReservationStore(topology)

    companion object {
        fun run(args: Array<String>): AutoCloseable {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val configPath = SampleTopology.configPath(args)
            val builder = SpringApplicationBuilder(MatchmakingServerApplication::class.java)
                .environment(environment)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
