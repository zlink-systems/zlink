package systems.zlink.e2e.kotlin.instancespot.owner

import com.fasterxml.jackson.databind.ObjectMapper
import java.time.Duration
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.e2e.kotlin.instancespot.shared.Env
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class OwnerApplication {
    @Bean fun options(): OwnerOptions = OwnerOptions.fromEnv()
    @Bean fun json(): ObjectMapper = ObjectMapper()
    @Bean fun evidence(options: OwnerOptions) = EvidenceStore(options)
    @Bean fun gates() = GateController()

    @Bean
    fun endpoints(
        options: OwnerOptions,
        evidence: EvidenceStore,
        gates: GateController,
        json: ObjectMapper,
    ) = OwnerEndpoints(options, evidence, gates, json)

    @Bean
    fun locationStore(options: OwnerOptions) = ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint)
            .setKeyPrefix(options.locationKeyPrefix)
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis)),
    )

    @Bean
    fun ownerFramework(options: OwnerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
            framework.useCoroutineHandlers(Dispatchers.Default)
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${options.logDir}/${options.rid}-flow.log")
                .traceLabel(options.rid)
            val locations = framework.configureLocations()
            locations.setOwnerLeaseRenewInterval(Duration.ofMillis(options.heartbeatMillis))
            locations.setOwnerLeaseTtl(Duration.ofMillis(options.leaseTtlMillis))
            locations.setPollingInterval(Duration.ofMillis(options.pollingMillis))
            locations.setStoreFailureGrace(Duration.ofMillis(options.storeFailureGraceMillis))
            val mesh = framework.addRouteMesh(Contracts.MESH)
                .listen(options.meshEndpoint)
                .setRoutingIdPrefix(options.rid)
            mesh.channelName(Contracts.MESH).server().addHandlerGroup(Contracts.HANDLER_GROUP)
            mesh.objects().server().addInstanceSpotFactory(
                Contracts.STABLE_TYPE,
                ProbeSpot::class.java,
            ) { factory ->
                if (options.stableTypeLimit > 0) factory.stableTypeLimit(options.stableTypeLimit)
                if (options.disableRelocation) factory.disableRelocation()
            }
        }

    companion object {
        fun run(args: Array<out String>) {
            Env.configure(args)
            val context = SpringApplicationBuilder(OwnerApplication::class.java)
                .web(WebApplicationType.NONE)
                .run(*Env.withoutConfig(args))
            context.registerShutdownHook()
        }
    }
}
