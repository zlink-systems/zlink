package systems.zlink.e2e.kotlin.spotactortransfer.actor

import com.fasterxml.jackson.databind.ObjectMapper
import java.time.Duration
import java.nio.file.Path
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.spotactortransfer.shared.Contracts
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@EnableConfigurationProperties(ActorNodeOptions::class)
@SpringBootApplication(proxyBeanMethods = false)
class Application {
    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceStore(config: ActorNodeOptions): EvidenceStore {
        return EvidenceStore(
            config.nodeRid,
            "${config.logDirectory}/${config.nodeRid}.evidence.log",
        )
    }

    @Bean
    fun gateStore(): GateStore = GateStore()

    @Bean
    fun domainStateStore(config: ActorNodeOptions): DomainStateStore =
        DomainStateStore(config.logDirectory + "/domain-state")

    @Bean
    fun actorNodeHttpServer(
        json: ObjectMapper,
        evidence: EvidenceStore,
        gates: GateStore,
        spots: systems.zlink.framework.spots.ZLinkSpotManager,
        actors: systems.zlink.framework.actors.ZLinkActorManager,
        actorClient: systems.zlink.framework.actors.ZLinkActorClient,
        lifecycle: systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle,
        config: ActorNodeOptions,
    ): ActorNodeHttpServer {
        val requiredPeerCount = config.meshPeers.split(',')
            .map { it.split('=', limit = 2) }
            .count { it.size == 2 && it[0] != config.nodeRid }
        return ActorNodeHttpServer(
            config.httpEndpoint,
            json,
            evidence,
            gates,
            spots,
            actors,
            actorClient,
            requiredPeerCount,
            lifecycle,
        )
    }

    @Bean
    fun framework(
        evidence: EvidenceStore,
        relocationStore: ZLinkRedisRelocationStore,
        config: ActorNodeOptions,
    ): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        val nodeRid = evidence.nodeRid
        val logDir = config.logDirectory
        options.addRelocationStore(relocationStore)
        options.addHandlersFromPackageOf(Application::class.java)
        options.configureLocations().apply {
            // This lane does not claim ST-F4/F5 until a transport-delay fixture exists.
            setRouteCacheMaxAge(Duration.ZERO)
            setMessageFollowDuration(Duration.ofSeconds(2))
        }
        options.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
            .traceLogFile("$logDir/$nodeRid-flow.log")
            .traceLabel("kotlin-spot-transfer-$nodeRid")
        val node = options.addRouteMesh(Contracts.MESH)
        node.listen(config.meshEndpoint)
            .setRoutingId(RoutingId.from(nodeRid))
        if (config.scenario == "ST-A1") {
            node.setPlacementWeight(if (nodeRid == "actor-a") 100 else 0)
        }
        config.meshPeers.split(',').forEach { peer ->
            val fields = peer.split('=', limit = 2)
            if (fields.size == 2 && fields[0] != nodeRid) {
                node.peerConnections().connect(RoutingId.from(fields[0]), fields[1])
            }
        }
        node.objects().client()
        val objects = node.objects().server()
        objects.addEntrySpot(TransferEntrySpot::class.java)
        registerActor(objects, Contracts.STATEFUL, true)
        registerActor(objects, Contracts.EMPTY_STATE, true)
        registerActor(objects, Contracts.NO_ADAPTER, false)
        registerActor(objects, Contracts.FAIL_OUT, true)
        registerActor(objects, Contracts.FAIL_LEAVE, true)
        registerActor(objects, Contracts.FAIL_IN, true)
        objects.addSpotFactory(
            TransferUserSpot::class.java.name,
            TransferUserSpot::class.java,
        ) { factory -> factory.disableRelocation() }
        options.addStreamNode("spot-transfer-session-$nodeRid")
            .bind(config.streamEndpoint)
            .enableActorDispatch()
            .registerSession(TransferSession::class.java)
    }

    private fun registerActor(
        objects: ZLinkMeshObjectServerBuilder,
        actorType: String,
        adapter: Boolean,
    ) {
        objects.addActorFactory(
            actorType,
            TransferActor::class.java,
            TransferActorFactory::class.java,
        ) { factory ->
            if (adapter) factory.preserveStateWith(TransferActorAdapter::class.java)
            else factory.recreateOnRelocation()
        }
    }

    @Bean
    fun locationStore(config: ActorNodeOptions): ZLinkRedisLocationStore = ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint)
            .setKeyPrefix(config.locationKeyPrefix),
    )

    @Bean
    fun relocationStore(config: ActorNodeOptions): ZLinkRedisRelocationStore = ZLinkRedisRelocationStore(
        ZLinkRedisRelocationOptions()
            .setConnectionString(config.redisLocationEndpoint)
            .setKeyPrefix(config.locationKeyPrefix + "relocation:"),
    )
}

fun main(vararg args: String) {
    require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
        "Usage: spot-actor-transfer-kotlin-actor-node --config <path>"
    }
    val environment = StandardEnvironment().also {
        it.propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
        it.propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
    }
    val builder = SpringApplicationBuilder(Application::class.java)
        .environment(environment)
        .properties("spring.config.location=${Path.of(args[1]).toAbsolutePath().toUri()}")
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    builder.run()
}
