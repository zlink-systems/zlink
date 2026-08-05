package systems.zlink.e2e.registrationcodec.jsononlypeer;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Configuration.ServerOptions;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Endpoints.OperationalEndpoints;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers.JsonRequestHandler;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers.MsgpackRequestHandler;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers.UnexpectedProtobufHandler;
import systems.zlink.e2e.registrationcodec.jsononlypeer.Infrastructure.EvidenceStore;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ServerOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.registrationcodec.jsononlypeer.Handlers")
public final class JsonOnlyPeerApplication {
    public AutoCloseable run(String... args) {
        String config = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder = new SpringApplicationBuilder(JsonOnlyPeerApplication.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }

    @Bean EvidenceStore evidenceStore() { return new EvidenceStore(); }
    @Bean ObjectMapper objectMapper() { return new ObjectMapper(); }
    @Bean OperationalEndpoints operationalEndpoints(EvidenceStore evidence, ObjectMapper json, ServerOptions options) {
        return new OperationalEndpoints(evidence, json, options.httpEndpoint());
    }

    @Bean
    ZLinkFrameworkConfigurer serverFramework(ServerOptions options) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/json-only-flow.log")
                .traceLabel("java-rc-json-only");
            var endpoint = java.net.URI.create(options.serverEndpoint());
            var channel = framework.addClientServerChannel(Contracts.CHANNEL);
            var server = channel.server()
                .setBindHost(endpoint.getHost())
                .setAdvertiseHost(endpoint.getHost())
                .listen(endpoint.getPort());
            server.addRequestHandler(
                JsonRequestHandler.class,
                Contracts.JsonEchoReq.class,
                Contracts.EchoRes.class);
            server.addRequestHandler(
                MsgpackRequestHandler.class,
                Contracts.PackedEchoReq.class,
                Contracts.PackedEchoRes.class);
            server.addRequestHandler(
                UnexpectedProtobufHandler.class,
                com.google.protobuf.StringValue.class,
                com.google.protobuf.StringValue.class);
        };
    }

    @Bean JsonRequestHandler jsonRequestHandler(EvidenceStore evidence) { return new JsonRequestHandler(evidence); }
    @Bean MsgpackRequestHandler msgpackRequestHandler(EvidenceStore evidence) { return new MsgpackRequestHandler(evidence); }
    @Bean UnexpectedProtobufHandler unexpectedProtobufHandler(EvidenceStore evidence) {
        return new UnexpectedProtobufHandler(evidence);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: registration-codec-json-only-peer --config <path>");
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
