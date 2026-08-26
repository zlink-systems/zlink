package systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode

import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.DeliveryDispatchReadinessReporter
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.deliverydispatch.server.courierspotnode.spots.CourierEntrySpot
import systems.zlink.contracts.core.RoutingId

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [CourierSpotNodeApplication::class],
)
class CourierSpotNodeApplication {
    @Bean
    fun courierSpotNodeFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.useCoroutineHandlers(Dispatchers.Default)
            val node = SampleTopology.CourierNode
            val selected = NodeOptions.resolve(node)
            options.addHandlersFromPackageOf(CourierSpotNodeApplication::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL)

            val spotNode = options.addRouteMesh(SampleNames.CourierSpotMesh)
            //  Fixed RID so Dispatch can name this courier node in its actor-route readiness row.
            //  An auto-assigned RID cannot be named, and peer status carries no endpoint.
            spotNode.listen(selected.spotEndpoint)
                .setRoutingId(
                    RoutingId.from(
                        if (SampleTopology.CourierNode == "node2") {
                            SampleNames.CourierNode2
                        } else {
                            SampleNames.CourierNode1
                        },
                    ),
                )
            spotNode.objects().server()
                .addEntrySpot(CourierEntrySpot::class.java)
                .addActorFactory(
                    SampleNames.CourierActorType,
                    CourierActor::class.java,
                    CourierActorFactory::class.java,
                ) { factory -> factory.disableRelocation() }
            // The courier's decision goes back to dispatch as its own one-way message, so this
            // node needs a way to speak to the dispatch channel (common sample spec section 7.4).
            options.addClientServerChannel(SampleNames.DispatchChannel)
                .client()
        }

    @Bean
    fun actorDirectory(): ActorDirectory = ActorDirectory()

    @Bean
    fun locationStore(): ZLinkRedisLocationStore = SampleLocationStore.create()

    @Bean(destroyMethod = "close")
    fun readinessReporter(meshes: ZLinkRouteMeshRuntime): DeliveryDispatchReadinessReporter =
        DeliveryDispatchReadinessReporter.route(
            if (SampleTopology.CourierNode == "node2") {
                SampleNames.CourierNode2
            } else {
                SampleNames.CourierNode1
            },
            SampleNames.CourierSpotMesh,
            meshes,
        )

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val builder = SpringApplicationBuilder(CourierSpotNodeApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }

    private data class NodeOptions(
        val spotEndpoint: String,
    ) {
        companion object {
            fun resolve(node: String): NodeOptions =
                if (node == "node2") {
                    NodeOptions(
                        SampleTopology.CourierActorNode2SpotEndpoint,
                    )
                } else {
                    NodeOptions(
                        SampleTopology.CourierActorNode1SpotEndpoint,
                    )
                }
        }
    }
}
