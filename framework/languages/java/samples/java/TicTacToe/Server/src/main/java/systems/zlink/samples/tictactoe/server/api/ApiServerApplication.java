package systems.zlink.samples.tictactoe.server.api;
import java.net.InetAddress;
import java.net.URI;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.boot.web.server.WebServerFactoryCustomizer;
import org.springframework.boot.web.servlet.server.ConfigurableServletWebServerFactory;
import org.springframework.core.env.StandardEnvironment;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.ApplicationListener;
import org.springframework.boot.web.context.WebServerInitializedEvent;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.samples.tictactoe.server.configuration.SampleLocationStore;
import systems.zlink.samples.tictactoe.server.api.handlers.AuthenticatePlayerHandler;
import systems.zlink.samples.tictactoe.server.api.handlers.CreateGameHttpHandler;
import systems.zlink.samples.tictactoe.server.configuration.ApiSettings;
import systems.zlink.samples.tictactoe.server.configuration.TicTacToeReadinessReporter;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;



@EnableZLinkFramework
@EnableConfigurationProperties(ApiSettings.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = {
        ApiServer.class,
        AuthenticatePlayerHandler.class,
        CreateGameHttpHandler.class
    })
public final class ApiServerApplication {
    private ApiServerApplication() {
    }

    public static ConfigurableApplicationContext run(String configPath) {
        StandardEnvironment environment = new StandardEnvironment();
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(ApiServerApplication.class)
            .environment(environment)
            .web(WebApplicationType.SERVLET)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri());
        builder.application().setKeepAlive(true);
        return builder.run(new String[0]);
    }

    @Bean
    ZLinkFrameworkConfigurer apiFramework(ApiSettings settings) {
        return ApiServer.configure(settings);
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(ApiSettings settings) {
        return SampleLocationStore.create(settings);
    }

    @Bean
    WebServerFactoryCustomizer<ConfigurableServletWebServerFactory> apiHttpServer(ApiSettings settings) {
        return server -> {
            var endpoint = URI.create(settings.apiBindUrl());
            server.setAddress(InetAddress.getLoopbackAddress());
            server.setPort(endpoint.getPort());
        };
    }

    @Bean
    ApplicationListener<WebServerInitializedEvent> ticTacToeHttpReadiness(ApiSettings settings) {
        return event -> LoggerFactory.getLogger(ApiServerApplication.class)
            .info("tictactoe-ready kind=http node={}", settings.nodeId());
    }

    @Bean(destroyMethod = "close")
    TicTacToeReadinessReporter ticTacToeSpotRouteReadiness(
        ApiSettings settings,
        ZLinkRouteMeshRuntime meshes) {
        return TicTacToeReadinessReporter.spotRoute(settings.nodeId(), meshes);
    }
}
