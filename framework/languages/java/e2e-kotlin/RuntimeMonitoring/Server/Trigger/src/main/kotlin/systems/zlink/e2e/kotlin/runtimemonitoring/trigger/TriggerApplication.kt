package systems.zlink.e2e.kotlin.runtimemonitoring.trigger

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.SpringBootConfiguration
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.EnableAutoConfiguration
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.e2e.kotlin.runtimemonitoring.service.EvidenceState
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootConfiguration(proxyBeanMethods = false)
@EnableAutoConfiguration
class TriggerApplication {
    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun evidenceState(): EvidenceState = EvidenceState()

    @Bean
    fun triggerHttpServer(
        state: EvidenceState,
        json: ObjectMapper,
        client: ZLinkClient,
    ): TriggerHttpServer {
        return TriggerHttpServer(
            Env.get("e2e.http.endpoint"),
            state,
            json,
            client,
        )
    }

    @Bean
    fun frameworkConfigurer(): ZLinkFrameworkConfigurer {
        return ZLinkFrameworkConfigurer { options ->
            val logDir = Env.get("e2e.log.dir", "logs")
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("$logDir/trigger-flow.log")
                .traceLabel("kotlin-mon-trigger")
            options.addClientServerChannel(Contracts.CHANNEL)
                .client()
                .connect(Env.get("e2e.service.api.endpoint"))
        }
    }

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val builder = SpringApplicationBuilder(TriggerApplication::class.java)
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }
    }
}
