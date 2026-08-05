package systems.zlink.e2e.kotlin.runtimemonitoring.throwingservice

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.e2e.kotlin.runtimemonitoring.service.EvidenceHttpServer
import systems.zlink.e2e.kotlin.runtimemonitoring.service.EvidenceState
import systems.zlink.e2e.kotlin.runtimemonitoring.service.WorkRequestHandler
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import java.time.Duration

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.runtimemonitoring.throwingservice"],
)
class ThrowingServiceApplication {
    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceState(): EvidenceState = EvidenceState()

    @Bean
    fun evidenceHttpServer(state: EvidenceState, json: ObjectMapper): EvidenceHttpServer {
        return EvidenceHttpServer(state, json, Env.get("e2e.http.endpoint"))
    }

    @Bean
    fun frameworkConfigurer(): ZLinkFrameworkConfigurer {
        return ZLinkFrameworkConfigurer { options ->
            val logDir = Env.get("e2e.log.dir", "logs")
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("$logDir/throwing-service-flow.log")
                .traceLabel("kotlin-mon-throwing-service")
            options.addHandlersFromPackageOf(WorkRequestHandler::class.java)
            val apiEndpoint = java.net.URI.create(
                Env.get("e2e.api.endpoint"),
            )
            options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setAdvertiseHost(apiEndpoint.host)
                .listen(apiEndpoint.port)
                .addHandlerGroup(Contracts.HANDLER_GROUP)
        }
    }

    @Bean
    fun workRequestHandler(): WorkRequestHandler = WorkRequestHandler()

    @Bean
    fun locationStore(): ZLinkRedisLocationStore {
        return ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix"))
                .setCommandTimeout(Duration.ofMillis(500)),
        )
    }

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val builder = SpringApplicationBuilder(ThrowingServiceApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }
    }
}
