package systems.zlink.e2e.kotlin.registrationcodec

import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.registrationcodec.invalidduplicate.configuration.ServerOptions
import systems.zlink.e2e.kotlin.registrationcodec.handlers.ManualRequestHandler
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.registrationcodec.invalid"],
)
class InvalidServerApplication {
    @Bean fun scenarioState(): ScenarioState = ScenarioState()
    @Bean fun serverOptions(args: ApplicationArguments): ServerOptions =
        ServerOptions.parse(args.sourceArgs)

    @Bean
    fun invalidFramework(serverOptions: ServerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val endpoint = java.net.URI.create(serverOptions.serverEndpoint)
            val server = options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setBindHost(endpoint.host)
                .setAdvertiseHost(endpoint.host)
                .listen(endpoint.port)
            server.addRequestHandler(ManualRequestHandler::class.java, EchoManualReq::class.java, EchoManualRes::class.java)
            server.addRequestHandler(ManualRequestHandler::class.java, EchoManualReq::class.java, EchoManualRes::class.java)
        }

    @Bean fun manualRequestHandler(state: ScenarioState): ManualRequestHandler =
        ManualRequestHandler(state)
}

fun runInvalidServerApplication(vararg args: String): AutoCloseable {
    Env.configure(args)
    val builder = SpringApplicationBuilder(InvalidServerApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    val context = builder.run(*Env.applicationArgs(args))
    return AutoCloseable { context.close() }
}
