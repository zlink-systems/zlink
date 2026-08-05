package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

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
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
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
    scanBasePackageClasses = [OrderWorkflowApplication::class],
)
class OrderWorkflowApplication {
    @Bean
    fun commerceStore(topology: SampleTopology): CommerceStore = CommerceStore(topology)

    @Bean
    fun orderWorkflowFramework(topology: SampleTopology): ZLinkFrameworkConfigurer {
        val role = topology.role()
        val channelEndpoint = URI.create(role.channelEndpoint)
        return ZLinkFrameworkConfigurer { configurer ->
            configurer.configureLocations()
            configurer.addLocationStore(SampleLocationStore.create(topology))
            val location = topology.location()
            configurer.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(location.redisEndpoint)
                        .setKeyPrefix("${location.redisKeyPrefix}relocation:"),
                ),
            )
            configurer.useCoroutineHandlers(Dispatchers.Default)
            configurer.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile("${role.logDirectory}/flow-${role.instanceId}.log")
                traceLabel(role.instanceId)
            }
            configurer.addHandlersFromPackageOf(OrderWorkflowApplication::class.java)
            configurer.addRouteMesh(SampleNames.OrderWorkflowMesh)
                .setRoutingIdPrefix("shoppingmall-workflow")
                .listen(role.channelEndpoint)
                .objects().server()
                .addInstanceSpotFactory(
                    SampleNames.OrderWorkflowSpotType,
                    OrderWorkflowSpot::class.java,
                ) { factory -> factory.recreateOnRelocation() }
        }
    }

    companion object {
        fun run(configPath: String): AutoCloseable {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(OrderWorkflowApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
