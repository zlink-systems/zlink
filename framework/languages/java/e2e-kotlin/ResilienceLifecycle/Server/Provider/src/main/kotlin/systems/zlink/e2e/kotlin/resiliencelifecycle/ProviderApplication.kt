package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.ObjectMapper
import java.time.Duration
import java.net.URI
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.resiliencelifecycle.handlers.WorkCommandHandler
import systems.zlink.e2e.kotlin.resiliencelifecycle.handlers.WorkRequestHandler
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.resiliencelifecycle.handlers"],
)
open class ProviderApplication {
    @Bean
    open fun scenarioState(): ScenarioState =
        ScenarioState(Env.get("e2e.provider.rid", "provider-a"))

    @Bean
    open fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    open fun evidenceHttpServer(
        state: ScenarioState,
        json: ObjectMapper,
        runtimeOptions: ZLinkChannelRuntimeOptions,
        applicationContext: ConfigurableApplicationContext,
    ): EvidenceHttpServer =
        EvidenceHttpServer(
            state,
            json,
            Env.get("e2e.http.endpoint"),
            runtimeOptions,
            applicationContext,
        )

    @Bean
    open fun providerFramework(state: ScenarioState): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val logDir = Env.get("e2e.log.dir", "logs")
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("$logDir/${state.providerRid()}-flow.log")
                .traceLabel("kotlin-rl-${state.providerRid()}")
                .setMessageFlowObserver(EvidenceDispatchErrorObserver(state))
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(3))
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500))
            options.configureLocations().setPollingInterval(Duration.ofMillis(200))
            options.addHandlersFromPackageOf(WorkRequestHandler::class.java)
            val apiEndpoint = URI.create(Env.get("e2e.api.endpoint"))
            options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setAdvertiseHost(apiEndpoint.host)
                .listen(apiEndpoint.port)
                .addHandlerGroup(Contracts.HANDLER_GROUP)
        }

    @Bean
    open fun locationStore(): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix")),
        )

    @Bean
    open fun workRequestHandler(state: ScenarioState): WorkRequestHandler =
        WorkRequestHandler(state)

    @Bean
    open fun workCommandHandler(state: ScenarioState): WorkCommandHandler =
        WorkCommandHandler(state)

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val builder = SpringApplicationBuilder(ProviderApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }
    }
}
