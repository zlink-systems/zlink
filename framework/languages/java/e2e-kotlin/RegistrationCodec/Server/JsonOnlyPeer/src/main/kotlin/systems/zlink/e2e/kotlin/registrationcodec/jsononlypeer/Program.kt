package systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer

import com.fasterxml.jackson.databind.ObjectMapper
import com.google.protobuf.StringValue
import org.springframework.beans.factory.ObjectProvider
import org.springframework.beans.factory.config.ConfigurableBeanFactory
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Scope
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.Env
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleRes
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.endpoints.EvidenceHttpServer
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.AttrEchoHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.AutoRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.AutoSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.DiLifecycleRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.FirstOrderFilter
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.JsonRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.JsonSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.ManualRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.ManualSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.MsgpackRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.MsgpackSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.ProtobufRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.ProtobufSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers.SecondOrderFilter
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.infrastructure.DiScopedDependency
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.infrastructure.DiSingletonDependency
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.infrastructure.ScenarioState
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers"],
)
class ServerApplication {
    @Bean fun scenarioState(): ScenarioState = ScenarioState()
    @Bean fun objectMapper(): ObjectMapper = ObjectMapper()
    @Bean fun serverOptions(args: ApplicationArguments): ServerOptions =
        ServerOptions.parse(args.sourceArgs)
    @Bean fun evidenceHttpServer(state: ScenarioState, json: ObjectMapper, serverOptions: ServerOptions): EvidenceHttpServer =
        EvidenceHttpServer(state, json, serverOptions.httpEndpoint)

    @Bean
    fun serverFramework(serverOptions: ServerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            if (!serverOptions.jsonOnly) {
                options.codecs().use(ZLinkProtobufCodec.defaultCodec())
                options.codecs().use(ZLinkMessagePackCodec.forPayloadTypes { false })
            }
            options.useFilter(FirstOrderFilter::class.java)
            options.useFilter(SecondOrderFilter::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${serverOptions.logDir}/server-flow.log")
                .traceLabel("kotlin-rc-server")
            options.addHandlersFromPackageOf(AutoRequestHandler::class.java)
            val endpoint = java.net.URI.create(serverOptions.serverEndpoint)
            val server = options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setBindHost(endpoint.host)
                .setAdvertiseHost(endpoint.host)
                .listen(endpoint.port)
                .addHandlerGroup(Contracts.AUTO_GROUP)
                .addHandlerGroup(Contracts.ATTR_GROUP)
                .addHandlerGroup(Contracts.MANUAL_GROUP)
        }

    @Bean fun autoRequestHandler(state: ScenarioState): AutoRequestHandler = AutoRequestHandler(state)
    @Bean fun autoSendHandler(state: ScenarioState): AutoSendHandler = AutoSendHandler(state)
    @Bean fun attrEchoHandler(state: ScenarioState): AttrEchoHandler = AttrEchoHandler(state)
    @Bean fun manualRequestHandler(state: ScenarioState): ManualRequestHandler = ManualRequestHandler(state)
    @Bean fun manualSendHandler(state: ScenarioState): ManualSendHandler = ManualSendHandler(state)
    @Bean fun diLifecycleRequestHandler(
        scoped: ObjectProvider<DiScopedDependency>,
        singleton: DiSingletonDependency,
        state: ScenarioState,
    ): DiLifecycleRequestHandler = DiLifecycleRequestHandler(scoped, singleton, state)
    @Bean fun diSingletonDependency(): DiSingletonDependency = DiSingletonDependency()
    @Bean
    @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
    fun diScopedDependency(state: ScenarioState): DiScopedDependency = DiScopedDependency(state)
    @Bean fun jsonRequestHandler(state: ScenarioState): JsonRequestHandler = JsonRequestHandler(state)
    @Bean fun jsonSendHandler(state: ScenarioState): JsonSendHandler = JsonSendHandler(state)
    @Bean fun protobufRequestHandler(state: ScenarioState): ProtobufRequestHandler = ProtobufRequestHandler(state)
    @Bean fun protobufSendHandler(state: ScenarioState): ProtobufSendHandler = ProtobufSendHandler(state)
    @Bean fun msgpackRequestHandler(state: ScenarioState): MsgpackRequestHandler = MsgpackRequestHandler(state)
    @Bean fun msgpackSendHandler(state: ScenarioState): MsgpackSendHandler = MsgpackSendHandler(state)
}

fun runServerApplication(vararg args: String): AutoCloseable {
    Env.configure(args)
    val builder = SpringApplicationBuilder(ServerApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    val context = builder.run(*Env.applicationArgs(args))
    return AutoCloseable { context.close() }
}
