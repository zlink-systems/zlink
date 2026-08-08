package systems.zlink.e2e.kotlin.observabilityops.a5.server

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import java.time.Duration
import java.util.concurrent.CountDownLatch

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class A5Application {
    @Bean fun objectMapper() = ObjectMapper()
    @Bean fun flowEvidence() = FlowEvidence()
    @Bean fun probeHandler() = ProbeHandler()

    @Bean
    fun httpServer(
        json: ObjectMapper,
        runtimeOptions: ZLinkChannelRuntimeOptions,
        runtime: org.springframework.beans.factory.ObjectProvider<systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime>,
        routes: systems.zlink.framework.channels.ZLinkRouteClient,
        evidence: FlowEvidence,
    ) = A5HttpServer(json, runtimeOptions, runtime, routes, evidence)

    @Bean
    fun locationStore() = ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions()
            .setConnectionString(Env.get("e2e.redis.location.endpoint"))
            .setKeyPrefix(Env.get("e2e.location.key.prefix"))
            .setCommandTimeout(Duration.ofMillis(500)),
    )

    @Bean
    fun frameworkConfigurer(evidence: FlowEvidence): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            evidence.install()
            options.configureLocations()
                .setOwnerLeaseRenewInterval(Duration.ofMillis(500))
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL)

            val endpoint = java.net.URI.create(Env.get("e2e.route.endpoint"))
            val channel = options.addClientServerChannel(Contracts.CHANNEL)
            channel
                .server()
                .setBindHost(endpoint.host)
                .setAdvertiseHost(endpoint.host)
                .listen(endpoint.port)
                .addRequestHandler(
                    ProbeHandler::class.java,
                    Contracts.ProbeRequest::class.java,
                    Contracts.ProbeReply::class.java,
                )
            channel.client().connect(Env.get("e2e.route.endpoint"))
        }

    companion object {
        fun run(args: Array<String>) {
            Env.configure(args)
            val context = SpringApplicationBuilder(A5Application::class.java)
                .web(WebApplicationType.NONE)
                .run(*args)
            try {
                CountDownLatch(1).await()
            } finally {
                context.close()
            }
        }
    }
}
