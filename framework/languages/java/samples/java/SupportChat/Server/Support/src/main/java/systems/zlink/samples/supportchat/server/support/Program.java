package systems.zlink.samples.supportchat.server.support;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import systems.zlink.contracts.core.RoutingId;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.supportchat.server.configuration.SampleFlowLog;
import systems.zlink.samples.supportchat.server.configuration.SampleLocationStore;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTopology;
import systems.zlink.samples.supportchat.server.support.actors.SupportActorDirectory;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActorFactory;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActorRelocationAdapter;
import systems.zlink.samples.supportchat.server.support.application.AgentAssignmentService;
import systems.zlink.samples.supportchat.server.support.application.ConversationAllocator;
import systems.zlink.samples.supportchat.server.support.application.ConversationSpotFactory;
import systems.zlink.samples.supportchat.server.support.infrastructure.FrameworkConversationSpotFactory;
import systems.zlink.samples.supportchat.server.support.infrastructure.ConversationNotificationPublisher;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.supportchat.server.support.spots.conversationspot.ConversationSpot;
import systems.zlink.samples.supportchat.server.support.spots.entryspot.SupportEntrySpot;

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
    ZLinkFrameworkConfigurer supportFramework(SampleTopology topology) {
        SampleTopology.Support support = topology.support();
        URI channelEndpoint = URI.create(support.channelEndpoint());
        return options -> {
            options.configureLocations();
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString(topology.location().redisEndpoint())
                    .setKeyPrefix(topology.location().redisKeyPrefix() + "relocation:")));
            options.addHandlersFromPackageOf(Program.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(SampleFlowLog.path(topology, "support"))
                .traceLabel("support");
            options.addClientServerChannel(SampleNames.ApiChannel)
                .client();
            options.addClientServerChannel(SampleNames.SupportChannel)
                .server()
                .setBindHost(channelEndpoint.getHost())
                .setAdvertiseHost(channelEndpoint.getHost())
                .listen(channelEndpoint.getPort())
                .addHandlerGroup(SampleNames.SupportChannel);
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.SupportActorMesh);
            node.listen(support.routerEndpoint())
                .setRoutingIdPrefix("support-owner");
            node.objects()
                .server()
                .addEntrySpot(SupportEntrySpot.class)
                .addActorFactory(
                    SampleNames.SupportActorType,
                    SupportUserActor.class,
                    SupportUserActorFactory.class,
                    factory -> factory.preserveStateWith(
                        SupportUserActorRelocationAdapter.class))
                .addSpotFactory(
                    SampleNames.ConversationSpotType,
                    ConversationSpot.class,
                    factory -> factory.disableRelocation());
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean
    AgentAssignmentService agentAssignmentService() {
        return new AgentAssignmentService(SampleNames.AgentCapacity);
    }

    @Bean
    ConversationSpotFactory conversationSpotFactory(ZLinkSpotManager spots) {
        return new FrameworkConversationSpotFactory(spots);
    }

    @Bean
    ConversationAllocator conversationAllocator(ConversationSpotFactory spots) {
        return new ConversationAllocator(spots);
    }

    @Bean
    ConversationNotificationPublisher conversationNotificationPublisher() {
        return new ConversationNotificationPublisher();
    }

    @Bean
    SupportActorDirectory supportActorDirectory() {
        return new SupportActorDirectory();
    }

    @Bean(destroyMethod = "close")
    AutoCloseable supportHttpServer(SampleTopology topology) throws IOException {
        ObjectMapper json = new ObjectMapper();
        URI uri = URI.create(topology.support().httpEndpoint());
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
