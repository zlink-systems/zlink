package systems.zlink.e2e.kotlin.registrationcodec.main

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
import systems.zlink.e2e.kotlin.registrationcodec.isPackedType
import systems.zlink.e2e.kotlin.registrationcodec.main.configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrationcodec.main.endpoints.EvidenceHttpServer
import systems.zlink.e2e.kotlin.registrationcodec.main.endpoints.RegistrationScenarioEndpoints
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.AttrEchoHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.AutoRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.AutoSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.DiLifecycleRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.FirstOrderFilter
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.JsonRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.JsonSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.ManualRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.ManualSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.MsgpackRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.MsgpackSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.ProtobufRequestHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.ProtobufSendHandler
import systems.zlink.e2e.kotlin.registrationcodec.main.handlers.SecondOrderFilter
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.DiScopedDependency
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.DiSingletonDependency
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.ScenarioState
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.registrationcodec.main.handlers"],
)
class ServerApplication {
    @Bean fun scenarioState(): ScenarioState = ScenarioState()
    @Bean fun objectMapper(): ObjectMapper = ObjectMapper()
    @Bean fun serverOptions(args: ApplicationArguments): ServerOptions =
        ServerOptions.parse(args.sourceArgs)
    @Bean
    fun evidenceHttpServer(
        state: ScenarioState,
        json: ObjectMapper,
        serverOptions: ServerOptions,
        scenarioEndpoints: RegistrationScenarioEndpoints,
    ): EvidenceHttpServer =
        EvidenceHttpServer(state, json, serverOptions.httpEndpoint, scenarioEndpoints)

    @Bean
    fun registrationScenarioEndpoints(client: ZLinkClient, json: ObjectMapper): RegistrationScenarioEndpoints =
        RegistrationScenarioEndpoints(client, json)

    @Bean
    fun serverFramework(serverOptions: ServerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            if (!serverOptions.jsonOnly) {
                options.codecs().use(ZLinkProtobufCodec.defaultCodec())
                options.codecs().use(ZLinkMessagePackCodec.forPayloadTypes(::isPackedType))
            }
            options.useFilter(FirstOrderFilter::class.java)
            options.useFilter(SecondOrderFilter::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${serverOptions.logDir}/server-flow.log")
                .traceLabel("kotlin-rc-server")
            options.addHandlersFromPackageOf(AutoRequestHandler::class.java)
            val endpoint = java.net.URI.create(serverOptions.serverEndpoint)
            val channel = options.addClientServerChannel(Contracts.CHANNEL)
            val server = channel.server()
                .setBindHost(endpoint.host)
                .setAdvertiseHost(endpoint.host)
                .listen(endpoint.port)
                .addHandlerGroup(Contracts.AUTO_GROUP)
                .addHandlerGroup(Contracts.ATTR_GROUP)
                .addHandlerGroup(Contracts.MANUAL_GROUP)
            channel.client().connect(serverOptions.serverEndpoint)
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
