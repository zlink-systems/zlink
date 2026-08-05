package systems.zlink.e2e.registrationcodec.invalidduplicate;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.registrationcodec.invalidduplicate.Configuration.ServerOptions;
import systems.zlink.e2e.registrationcodec.invalidduplicate.Handlers.ManualRequestHandler;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ServerOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class InvalidDuplicateApplication {
    public AutoCloseable run(String... args) {
        String config = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder = new SpringApplicationBuilder(InvalidDuplicateApplication.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }


    @Bean
    ZLinkFrameworkConfigurer invalidFramework(ServerOptions options) {
        return framework -> {
            var endpoint = java.net.URI.create(options.serverEndpoint());
            var channel = framework.addClientServerChannel(Contracts.CHANNEL);
            var server = channel.server()
                .setBindHost(endpoint.getHost())
                .setAdvertiseHost(endpoint.getHost())
                .listen(endpoint.getPort());
            server.addRequestHandler(
                ManualRequestHandler.class,
                Contracts.EchoManualReq.class,
                Contracts.EchoRes.class);
            server.addRequestHandler(
                ManualRequestHandler.class,
                Contracts.EchoManualReq.class,
                Contracts.EchoRes.class);
        };
    }

    @Bean
    ManualRequestHandler manualRequestHandler() {
        return new ManualRequestHandler();
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: registration-codec-invalid-duplicate --config <path>");
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
