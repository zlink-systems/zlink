package systems.zlink.e2e.kotlin.registrymessaging.workflow

import java.io.File
import org.springframework.boot.ApplicationRunner
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowReq
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Endpoints.WorkflowEndpoints
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Handlers.EvidenceDispatchErrorObserver
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Handlers.WorkflowRequestHandler
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Infrastructure.EvidenceStore
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

private lateinit var parsedOptions: ServerOptions

fun main(args: Array<String>) {
    parsedOptions = ServerOptions.parse(args)
    File(parsedOptions.logDir).mkdirs()
    val context = SpringApplicationBuilder(WorkflowApplication::class.java)
        .web(WebApplicationType.NONE)
        .run(*args)
    WorkflowEndpoints(parsedOptions, context).start()
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class WorkflowApplication {
    @Bean
    fun serverOptions(): ServerOptions = parsedOptions

    @Bean
    fun evidenceStore(options: ServerOptions): EvidenceStore =
        EvidenceStore(options.rid, options.evidenceFile)

    @Bean
    fun framework(options: ServerOptions, evidence: EvidenceStore): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
            framework.configureDispatch()
                .setMessageFlowObserver(EvidenceDispatchErrorObserver(evidence))
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${options.logDir}/${options.rid}-flow.log")
                .traceLabel(options.rid)
            framework.addHandlersFromPackageOf(WorkflowRequestHandler::class.java)

            val endpoint = java.net.URI.create(options.workflowEndpoint)
            framework.addClientServerChannel(Contracts.WORKFLOW_CHANNEL)
                .server()
                .setBindHost(endpoint.host)
                .setAdvertiseHost(endpoint.host)
                .listen(endpoint.port)
                .addHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
        }

    @Bean
    fun locationStore(options: ServerOptions): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(options.redisLocationEndpoint)
                .setKeyPrefix(options.locationKeyPrefix),
        )

    @Bean
    fun applyInitialSocketWeight(
        options: ServerOptions,
        runtimeOptions: ZLinkChannelRuntimeOptions,
    ): ApplicationRunner =
        ApplicationRunner {
            runtimeOptions
                .clientServerChannel(Contracts.WORKFLOW_CHANNEL)
                .configureServerSocket()
                .weight(options.weight)
        }

    @Bean
    fun workflowRequestHandler(evidence: EvidenceStore): WorkflowRequestHandler =
        WorkflowRequestHandler(evidence)
}
