package systems.zlink.samples.supportchat.server.api;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.supportchat.server.configuration.SampleFlowLog;
import systems.zlink.samples.supportchat.server.configuration.SampleLocationStore;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTopology;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(proxyBeanMethods = false, scanBasePackageClasses = Program.class)
public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ConfigurableApplicationContext app = run(SampleTopology.configPath(args));
        Runtime.getRuntime().addShutdownHook(new Thread(app::close));
        Thread.currentThread().join();
    }

    public static ConfigurableApplicationContext run(String configPath) {
        StandardEnvironment environment = new StandardEnvironment();
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(environment)
            .web(WebApplicationType.NONE)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri());
        builder.application().setKeepAlive(true);
        return builder.run();
    }

    @Bean
    ZLinkFrameworkConfigurer apiFramework(SampleTopology topology) {
        SampleTopology.Api api = topology.api();
        URI channelEndpoint = URI.create(api.channelEndpoint());
        return options -> {
            options.addHandlersFromPackageOf(Program.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(SampleFlowLog.path(topology, "api"))
                .traceLabel("api");
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(channelEndpoint.getHost())
                .setAdvertiseHost(channelEndpoint.getHost())
                .listen(channelEndpoint.getPort())
                .addHandlerGroup(SampleNames.ApiChannel);
            options.addClientServerChannel(SampleNames.SupportChannel)
                .client();
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean(destroyMethod = "close")
    AutoCloseable apiHttpServer(SampleTopology topology) throws IOException {
        ObjectMapper json = new ObjectMapper();
        URI uri = URI.create(topology.api().httpEndpoint());
        HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/health", exchange -> {
            byte[] bytes = json.writeValueAsString(new Health("ok")).getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().add("content-type", "application/json");
            exchange.sendResponseHeaders(200, bytes.length);
            exchange.getResponseBody().write(bytes);
            exchange.close();
        });
        server.start();
        return () -> server.stop(0);
    }

    private record Health(String status) {
    }
}
