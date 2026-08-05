package systems.zlink.samples.supportchat.server.session;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.supportchat.server.configuration.SampleFlowLog;
import systems.zlink.samples.supportchat.server.configuration.SampleLocationStore;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTopology;
import systems.zlink.samples.supportchat.server.session.sessions.SupportChatSession;

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
    ZLinkFrameworkConfigurer sessionFramework(SampleTopology topology) {
        SampleTopology.Session session = topology.session();
        return options -> {
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(SampleFlowLog.path(topology, "session"))
                .traceLabel("session");
            options.configureLocations();
            options.addClientServerChannel(SampleNames.ApiChannel)
                .client();
            options.addClientServerChannel(SampleNames.SupportChannel)
                .client();
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.SupportActorMesh);
            node.listen(session.routerEndpoint())
                .setRoutingIdPrefix("support-session");
            node.objects().client();
            options.addStreamNode(SampleNames.StreamNode)
                .bind(session.streamEndpoint())
                .enableActorDispatch()
                .registerSession(SupportChatSession.class);
        };
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }
}
