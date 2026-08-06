package systems.zlink.e2e.kotlin.instancespot.client

import com.fasterxml.jackson.databind.ObjectMapper
import java.time.Duration
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.e2e.kotlin.instancespot.shared.Env
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.channels.ZLinkRouteClient

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class ClientApplication {
    @Bean fun options(): ClientOptions = ClientOptions.fromEnv()
    @Bean fun json(): ObjectMapper = ObjectMapper()

    @Bean
    fun endpoints(
        options: ClientOptions,
        routes: ZLinkRouteClient,
        spots: ZLinkSpotManager,
        json: ObjectMapper,
    ) = ClientEndpoints(options, routes, spots, json)

    @Bean
    fun locationStore(options: ClientOptions) = ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint)
            .setKeyPrefix(options.locationKeyPrefix)
            .setCommandTimeout(Duration.ofMillis(options.redisCommandTimeoutMillis)),
    )

    @Bean
    fun clientFramework(options: ClientOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
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
            mesh.channelName(Contracts.MESH).client()
            mesh.objects().server()
        }

    companion object {
        fun run(args: Array<out String>) {
            Env.configure(args)
            val context = SpringApplicationBuilder(ClientApplication::class.java)
                .web(WebApplicationType.NONE)
                .run(*Env.withoutConfig(args))
            context.registerShutdownHook()
        }
    }
}
