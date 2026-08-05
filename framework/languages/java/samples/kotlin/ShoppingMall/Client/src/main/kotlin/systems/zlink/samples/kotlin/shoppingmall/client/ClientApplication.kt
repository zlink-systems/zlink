package systems.zlink.samples.kotlin.shoppingmall.client

import java.nio.file.Path
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.shoppingmall.client.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [ClientApplication::class],
)
class ClientApplication {
    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

    @Bean
    fun clientFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.addClientServerChannel(SampleNames.commerceApiChannel(SampleNames.ApiInstanceA))
                .client()
            options.addClientServerChannel(SampleNames.commerceApiChannel(SampleNames.ApiInstanceB))
                .client()
        }

    companion object {
        suspend fun run(configPath: String) {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(ClientApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            val context = builder.run()
            val channels = context.getBean(ZLinkClient::class.java)
            ShoppingMallClientScenario(channels).run()
        }
    }
}
