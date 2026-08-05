package systems.zlink.samples.gamequest.server.gameapi;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
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
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.gamequest.server.configuration.SampleLocationStore;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.server.configuration.SampleTimings;
import systems.zlink.samples.gamequest.server.configuration.SampleTopology;
import systems.zlink.samples.gamequest.server.gameapi.sessions.GameQuestSession;
import systems.zlink.samples.gamequest.server.gameapi.actors.GameQuestPlayerActor;
import systems.zlink.samples.gamequest.server.gameapi.actors.GameQuestPlayerActorFactory;
import systems.zlink.samples.gamequest.server.gameapi.spots.GameQuestEntrySpot;
import systems.zlink.samples.gamequest.server.gameapi.store.GameQuestStore;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = Program.class)
public class Program {
    public static void main(String[] args) throws Exception {
        ConfigurableApplicationContext app = run(SampleTopology.configPath(args));
        GameQuestStore store = app.getBean(GameQuestStore.class);
        SampleTopology topology = app.getBean(SampleTopology.class);
        HttpServer http = startHttp(store, topology);
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
    ZLinkFrameworkConfigurer gameApiFramework(SampleTopology topology) {
        SampleTopology.GameApi api = topology.gameApi();
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
                .traceLogFile(api.logDirectory() + "/flow-" + api.instanceName() + ".log")
                .traceLabel(api.instanceName());
            options.addRouteMesh(SampleNames.PlayerQuestSpotDiscovery)
                .setRoutingIdPrefix("gamequest-api")
                .listen(api.spotRouterEndpoint())
                .objects()
                .server()
                .addEntrySpot(GameQuestEntrySpot.class)
                .addActorFactory(
                    SampleNames.PlayerSessionActorType,
                    GameQuestPlayerActor.class,
                    GameQuestPlayerActorFactory.class,
                    factory -> factory.recreateOnRelocation());
            options.addStreamNode(SampleNames.StreamNode)
                .bind(api.streamEndpoint())
                .enableActorDispatch()
                .registerSession(GameQuestSession.class);
        };
    }

    @Bean(destroyMethod = "close")
    GameQuestStore gameQuestStore(SampleTopology topology) {
        GameQuestStore store = new GameQuestStore(topology);
        ApplicationContextHolder.store = store;
        return store;
    }

    @Bean
    GameQuestApiServices gameQuestApiServices(GameQuestStore store, ZLinkRouteClient channels) {
        ApplicationContextHolder.store = store;
        ApplicationContextHolder.channels = channels;
        return new GameQuestApiServices();
    }

    private static HttpServer startHttp(GameQuestStore store, SampleTopology topology) throws IOException {
        ObjectMapper json = new ObjectMapper();
        URI uri = URI.create(topology.gameApi().httpEndpoint());
        HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/health", exchange -> writeJson(exchange, json, 200, new Health("ok")));
        server.createContext("/internal/snapshot", exchange -> {
            Messages.GetGameplaySnapshotReq request = readJson(exchange, json, Messages.GetGameplaySnapshotReq.class);
            writeJson(exchange, json, 200, store.snapshot(request.playerId()));
        });
        server.createContext("/quest/progress/", exchange -> {
            String playerId = exchange.getRequestURI().getPath().substring("/quest/progress/".length());
            writeJson(exchange, json, 200, new Messages.GetQuestProgressRes(store.projection(playerId)));
        });
        server.createContext("/self-check/gameplay/kill-without-publish/", exchange -> {
            String playerId = exchange.getRequestURI().getPath()
                .substring("/self-check/gameplay/kill-without-publish/".length());
            store.addUnpublishedKill(playerId);
            writeJson(exchange, json, 200, new Accepted(true));
        });
        server.createContext("/self-check/projection/", exchange ->
            handleProjection(exchange, json, store, topology).exceptionally(error -> {
                writeJsonUnchecked(exchange, json, 500, new ErrorBody(error.getMessage()));
                return null;
            }));
        server.createContext("/self-check/assert", exchange -> writeJson(exchange, json, 200, store.assertState()));
        server.start();
        return server;
    }

    private static java.util.concurrent.CompletionStage<Void> handleProjection(
        HttpExchange exchange,
        ObjectMapper json,
        GameQuestStore store,
        SampleTopology topology) {
        String[] parts = exchange.getRequestURI().getPath().split("/");
        String playerId = parts.length >= 4 ? parts[3] : "";
        String questId = parts.length >= 5 ? parts[4] : "";
        String action = parts.length >= 6 ? parts[5] : "";
        if ("delete".equals(action)) {
            return ApplicationContextHolder.channels
                .requestToSpot(
                    playerId,
                    new Messages.DeleteQuestProjectionReq(playerId, questId))
                .timeout(SampleTimings.RequestTimeout)
                .instanceSpot(SampleNames.PlayerQuestSpotType)
                .inMesh(SampleNames.PlayerQuestSpotDiscovery)
                .submit(Messages.DeleteQuestProjectionRes.class)
                .thenAccept(deleted -> {
                    store.deleteProjection(playerId, questId);
                    writeJsonUnchecked(exchange, json, 200, deleted);
                });
        }
        if ("rebuild".equals(action)) {
            return ApplicationContextHolder.channels
                .requestToSpot(
                    playerId,
                    new Messages.RebuildQuestProjectionReq(playerId, questId, 0))
                .timeout(SampleTimings.RequestTimeout)
                .instanceSpot(SampleNames.PlayerQuestSpotType)
                .inMesh(SampleNames.PlayerQuestSpotDiscovery)
                .submit(Messages.QuestProgress.class)
                .thenAccept(rebuilt -> {
                    store.mergeProjection(playerId, java.util.List.of(rebuilt));
                    writeJsonUnchecked(exchange, json, 200, rebuilt);
                });
        }
        writeJsonUnchecked(exchange, json, 404, new ErrorBody("unknown projection action"));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
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

    private static <T> T readJson(HttpExchange exchange, ObjectMapper json, Class<T> type) throws IOException {
        return json.readValue(exchange.getRequestBody(), type);
    }

    private static void writeJson(HttpExchange exchange, ObjectMapper json, int status, Object body)
        throws IOException {
        byte[] bytes = json.writeValueAsString(body).getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    private record Health(String status) {
    }

    private record Accepted(boolean accepted) {
    }

    private record Deleted(boolean deleted) {
    }

    private record ErrorBody(String error) {
    }

    private static final class GameQuestApiServices {
    }

    private static final class ApplicationContextHolder {
        private static GameQuestStore store;
        private static ZLinkRouteClient channels;
    }
}
