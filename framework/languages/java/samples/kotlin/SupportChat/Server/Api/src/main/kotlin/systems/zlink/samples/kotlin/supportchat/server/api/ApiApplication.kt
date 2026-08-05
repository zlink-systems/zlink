package systems.zlink.samples.kotlin.supportchat.server.api

import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.nio.file.Path
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.core.env.StandardEnvironment
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleFlowLog
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTopology

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [ApiApplication::class],
)
class ApiApplication {
    @Bean
    fun apiFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val api = topology.api()
            val channelEndpoint = URI.create(api.channelEndpoint)
            options.addHandlersFromPackageOf(ApiApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(SampleFlowLog.path(topology, "api"))
                traceLabel("api")
            }
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(channelEndpoint.host)
                .setAdvertiseHost(channelEndpoint.host)
                .listen(channelEndpoint.port)
                .addHandlerGroup(SampleNames.ApiChannel)
            options.addClientServerChannel(SampleNames.SupportChannel)
                .client()
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

    @Bean(destroyMethod = "close")
    fun apiHttpServer(topology: SampleTopology): AutoCloseable {
        val uri = URI.create(topology.api().httpEndpoint)
        val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0).apply {
            createContext("/health") { exchange ->
                val bytes = """{"status":"ok"}""".toByteArray(StandardCharsets.UTF_8)
                exchange.responseHeaders.add("content-type", "application/json")
                exchange.sendResponseHeaders(200, bytes.size.toLong())
                exchange.responseBody.use { it.write(bytes) }
            }
            start()
        }
        return AutoCloseable { server.stop(0) }
    }

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(ApiApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            builder.application().setKeepAlive(true)
            return builder.run()
        }
    }
}
