package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.KotlinModule
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [DispatchServerApplication::class],
)
class DispatchServerApplication {
    @Bean
    fun dispatchFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(DispatchServerApplication::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(
                    SampleTopology.LogDirectory +
                        "/flow-dispatch.log",
                )
                .traceLabel("dispatch")
            options.addClientServerChannel(SampleNames.CourierChannel)
                .client()
            // The courier's decision comes back here as its own one-way message, so dispatch has
            // to be a channel server (common sample spec section 7.4).
            val dispatchEndpoint = java.net.URI.create(SampleTopology.DispatchChannelEndpoint)
            options.addClientServerChannel(SampleNames.DispatchChannel)
                .server()
                .setBindHost(dispatchEndpoint.host)
                .setAdvertiseHost(dispatchEndpoint.host)
                .listen(dispatchEndpoint.port)
                .addHandlerGroup(SampleNames.DispatchChannel)
            options.addClientServerChannel(SampleNames.TrackingChannel)
                .client()
            val courierRoutes = options.addRouteMesh(SampleNames.CourierSpotMesh)
            courierRoutes
                .listen(SampleTopology.DispatchSpotEndpoint)
                .setRoutingIdPrefix("delivery-dispatch")
            courierRoutes.objects().client()
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore = SampleLocationStore.create()

    @Bean
    fun dispatchWorkQueue(worker: DispatchWorker): DispatchWorkQueue = DispatchWorkQueue(worker)

    @Bean
    fun deliveryOfferStore(): DeliveryOfferStore = DeliveryOfferStore()

    @Bean
    fun dispatchWorker(
        channels: ZLinkClient,
        actors: systems.zlink.framework.actors.ZLinkActorClient,
        offers: DeliveryOfferStore,
    ): DispatchWorker = DispatchWorker(channels, actors, offers)

    @Bean(destroyMethod = "close")
    fun offerDeadlineSweeper(
        offers: DeliveryOfferStore,
        worker: DispatchWorker,
    ): OfferDeadlineSweeper = OfferDeadlineSweeper(offers, worker)

    @Bean
    fun dispatchHttpServer(
        json: ObjectMapper,
        queue: DispatchWorkQueue,
    ): DispatchHttpServer = DispatchHttpServer(json, queue)

    @Bean
    fun objectMapper(): ObjectMapper =
        ObjectMapper().registerModule(KotlinModule.Builder().build())

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val builder = SpringApplicationBuilder(DispatchServerApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }
}
