package systems.zlink.e2e.kotlin.spotservice.play

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.endpoints.EvidenceHttpServer
import systems.zlink.e2e.kotlin.spotservice.play.handlers.*
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import com.fasterxml.jackson.databind.ObjectMapper
import java.util.concurrent.CompletableFuture
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.configuration.ClientServerChannelBuilder
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.spots.ZLinkSpotManager

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.spotservice.play"]
)
class PlayApplication {
    @Bean
    fun scenarioState(): ScenarioState = ScenarioState(Env.get("e2e.node.rid", "play-a"))

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceHttpServer(
        state: ScenarioState,
        json: ObjectMapper,
        spots: ZLinkSpotManager,
        routes: ZLinkRouteClient,
        meshOptions: ZLinkRouteMeshRuntimeOptions,
    ): EvidenceHttpServer =
        EvidenceHttpServer(
            state,
            json,
            Env.get("e2e.http.endpoint"),
            spots,
            routes,
            meshOptions,
        )

    @Bean
    fun playFramework(
        state: ScenarioState,
        relocationStore: ZLinkRedisRelocationStore,
    ): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val nodeRid = state.nodeRid()
            val logDir = Env.get("e2e.log.dir", "logs")
            options.addRelocationStore(relocationStore)
            options.addHandlersFromPackageOf(IngressCommandHandler::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("$logDir/$nodeRid-flow.log")
                .traceLabel("kotlin-sm-$nodeRid")
                .setMessageFlowObserver { error ->
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return@setMessageFlowObserver CompletableFuture.completedFuture(null)
                    }
                    state.record(
                        "DispatchError",
                        error.spotId() ?: "",
                        error.surface().toString() +
                            "|" + error.errorReason() +
                            "/" + error.errorAction() +
                            "/" + error.packetName()
                    )
                    CompletableFuture.completedFuture(null)
                }
            val node: ZLinkMeshNodeBuilder = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(Env.get("e2e.route.endpoint"))
                .setRoutingId(RoutingId.from(nodeRid))
            node.channelName(Contracts.ROUTE_CHANNEL).server()
            if (nodeRid != "play-a") {
                node.peerConnections().connect(
                    RoutingId.from("play-a"),
                    Env.get("e2e.route.a.endpoint"),
                )
            }
            if (nodeRid != "play-b") {
                node.peerConnections().connect(
                    RoutingId.from("play-b"),
                    Env.get("e2e.route.b.endpoint"),
                )
            }
            node.addRouteRequestHandler(
                RoutePingHandler::class.java,
                Contracts.RoutePingReq::class.java,
                Contracts.RoutePingRes::class.java
            )
            node.addRouteRequestHandler(
                EnsureActorHandler::class.java,
                Contracts.EnsureActorReq::class.java,
                Contracts.EnsureActorRes::class.java
            )
            val peerIngress = if (nodeRid == "play-a") {
                Env.get("e2e.ingress.b.endpoint")
            } else {
                Env.get("e2e.ingress.a.endpoint")
            }
            val ingress: ClientServerChannelBuilder = options.addClientServerChannel(Contracts.INGRESS_CHANNEL)
            ingress.server()
                .listen(Env.get("e2e.ingress.endpoint").substringAfterLast(':').toInt())
                .addHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
            ingress.client().connect(peerIngress)
            node.objects().server()
                .addEntrySpot(ScenarioEntrySpot::class.java)
                .addSpotFactory(
                    "user",
                    UserSpot::class.java,
                ) { factory -> factory.disableRelocation() }
                .addSpotFactory(
                    "mismatched",
                    MismatchedSpot::class.java,
                ) { factory -> factory.disableRelocation() }
                .addSpotFactory(
                    "timer",
                    TimerScenarioSpot::class.java,
                ) { factory -> factory.disableRelocation() }
                .addActorFactory(
                    "scenario",
                    ScenarioActor::class.java,
                    ScenarioActorFactory::class.java,
                ) { factory -> factory.recreateOnRelocation() }
            val streamEndpoint = Env.get("e2e.stream.endpoint")
            val tlsStreamEndpoint = Env.get("e2e.tls.stream.endpoint", "")
            if (streamEndpoint.isNotBlank() || tlsStreamEndpoint.isNotBlank()) {
                val stream = options.addStreamNode("gateway")
                if (streamEndpoint.isNotBlank()) {
                    stream.bind(streamEndpoint)
                }
                if (tlsStreamEndpoint.isNotBlank()) {
                    stream.bind(tlsStreamEndpoint)
                        .setTlsServer(
                            Env.get("e2e.tls.certificate.path"),
                            Env.get("e2e.tls.key.path")
                        )
                }
                stream.enableActorDispatch()
                stream.registerSession(ScenarioSession::class.java)
                    .addSessionPacketHandler(ActorAuthHandler::class.java)
            }
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix"))
        )

    @Bean
    fun relocationStore(): ZLinkRedisRelocationStore =
        ZLinkRedisRelocationStore(
            ZLinkRedisRelocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix"))
        )

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val builder = SpringApplicationBuilder(PlayApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }
    }
}
