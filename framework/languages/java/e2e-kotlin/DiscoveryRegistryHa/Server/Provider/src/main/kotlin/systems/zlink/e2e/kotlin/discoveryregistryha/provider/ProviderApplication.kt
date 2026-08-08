package systems.zlink.e2e.kotlin.discoveryregistryha.provider

import java.time.Duration
import java.net.URI
import kotlinx.coroutines.Dispatchers
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
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.ObjectProbeSpot
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Support.ProviderEvidenceStore
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.kotlin.useCoroutineHandlers

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
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL)


            options.addHandlersFromPackageOf(WorkRequestHandler::class.java)
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(providerOptions.heartbeatMillis()))
            options.configureLocations().setOwnerLeaseTtl(Duration.ofMillis(providerOptions.leaseTtlMillis()))
            options.configureLocations().setPollingInterval(Duration.ofMillis(providerOptions.pollingMillis()))
            options.configureLocations().setStoreFailureGrace(Duration.ofMillis(providerOptions.storeFailureGraceMillis()))
            val mesh = options.addRouteMesh(Contracts.CHANNEL)
                .listen(providerOptions.apiEndpoint())
                .setRoutingIdPrefix(state.providerRid)
            mesh.channelName(Contracts.CHANNEL)
                .server()
                .addHandlerGroup(Contracts.HANDLER_GROUP)
            mesh.objects().server().addInstanceSpotFactory(
                Contracts.OBJECT_TYPE,
                ObjectProbeSpot::class.java,
            ) { factory -> factory.disableRelocation() }
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
