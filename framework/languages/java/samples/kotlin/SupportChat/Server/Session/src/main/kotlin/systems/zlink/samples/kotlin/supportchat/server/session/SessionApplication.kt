package systems.zlink.samples.kotlin.supportchat.server.session

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
import systems.zlink.samples.kotlin.supportchat.server.session.sessions.SupportChatSession

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [SessionApplication::class],
)
class SessionApplication {
    @Bean
    fun sessionFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val session = topology.session()
            options.addHandlersFromPackageOf(SessionApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(SampleFlowLog.path(topology, "session"))
                traceLabel("session")
            }
            options.configureLocations()
            options.addClientServerChannel(SampleNames.ApiChannel)
                .client()
            options.addClientServerChannel(SampleNames.SupportChannel)
                .client()
            val node = options.addRouteMesh(SampleNames.SupportSpotDiscovery)
            node.listen(session.routerEndpoint)
                .setRoutingIdPrefix("support-session")
            node.objects().client()
            options.addStreamNode(SampleNames.StreamNode)
                .bind(session.streamEndpoint)
                .enableActorDispatch()
                .registerSession(SupportChatSession::class.java)
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(SessionApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            builder.application().setKeepAlive(true)
            return builder.run()
        }
    }
}
