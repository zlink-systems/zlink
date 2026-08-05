package systems.zlink.samples.kotlin.deliverydispatch.server.tracking

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
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.DeliveryEvidenceStore
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [TrackingServerApplication::class],
)
class TrackingServerApplication {
    @Bean
    fun trackingFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(
                    SampleTopology.LogDirectory +
                        "/flow-tracking.log",
                )
                .traceLabel("tracking")
            options.addHandlersFromPackageOf(TrackingServerApplication::class.java)
            val trackingSpot = options.addRouteMesh(SampleNames.CustomerSpotMesh)
                .listen(SampleTopology.TrackingSpotEndpoint)
                .setRoutingIdPrefix("delivery-tracking")
            trackingSpot.objects().client()
            val trackingEndpoint = java.net.URI.create(SampleTopology.TrackingChannelEndpoint)
            options.addClientServerChannel(SampleNames.TrackingChannel)
                .server()
                .setBindHost(trackingEndpoint.host)
                .setAdvertiseHost(trackingEndpoint.host)
                .listen(trackingEndpoint.port)
                .addHandlerGroup("tracking")
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore = SampleLocationStore.create()

    @Bean
    fun evidenceStore(): DeliveryEvidenceStore =
        DeliveryEvidenceStore(
            SampleTopology.StateDirectory,
        )

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val builder = SpringApplicationBuilder(TrackingServerApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }
}
