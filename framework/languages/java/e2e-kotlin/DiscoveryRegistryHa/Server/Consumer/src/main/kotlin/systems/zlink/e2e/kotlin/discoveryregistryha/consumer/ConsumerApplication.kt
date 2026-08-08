package systems.zlink.e2e.kotlin.discoveryregistryha.consumer

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import java.time.Duration
import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts
import systems.zlink.e2e.kotlin.discoveryregistryha.consumer.Configuration.ConsumerOptions
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locationprovider.ZLinkLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.discoveryregistryha.consumer"],
)
class ConsumerApplication {
    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            val builder = SpringApplicationBuilder(ConsumerApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun consumerOptions(args: ApplicationArguments): ConsumerOptions =
        ConsumerOptions.parse(args.sourceArgs)

    @Bean
    fun consumerFramework(consumerOptions: ConsumerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL)


            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(consumerOptions.heartbeatMillis))
            options.configureLocations().setOwnerLeaseTtl(Duration.ofMillis(consumerOptions.leaseTtlMillis))
            options.configureLocations().setPollingInterval(Duration.ofMillis(consumerOptions.pollingMillis))
            options.configureLocations().setStoreFailureGrace(Duration.ofMillis(consumerOptions.storeFailureGraceMillis))
            options.addRouteMesh(Contracts.CHANNEL)
                .listen("tcp://127.0.0.1:0")
                .setRoutingIdPrefix(consumerOptions.rid)
                .channelName(Contracts.CHANNEL)
                .client()
        }

    @Bean
    fun locationStoreDelayState(): LocationStoreDelayState = LocationStoreDelayState()

    @Bean
    fun locationStore(
        consumerOptions: ConsumerOptions,
        delayState: LocationStoreDelayState,
    ): ZLinkLocationStore {
        val redisStore = ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(consumerOptions.redisLocationEndpoint)
                .setKeyPrefix(consumerOptions.locationKeyPrefix)
                .setCommandTimeout(Duration.ofMillis(consumerOptions.redisCommandTimeoutMillis)),
        )
        return when (consumerOptions.storeMode) {
            "delay" -> DelayableLocationStore(redisStore, delayState)
            else -> redisStore
        }
    }

    @Bean
    fun consumerHttpServer(
        client: ZLinkClient,
        routes: ZLinkRouteClient,
        lifecycle: ZLinkFrameworkLifecycle,
        json: ObjectMapper,
        consumerOptions: ConsumerOptions,
        delayState: LocationStoreDelayState,
    ): ConsumerHttpServer =
        ConsumerHttpServer(
            client,
            routes,
            lifecycle,
            json,
            consumerOptions,
            delayState,
        )
}
