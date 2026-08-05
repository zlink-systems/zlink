package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import java.nio.file.Path
import java.net.URI
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [CommerceApiApplication::class],
)
class CommerceApiApplication {
    @Bean
    fun commerceStore(topology: SampleTopology): CommerceStore = CommerceStore(topology)

    @Bean
    fun commerceApiFramework(topology: SampleTopology): ZLinkFrameworkConfigurer {
        val role = topology.role()
        val channelEndpoint = URI.create(role.channelEndpoint)
        return ZLinkFrameworkConfigurer { configurer ->
            configurer.configureLocations()
            configurer.addLocationStore(SampleLocationStore.create(topology))
            configurer.useCoroutineHandlers(Dispatchers.Default)
            configurer.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile("${role.logDirectory}/flow-${role.instanceId}.log")
                traceLabel(role.instanceId)
            }
            configurer.addHandlersFromPackageOf(CommerceApiApplication::class.java)

            configurer.addClientServerChannel(SampleNames.commerceApiChannel(role.instanceId))
                .server()
                .setBindHost(channelEndpoint.host)
                .setAdvertiseHost(channelEndpoint.host)
                .listen(channelEndpoint.port)
                .addHandlerGroup("commerce")

            val peer = if (role.instanceId == SampleNames.ApiInstanceB) SampleNames.ApiInstanceA else SampleNames.ApiInstanceB
            configurer.addClientServerChannel(SampleNames.commerceApiChannel(peer))
                .client()

            configurer.addRouteMesh(SampleNames.OrderWorkflowMesh)
                .setRoutingIdPrefix("shoppingmall-api")
                .listen()
                .objects().client()
        }
    }

    companion object {
        fun run(configPath: String): AutoCloseable {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(CommerceApiApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            builder.application().setKeepAlive(true)
            val context = builder.run()
            context.getBean(CommerceStore::class.java).seedDefaults()
            return AutoCloseable { context.close() }
        }
    }
}
