package systems.zlink.e2e.kotlin.registrationcodec.codecrequester

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.codecrequester.configuration.CodecRequesterOptions
import systems.zlink.e2e.kotlin.registrationcodec.codecrequester.endpoints.CodecRequesterHttpServer
import systems.zlink.e2e.kotlin.registrationcodec.Env
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class CodecRequesterApplication {
    @Bean fun objectMapper(): ObjectMapper = ObjectMapper()
    @Bean fun requesterOptions(args: ApplicationArguments): CodecRequesterOptions =
        CodecRequesterOptions.parse(args.sourceArgs)
    @Bean fun codecRequesterHttpServer(
        options: CodecRequesterOptions,
        json: ObjectMapper,
        requester: CodecRequesterProbe,
    ): CodecRequesterHttpServer =
        CodecRequesterHttpServer(options.httpEndpoint, json, requester)

    @Bean
    fun requesterFramework(options: CodecRequesterOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
            framework.codecs().use(ZLinkProtobufCodec.defaultCodec())
            framework.codecs().use(
                ZLinkMessagePackCodec.forPayloadTypes { type ->
                    type == PackedEchoReq::class.java ||
                        type == PackedEchoRes::class.java ||
                        type == PackedEchoMsg::class.java
                },
            )
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${options.logDir}/codec-requester-flow.log")
                .traceLabel("kotlin-rc-codec-requester")
            framework.addClientServerChannel(Contracts.CHANNEL)
                .client()
                .connect(options.serverEndpoint)
        }

    @Bean fun codecRequesterProbe(client: systems.zlink.framework.channels.ZLinkClient): CodecRequesterProbe =
        CodecRequesterProbe(client)
}

fun runCodecRequesterApplication(vararg args: String): AutoCloseable {
    Env.configure(args)
    val builder = SpringApplicationBuilder(CodecRequesterApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    val context = builder.run(*Env.applicationArgs(args))
    return AutoCloseable { context.close() }
}
