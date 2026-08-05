package systems.zlink.samples.gamequest.server.questmission;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.gamequest.server.configuration.SampleLocationStore;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.server.configuration.SampleTopology;
import systems.zlink.samples.gamequest.server.questmission.store.QuestStore;
import systems.zlink.samples.gamequest.server.questmission.spots.PlayerQuestSpot;
import systems.zlink.samples.gamequest.server.questmission.spots.ClosePlayerQuestMsg;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = Program.class)
public class Program {
    public static void main(String[] args) throws Exception {
        ConfigurableApplicationContext app = run(SampleTopology.configPath(args));
        QuestStore store = app.getBean(QuestStore.class);
        ZLinkRouteClient routes = app.getBean(ZLinkRouteClient.class);
        SampleTopology topology = app.getBean(SampleTopology.class);
        HttpServer http = startHttp(store, routes, topology);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            http.stop(0);
            try {
                app.close();
            } catch (Exception ignored) {
            }
        }));
        Thread.currentThread().join();
    }

    public static ConfigurableApplicationContext run(String configPath) {
        StandardEnvironment environment = new StandardEnvironment();
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run();
    }

    @Bean
    ZLinkFrameworkConfigurer questMissionFramework(SampleTopology topology) {
        SampleTopology.QuestMission mission = topology.questMission();
        return options -> {
            options.configureLocations();
            options.addLocationStore(SampleLocationStore.create(topology));
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString(topology.location().redisEndpoint())
                    .setKeyPrefix(topology.location().redisKeyPrefix() + "relocation:")));
            options.addHandlersFromPackageOf(Program.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(mission.logDirectory() + "/flow-" + mission.instanceName() + ".log")
                .traceLabel(mission.instanceName());
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.PlayerQuestSpotDiscovery);
            node.listen(mission.spotRouterEndpoint())
                .setRoutingIdPrefix("gamequest-mission-owner");
            node.objects()
                .server()
                .addInstanceSpotFactory(
                    SampleNames.PlayerQuestSpotType,
                    PlayerQuestSpot.class,
                    factory -> factory.recreateOnRelocation());
        };
    }

    @Bean(destroyMethod = "close")
    QuestStore questStore(SampleTopology topology) {
        QuestStore store = new QuestStore(topology);
        ApplicationContextHolder.store = store;
        return store;
    }

    private static HttpServer startHttp(
        QuestStore store,
        ZLinkRouteClient routes,
        SampleTopology topology) throws IOException {
        ObjectMapper json = new ObjectMapper();
        URI uri = URI.create(topology.questMission().httpEndpoint());
        HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/health", exchange -> writeJson(exchange, json, 200, new Health("ok")));
        server.createContext("/self-check/events", exchange -> writeJson(exchange, json, 200, store.events()));
        server.createContext("/self-check/owner/", exchange -> {
            String path = exchange.getRequestURI().getPath();
            String suffix = path.substring("/self-check/owner/".length());
            String[] parts = suffix.split("/");
            if (parts.length != 2 || !"close".equals(parts[1])) {
                writeJson(exchange, json, 404, new ErrorBody("unknown owner operation"));
                return;
            }
            routes.sendToSpot(parts[0], new ClosePlayerQuestMsg())
                .submit()
                .toCompletableFuture()
                .join();
            writeJson(exchange, json, 202, new OwnerClosed(true));
        });
        server.start();
        return server;
    }

    private static void writeJson(HttpExchange exchange, ObjectMapper json, int status, Object body)
        throws IOException {
        byte[] bytes = json.writeValueAsString(body).getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    private static void writeJsonUnchecked(
        HttpExchange exchange,
        ObjectMapper json,
        int status,
        Object body) {
        try {
            writeJson(exchange, json, status, body);
        } catch (IOException error) {
            throw new java.util.concurrent.CompletionException(error);
        }
    }

    private record Health(String status) {
    }

    private record OwnerClosed(boolean closed) {
    }

    private record ErrorBody(String error) {
    }

    private static final class ApplicationContextHolder {
        private static QuestStore store;
    }
}
