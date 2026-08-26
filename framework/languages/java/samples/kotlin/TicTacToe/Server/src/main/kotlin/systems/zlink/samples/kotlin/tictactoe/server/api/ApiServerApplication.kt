package systems.zlink.samples.kotlin.tictactoe.server.api


import java.net.InetAddress
import java.net.URI
import java.nio.file.Path
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.boot.web.server.WebServerFactoryCustomizer
import org.springframework.boot.web.servlet.server.ConfigurableServletWebServerFactory
import org.springframework.core.env.StandardEnvironment
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import org.springframework.context.ApplicationListener
import org.springframework.boot.web.context.WebServerInitializedEvent
import org.slf4j.LoggerFactory
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.tictactoe.server.api.handlers.AuthenticatePlayerHandler
import systems.zlink.samples.kotlin.tictactoe.server.api.handlers.CreateGameHttpHandler
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.configuration.TicTacToeReadinessReporter



@EnableZLinkFramework
@EnableConfigurationProperties(SampleSettings::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [
        ApiServer::class,
        AuthenticatePlayerHandler::class,
        CreateGameHttpHandler::class,
    ],
)
class ApiServerApplication {
    @Bean
    fun apiFramework(settings: SampleSettings): ZLinkFrameworkConfigurer =
        ApiServer.configure(settings)

    @Bean(destroyMethod = "close")
    fun locationStore(settings: SampleSettings): ZLinkRedisLocationStore =
        SampleLocationStore.create(settings)

    @Bean(destroyMethod = "close")
    fun ticTacToeReadinessReporter(
        settings: SampleSettings,
        meshes: ZLinkRouteMeshRuntime,
    ): TicTacToeReadinessReporter = TicTacToeReadinessReporter.api(settings.nodeId, meshes)

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            return SpringApplicationBuilder(ApiServerApplication::class.java).also { builder ->
                builder.application().setKeepAlive(true)
            }
                .environment(environment)
                .web(WebApplicationType.SERVLET)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .run()
        }
    }

    @Bean
    fun apiHttpServer(settings: SampleSettings): WebServerFactoryCustomizer<ConfigurableServletWebServerFactory> =
        WebServerFactoryCustomizer { server ->
            val endpoint = URI.create(settings.apiBindUrl)
            server.setAddress(InetAddress.getLoopbackAddress())
            server.setPort(endpoint.port)
        }

    @Bean
    fun ticTacToeHttpReadiness(settings: SampleSettings): ApplicationListener<WebServerInitializedEvent> =
        ApplicationListener {
            LoggerFactory.getLogger(ApiServerApplication::class.java)
                .info("tictactoe-ready kind=http node={}", settings.nodeId)
        }
}
