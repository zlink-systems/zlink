package systems.zlink.samples.kotlin.deliverydispatch.server.couriersession

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
import systems.zlink.samples.kotlin.deliverydispatch.server.couriersession.sessions.CourierSession

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [CourierSessionApplication::class],
)
class CourierSessionApplication {
    @Bean
    fun courierSessionFramework(): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(CourierSessionApplication::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(
                    SampleTopology.LogDirectory +
                        "/flow-courier-session.log",
                )
                .traceLabel("courier-session")
            options.addClientServerChannel(SampleNames.CourierChannel)
                .client()
            val node = options.addRouteMesh(SampleNames.CourierSpotMesh)
            node.listen(SampleTopology.CourierSessionSpotEndpoint)
                .setRoutingIdPrefix("delivery-session")
            node.objects().client()
            options.addStreamNode(SampleNames.CourierStreamNode)
                .bind(SampleTopology.CourierStreamEndpoint)
                .enableActorDispatch()
                .registerSession(CourierSession::class.java)
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore = SampleLocationStore.create()

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val builder = SpringApplicationBuilder(CourierSessionApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*args)
            return AutoCloseable { context.close() }
        }
    }
}
