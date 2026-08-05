package systems.zlink.e2e.kotlin.runtimemonitoring.service

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.ApplicationRunner
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.spots.ZLinkSpotManager
import java.time.Duration

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class ServiceApplication {
    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceState(): EvidenceState = EvidenceState()

    @Bean
    fun evidenceHttpServer(
        state: EvidenceState,
        json: ObjectMapper,
        runtimeOptions: ZLinkChannelRuntimeOptions,
    ): EvidenceHttpServer {
        return EvidenceHttpServer(
            state,
            json,
            Env.get("e2e.http.endpoint"),
            runtimeOptions,
        )
    }

    @Bean
    fun frameworkConfigurer(): ZLinkFrameworkConfigurer {
        return ZLinkFrameworkConfigurer { options ->
            val logDir = Env.get("e2e.log.dir", "logs")
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500))
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(3))
            options.configureLocations().setPollingInterval(Duration.ofMillis(250))
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("$logDir/service-flow.log")
                .traceLabel("kotlin-mon-service")
            options.addHandlersFromPackageOf(WorkRequestHandler::class.java)
            val apiEndpoint = java.net.URI.create(
                Env.get("e2e.api.endpoint"),
            )
            options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setAdvertiseHost(apiEndpoint.host)
                .listen(apiEndpoint.port)
                .addHandlerGroup(Contracts.HANDLER_GROUP)
            val handshakeEndpoint = java.net.URI.create(
                Env.get("e2e.handshake.endpoint"),
            )
            options.addClientServerChannel(Contracts.HANDSHAKE_CHANNEL)
                .server()
                .setAdvertiseHost(handshakeEndpoint.host)
                .listen(handshakeEndpoint.port)
                .addHandlerGroup(Contracts.HANDLER_GROUP)
            val node = options.addRouteMesh(Contracts.SPOT_MESH)
            node.listen(Env.get("e2e.mesh.endpoint"))
                .setRoutingId(RoutingId.from("svc-a-spot"))
            node.channelName(Contracts.SPOT_CHANNEL)
            node.objects().server()
                .addSpotFactory(
                    "monitoring",
                    MonitoringSpot::class.java,
                ) { factory -> factory.disableRelocation() }
        }
    }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore {
        return ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix"))
                .setCommandTimeout(Duration.ofMillis(500)),
        )
    }

    @Bean
    fun workRequestHandler(): WorkRequestHandler = WorkRequestHandler()

    @Bean
    fun createSpot(spots: ZLinkSpotManager): ApplicationRunner {
        return ApplicationRunner {
            spots.getOrCreate(
                "monitoring-room",
                "monitoring",
            ).request(ZLinkMessage.empty())
                .submit()
                .toCompletableFuture()
                .join()
        }
    }

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val builder = SpringApplicationBuilder(ServiceApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }
    }
}
