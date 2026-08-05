package systems.zlink.e2e.registrationcodec.codecrequester;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import java.util.Set;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.registrationcodec.codecrequester.Configuration.CodecRequesterOptions;
import systems.zlink.e2e.registrationcodec.codecrequester.Endpoints.CodecRequesterEndpoints;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(CodecRequesterOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class CodecRequesterApplication {
    public AutoCloseable run(String... args) {
        String config = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder =
            new SpringApplicationBuilder(CodecRequesterApplication.class)
                .environment(environment)
                .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
                .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }


    @Bean ObjectMapper objectMapper() { return new ObjectMapper(); }

    @Bean
    CodecRequesterEndpoints codecRequesterEndpoints(
        CodecRequesterOptions options,
        ObjectMapper json,
        systems.zlink.framework.channels.ZLinkClient client) {
        return new CodecRequesterEndpoints(options, json, client);
    }

    @Bean
    ZLinkFrameworkConfigurer requesterFramework(CodecRequesterOptions options) {
        return framework -> {
            framework.codecs().use(ZLinkProtobufCodec.defaultCodec());
            framework.codecs().use(ZLinkMessagePackCodec.forPayloadTypes(
                CodecRequesterApplication::isPackedType));
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/codec-requester-flow.log")
                .traceLabel("java-rc-codec-requester");
            framework.addClientServerChannel(Contracts.CHANNEL)
                .client()
                .connect(options.serverEndpoint());
        };
    }

    private static boolean isPackedType(Class<?> type) {
        return Set.of(
            Contracts.PackedEchoReq.class,
            Contracts.PackedEchoRes.class,
            Contracts.PackedEchoMsg.class).contains(type);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: registration-codec-requester --config <path>");
        }
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
