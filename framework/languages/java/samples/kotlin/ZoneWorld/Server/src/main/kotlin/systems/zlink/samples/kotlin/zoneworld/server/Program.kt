package systems.zlink.samples.kotlin.zoneworld.server

import java.nio.file.Path
import java.time.Duration
import org.springframework.beans.factory.ObjectProvider
import org.springframework.boot.ApplicationRunner
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.zoneworld.server.configuration.MaintenanceStore
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeRegistry
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.zoneworld.server.gateway.GameSession
import systems.zlink.samples.kotlin.zoneworld.server.ops.NodeLivenessObserver
import systems.zlink.samples.kotlin.zoneworld.server.ops.OpsSession
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneBootstrap
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneStatusReporter
import systems.zlink.samples.kotlin.zoneworld.server.zone.PlayerActor
import systems.zlink.samples.kotlin.zoneworld.server.zone.PlayerActorFactory
import systems.zlink.samples.kotlin.zoneworld.server.zone.PlayerActorRelocationAdapter
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneEntrySpot
import systems.zlink.samples.kotlin.zoneworld.server.zone.ZoneSpot
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldNames
@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(proxyBeanMethods = false, scanBasePackages = ["systems.zlink.samples.kotlin.zoneworld.server"])
class Program {
    @Bean fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions().setConnectionString(topology.redisValue())
                .setKeyPrefix("${topology.prefixValue()}locations:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )

    @Bean(destroyMethod = "close") fun relocationStore(topology: SampleTopology) =
        ZLinkRedisRelocationStore(
            ZLinkRedisRelocationOptions().setConnectionString(topology.redisValue())
                .setKeyPrefix("${topology.prefixValue()}relocation:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )

    @Bean(destroyMethod = "close") fun maintenanceStore(topology: SampleTopology) = MaintenanceStore(topology)
    @Bean fun maintenanceState() = NodeMaintenanceState()
    @Bean fun nodeCensus() = NodeCensus()
    @Bean fun nodeRegistry() = NodeRegistry()

    @Bean fun framework(
        topology: SampleTopology,
        locations: ZLinkRedisLocationStore,
        relocation: ZLinkRedisRelocationStore,
    ): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        topology.validate()
        options.configureLocations()
        options.addLocationStore(locations)
        options.addRelocationStore(relocation)
        options.addHandlersFromPackageOf(Program::class.java)
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
        val mesh: ZLinkMeshNodeBuilder = options.addRouteMesh(ZoneWorldNames.MESH)
            .listen(topology.meshValue())
            // A zone node names its application identity in the routing id prefix; the
            // framework appends a per-process UUID, so a replacement started for the same
            // node id still publishes a routing id no earlier process ever held.
            .setRoutingIdPrefix(
                ZoneWorldNames.routingIdPrefix(
                    if (topology.isRole("zone")) topology.nodeValue() else topology.roleValue(),
                ),
            )
        when (topology.roleValue()) {
            "gateway" -> {
                mesh.objects().client()
                options.addStreamNode(ZoneWorldNames.GATEWAY_STREAM).bind(topology.streamValue())
                    .enableActorDispatch().registerSession(GameSession::class.java)
            }
            "zone" -> {
                mesh.channelName(ZoneWorldNames.ZONE_CHANNEL).server()
                    .addHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
                mesh.channelName(ZoneWorldNames.REPORT_CHANNEL).client()
                mesh.objects().server()
                    .addEntrySpot(ZoneEntrySpot::class.java)
                    .addSpotFactory(ZoneWorldNames.ZONE_SPOT_TYPE, ZoneSpot::class.java) {
                        it.stableTypeLimit(2).disableRelocation()
                    }
                    .addActorFactory(
                        ZoneWorldNames.PLAYER_ACTOR_TYPE,
                        PlayerActor::class.java,
                        PlayerActorFactory::class.java,
                    ) { it.preserveStateWith(PlayerActorRelocationAdapter::class.java) }
                options.addFanoutChannel(ZoneWorldNames.BROADCAST_CHANNEL).enableSubscriber()
                    .addHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP)
            }
            "ops" -> {
                mesh.channelName(ZoneWorldNames.REPORT_CHANNEL).server()
                    .addHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP)
                mesh.objects().client()
                options.addFanoutChannel(ZoneWorldNames.BROADCAST_CHANNEL)
                    .setRoutingIdPrefix("zoneworld-kotlin-ops-broadcast")
                    .enablePublisher()
                options.addStreamNode(ZoneWorldNames.OPS_STREAM).bind(topology.streamValue())
                    .enableActorDispatch().registerSession(OpsSession::class.java)
            }
        }
    }

    @Bean
    fun runtimeEvidence(provider: ObjectProvider<ZLinkFrameworkRuntime>, topology: SampleTopology) =
        ApplicationRunner {
            val runtime = provider.ifAvailable
            println("framework lifecycle role=${topology.roleValue()} state=${runtime?.status()?.state()}")
            println("runtime event mesh=${ZoneWorldNames.MESH} state=SERVING")
        }

    @Bean
    @ConditionalOnProperty(prefix = "sample", name = ["role"], havingValue = "ops")
    fun nodeLivenessObserver(runtime: ZLinkRouteMeshRuntime, registry: NodeRegistry) =
        NodeLivenessObserver(runtime, registry)

    @Bean
    @ConditionalOnProperty(prefix = "sample", name = ["role"], havingValue = "zone")
    fun zoneBootstrap(
        topology: SampleTopology,
        spots: ZLinkSpotManager,
        actors: ZLinkActorManager,
        maintenance: NodeMaintenanceState,
        store: MaintenanceStore,
    ) = ZoneBootstrap(topology, spots, actors, maintenance, store)

    @Bean
    @ConditionalOnProperty(prefix = "sample", name = ["role"], havingValue = "zone")
    fun zoneReporter(
        topology: SampleTopology,
        routes: ZLinkRouteClient,
        census: NodeCensus,
        maintenance: NodeMaintenanceState,
    ) = ZoneStatusReporter(topology, routes, census, maintenance)

    companion object {
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val builder = SpringApplicationBuilder(Program::class.java)
                .environment(environment)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            return builder.run()
        }
    }
}

fun main(args: Array<String>) {
    Program.run(SampleTopology.configPath(args))
    Thread.currentThread().join()
}
