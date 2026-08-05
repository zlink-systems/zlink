package systems.zlink.samples.kotlin.supportchat.server.support

import java.nio.file.Path
import java.net.URI
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
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleFlowLog
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAssignmentService
import systems.zlink.samples.kotlin.supportchat.server.support.application.AgentAvailabilityDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.application.ConversationStarter
import systems.zlink.samples.kotlin.supportchat.server.support.application.SupportConversationAllocator
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.FrameworkConversationStarter
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportActorDirectory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActorFactory
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActorRelocationAdapter
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.ConversationSpot
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.notifications.ConversationNotificationPublisher
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.SupportEntrySpot

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [SupportApplication::class],
)
class SupportApplication {
    @Bean
    fun supportFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val support = topology.support()
            val channelEndpoint = URI.create(support.channelEndpoint)
            options.addHandlersFromPackageOf(SupportApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureLocations()
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(topology.location().redisEndpoint)
                        .setKeyPrefix(topology.location().redisKeyPrefix + "relocation:"),
                ),
            )
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(SampleFlowLog.path(topology, "support"))
                traceLabel("support")
            }
            options.addClientServerChannel(SampleNames.SupportChannel)
                .server()
                .setBindHost(channelEndpoint.host)
                .setAdvertiseHost(channelEndpoint.host)
                .listen(channelEndpoint.port)
                .addHandlerGroup(SampleNames.SupportChannel)
            options.addClientServerChannel(SampleNames.ApiChannel)
                .client()
            val node = options.addRouteMesh(SampleNames.SupportSpotDiscovery)
            node.listen(support.routerEndpoint)
                .setRoutingIdPrefix("support-owner")
            node.objects().server()
                .addEntrySpot(SupportEntrySpot::class.java)
                .addActorFactory(
                    SampleNames.SupportActorType,
                    SupportUserActor::class.java,
                    SupportUserActorFactory::class.java,
                ) { factory ->
                    factory.preserveStateWith(SupportUserActorRelocationAdapter::class.java)
                }
                .addSpotFactory(
                    SampleNames.ConversationSpotType,
                    ConversationSpot::class.java,
                ) { factory -> factory.disableRelocation() }
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

    @Bean
    fun supportActorDirectory(): SupportActorDirectory = SupportActorDirectory()

    @Bean
    fun agentAvailabilityDirectory(): AgentAvailabilityDirectory =
        AgentAvailabilityDirectory(SampleNames.AgentCapacity)

    @Bean
    fun agentAssignmentService(availability: AgentAvailabilityDirectory): AgentAssignmentService =
        AgentAssignmentService(availability)

    @Bean
    fun conversationStarter(spots: ZLinkSpotManager): ConversationStarter =
        FrameworkConversationStarter(spots)

    @Bean
    fun supportConversationAllocator(conversations: ConversationStarter): SupportConversationAllocator =
        SupportConversationAllocator(conversations)

    @Bean
    fun conversationNotificationPublisher(): ConversationNotificationPublisher =
        ConversationNotificationPublisher()

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(SupportApplication::class.java)
                .environment(environment)
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
            builder.application().setKeepAlive(true)
            return builder.run()
        }
    }
}
