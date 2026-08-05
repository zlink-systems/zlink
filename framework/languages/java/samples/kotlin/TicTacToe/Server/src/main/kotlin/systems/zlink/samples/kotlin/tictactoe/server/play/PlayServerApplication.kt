package systems.zlink.samples.kotlin.tictactoe.server.play

import java.nio.file.Path
import com.fasterxml.jackson.databind.MapperFeature
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.json.JsonMapper
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.core.env.StandardEnvironment
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.TicTacToeGameCreatedHandler



@EnableZLinkFramework
@EnableConfigurationProperties(SampleSettings::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [PlayServer::class],
)
class PlayServerApplication {
    @Bean
    fun playFramework(settings: SampleSettings): ZLinkFrameworkConfigurer {
        val server = PlayServer.configure(settings)
        return ZLinkFrameworkConfigurer { options ->
            options.configureLocations()
            options.addLocationStore(SampleLocationStore.create(settings))
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(settings.redisEndpoint)
                        .setKeyPrefix(settings.redisKeyPrefix + "relocation:"),
                ),
            )
            server.configure(options)
        }
    }

    @Bean
    fun ticTacToeGameCreatedHandler(): TicTacToeGameCreatedHandler =
        TicTacToeGameCreatedHandler()

    @Bean(destroyMethod = "close")
    fun locationStore(settings: SampleSettings): ZLinkRedisLocationStore =
        SampleLocationStore.create(settings)

    @Bean
    fun ticTacToeJsonMapper(): ObjectMapper =
        JsonMapper.builder()
            .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
            .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
            .findAndAddModules()
            .build()

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            return SpringApplicationBuilder(PlayServerApplication::class.java).also { builder ->
                builder.application().setKeepAlive(true)
            }
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .run()
        }
    }
}
