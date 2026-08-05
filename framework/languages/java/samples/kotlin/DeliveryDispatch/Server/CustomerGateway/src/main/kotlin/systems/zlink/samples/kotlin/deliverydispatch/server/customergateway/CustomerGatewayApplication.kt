package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.sessions.CustomerSession
import systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.spots.CustomerEntrySpot

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [CustomerGatewayApplication::class],
)
class CustomerGatewayApplication {
    @Bean
    fun customerGatewayFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(CustomerGatewayApplication::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(
                    SampleTopology.LogDirectory +
                        "/flow-customer-gateway.log",
                )
                .traceLabel("customer-gateway")
            val node = options.addRouteMesh(SampleNames.CustomerSpotMesh)
            node.listen(SampleTopology.CustomerSpotRouterEndpoint)
                .setRoutingIdPrefix("delivery-customer")
            node.objects().server()
                .addEntrySpot(CustomerEntrySpot::class.java)
                .addActorFactory(
                    SampleNames.CustomerActorType,
                    CustomerActor::class.java,
                    CustomerActorFactory::class.java,
                ) { factory -> factory.disableRelocation() }
            options.addStreamNode(SampleNames.CustomerStreamNode)
                .bind(SampleTopology.CustomerStreamEndpoint)
                .enableActorDispatch()
                .registerSession(CustomerSession::class.java)
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore = SampleLocationStore.create()

    @Bean
    fun customerActorDirectory(): CustomerActorDirectory = CustomerActorDirectory()

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val builder = SpringApplicationBuilder(CustomerGatewayApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }
}
