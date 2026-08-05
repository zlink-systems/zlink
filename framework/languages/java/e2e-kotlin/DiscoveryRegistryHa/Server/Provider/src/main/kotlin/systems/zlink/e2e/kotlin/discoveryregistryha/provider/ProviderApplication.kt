package systems.zlink.e2e.kotlin.discoveryregistryha.provider

import java.time.Duration
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import com.fasterxml.jackson.databind.ObjectMapper
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts
import systems.zlink.e2e.kotlin.discoveryregistryha.ProviderOptions
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Handlers.WorkRequestHandler
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Support.ProviderEvidenceStore
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.discoveryregistryha.provider"],
)
class ProviderApplication {
    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            val builder = SpringApplicationBuilder(ProviderApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun providerOptions(args: ApplicationArguments): ProviderOptions =
        ProviderOptions.parse(args.sourceArgs)

    @Bean
    fun providerState(providerOptions: ProviderOptions): ProviderEvidenceStore =
        ProviderEvidenceStore(providerOptions.providerRid())

    @Bean
    fun providerFramework(
        state: ProviderEvidenceStore,
        providerOptions: ProviderOptions,
    ): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${providerOptions.logDir()}/${state.providerRid}-flow.log")
                .traceLabel("kotlin-dr-${state.providerRid}")
            options.addHandlersFromPackageOf(WorkRequestHandler::class.java)
            options.configureLocations().setHeartbeatInterval(Duration.ofMillis(providerOptions.heartbeatMillis()))
            options.configureLocations().setOwnerLeaseTtl(Duration.ofMillis(providerOptions.leaseTtlMillis()))
            options.configureLocations().setPollingInterval(Duration.ofMillis(providerOptions.pollingMillis()))
            options.configureLocations().setStoreFailureGrace(Duration.ofMillis(providerOptions.storeFailureGraceMillis()))
            options.addClientServerChannel(Contracts.CHANNEL)
                .enableServer(providerOptions.apiEndpoint())
                .setRoutingId(RoutingId.from(state.providerRid))
                .addHandlerGroup(Contracts.HANDLER_GROUP)
        }

    @Bean
    fun locationStore(providerOptions: ProviderOptions): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(providerOptions.redisLocationEndpoint())
                .setKeyPrefix(providerOptions.locationKeyPrefix())
                .setCommandTimeout(Duration.ofMillis(providerOptions.redisCommandTimeoutMillis()))
        )

    @Bean
    fun workRequestHandler(state: ProviderEvidenceStore): WorkRequestHandler =
        WorkRequestHandler(state)

    @Bean
    fun providerHttpServer(
        providerOptions: ProviderOptions,
        state: ProviderEvidenceStore,
        json: ObjectMapper,
    ): ProviderHttpServer =
        ProviderHttpServer(providerOptions, state, json)
}
