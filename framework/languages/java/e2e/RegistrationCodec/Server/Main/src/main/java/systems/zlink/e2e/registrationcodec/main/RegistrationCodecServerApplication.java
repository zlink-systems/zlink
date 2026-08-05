package systems.zlink.e2e.registrationcodec.main;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.protobuf.StringValue;
import java.nio.file.Path;
import java.util.Set;
import org.springframework.beans.factory.config.ConfigurableBeanFactory;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Scope;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.registrationcodec.main.Configuration.ServerOptions;
import systems.zlink.e2e.registrationcodec.main.Endpoints.OperationalEndpoints;
import systems.zlink.e2e.registrationcodec.main.Handlers.AttrEchoHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.AutoRequestHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.AutoSendHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.DiLifecycleReqHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.FirstOrderFilter;
import systems.zlink.e2e.registrationcodec.main.Handlers.JsonRequestHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.JsonSendHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.ManualRequestHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.ManualSendHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.MsgpackRequestHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.MsgpackSendHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.ProtobufRequestHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.ProtobufSendHandler;
import systems.zlink.e2e.registrationcodec.main.Handlers.SecondOrderFilter;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.DiScopedDependency;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.DiSingletonDependency;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ServerOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.registrationcodec.main.Handlers")
public final class RegistrationCodecServerApplication {
    public AutoCloseable run(String... args) {
        String config = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder =
            new SpringApplicationBuilder(RegistrationCodecServerApplication.class)
                .environment(environment)
                .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
                .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }

    @Bean
    EvidenceStore evidenceStore() {
        return new EvidenceStore();
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    OperationalEndpoints operationalEndpoints(
        EvidenceStore evidence,
        ObjectMapper json,
        systems.zlink.framework.channels.ZLinkClient client,
        ServerOptions options) {
        return new OperationalEndpoints(evidence, json, client, options.httpEndpoint());
    }

    @Bean
    ZLinkFrameworkConfigurer serverFramework(ServerOptions options) {
        return framework -> {
            framework.codecs().use(ZLinkProtobufCodec.defaultCodec());
            framework.codecs().use(ZLinkMessagePackCodec.forPayloadTypes(
                RegistrationCodecServerApplication::isPackedType));
            framework.useFilter(FirstOrderFilter.class);
            framework.useFilter(SecondOrderFilter.class);
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/server-flow.log")
                .traceLabel("java-rc-server");
            framework.addHandlersFromPackageOf(AutoRequestHandler.class);
            var endpoint = java.net.URI.create(options.serverEndpoint());
            var channel = framework.addClientServerChannel(Contracts.CHANNEL);
            var server = channel.server()
                .setBindHost(endpoint.getHost())
                .setAdvertiseHost(endpoint.getHost())
                .listen(endpoint.getPort())
                .addHandlerGroup(Contracts.AUTO_GROUP)
                .addHandlerGroup(Contracts.ATTR_GROUP);
            channel.client().connect(options.serverEndpoint());
            server.addRequestHandler(
                ManualRequestHandler.class,
                Contracts.EchoManualReq.class,
                Contracts.EchoRes.class);
            server.addSendHandler(
                ManualSendHandler.class,
                Contracts.EchoManualMsg.class);
            server.addRequestHandler(
                DiLifecycleReqHandler.class,
                Contracts.DiLifecycleReq.class,
                Contracts.DiLifecycleRes.class);
            server.addRequestHandler(
                JsonRequestHandler.class,
                Contracts.JsonEchoReq.class,
                Contracts.EchoRes.class);
            server.addSendHandler(
                JsonSendHandler.class,
                Contracts.JsonEchoMsg.class);
            server.addRequestHandler(
                ProtobufRequestHandler.class,
                StringValue.class,
                StringValue.class);
            server.addSendHandler(
                ProtobufSendHandler.class,
                StringValue.class);
            server.addRequestHandler(
                MsgpackRequestHandler.class,
                Contracts.PackedEchoReq.class,
                Contracts.PackedEchoRes.class);
            server.addSendHandler(
                MsgpackSendHandler.class,
                Contracts.PackedEchoMsg.class);
        };
    }

    private static boolean isPackedType(Class<?> type) {
        return Set.of(
            Contracts.PackedEchoReq.class,
            Contracts.PackedEchoRes.class,
            Contracts.PackedEchoMsg.class).contains(type);
    }

    @Bean AutoRequestHandler autoRequestHandler(EvidenceStore evidence) { return new AutoRequestHandler(evidence); }
    @Bean AutoSendHandler autoSendHandler(EvidenceStore evidence) { return new AutoSendHandler(evidence); }
    @Bean AttrEchoHandler attrEchoHandler(EvidenceStore evidence) { return new AttrEchoHandler(evidence); }
    @Bean ManualRequestHandler manualRequestHandler(EvidenceStore evidence) { return new ManualRequestHandler(evidence); }
    @Bean ManualSendHandler manualSendHandler(EvidenceStore evidence) { return new ManualSendHandler(evidence); }
    @Bean DiLifecycleReqHandler diLifecycleRequestHandler(
        org.springframework.beans.factory.ObjectProvider<DiScopedDependency> scoped,
        DiSingletonDependency singleton,
        EvidenceStore evidence) {
        return new DiLifecycleReqHandler(scoped, singleton, evidence);
    }
    @Bean DiSingletonDependency diSingletonDependency() { return new DiSingletonDependency(); }
    @Bean
    @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
    DiScopedDependency diScopedDependency(EvidenceStore evidence) { return new DiScopedDependency(evidence); }
    @Bean JsonRequestHandler jsonRequestHandler(EvidenceStore evidence) { return new JsonRequestHandler(evidence); }
    @Bean JsonSendHandler jsonSendHandler(EvidenceStore evidence) { return new JsonSendHandler(evidence); }
    @Bean ProtobufRequestHandler protobufRequestHandler(EvidenceStore evidence) { return new ProtobufRequestHandler(evidence); }
    @Bean ProtobufSendHandler protobufSendHandler(EvidenceStore evidence) { return new ProtobufSendHandler(evidence); }
    @Bean MsgpackRequestHandler msgpackRequestHandler(EvidenceStore evidence) { return new MsgpackRequestHandler(evidence); }
    @Bean MsgpackSendHandler msgpackSendHandler(EvidenceStore evidence) { return new MsgpackSendHandler(evidence); }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: registration-codec-main --config <path>");
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
