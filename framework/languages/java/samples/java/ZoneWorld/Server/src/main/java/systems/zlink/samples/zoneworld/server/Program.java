package systems.zlink.samples.zoneworld.server;

import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.Flow;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.zoneworld.server.configuration.MaintenanceStore;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.configuration.NodeRegistry;
import systems.zlink.samples.zoneworld.server.configuration.SampleLocationStore;
import systems.zlink.samples.zoneworld.server.configuration.SampleTopology;
import systems.zlink.samples.zoneworld.server.gateway.GameSession;
import systems.zlink.samples.zoneworld.server.ops.OpsSession;
import systems.zlink.samples.zoneworld.server.zone.ZoneBootstrap;
import systems.zlink.samples.zoneworld.server.zone.ZoneStatusReporter;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActorFactory;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActorRelocationAdapter;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneEntrySpot;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(proxyBeanMethods = false, scanBasePackages = "systems.zlink.samples.zoneworld.server")
public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ConfigurableApplicationContext app = run(SampleTopology.configPath(args));
        app.getBean(SampleTopology.class).validateServer();
        Thread.currentThread().join();
    }

    private static ConfigurableApplicationContext run(String configPath) {
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
    ZLinkRedisLocationStore locationStore(SampleTopology topology) {
        return SampleLocationStore.create(topology);
    }

    @Bean(destroyMethod = "close")
    ZLinkRedisRelocationStore relocationStore(SampleTopology topology) {
        return new ZLinkRedisRelocationStore(
            new ZLinkRedisRelocationOptions()
                .setConnectionString(topology.redisEndpoint())
                .setKeyPrefix(topology.redisKeyPrefix() + "relocation:")
                .setCommandTimeout(Duration.ofMillis(500)));
    }

    @Bean(destroyMethod = "close")
    MaintenanceStore maintenanceStore(SampleTopology topology) {
        return new MaintenanceStore(topology.redisEndpoint(), topology.redisKeyPrefix());
    }

    @Bean
    NodeMaintenanceState maintenanceState() {
        return new NodeMaintenanceState();
    }

    @Bean
    NodeCensus nodeCensus() {
        return new NodeCensus();
    }

    @Bean
    NodeRegistry nodeRegistry() {
        return new NodeRegistry();
    }

    @Bean
    ZLinkFrameworkConfigurer zoneWorldFramework(
        SampleTopology topology,
        ZLinkRedisLocationStore locations,
        ZLinkRedisRelocationStore relocationStore) {
        topology.validateServer();
        return options -> {
            options.configureLocations();
            options.addLocationStore(locations);
            options.addRelocationStore(relocationStore);
            options.addHandlersFromPackageOf(Program.class);
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);

            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(ZoneWorldNames.MESH)
                .listen(topology.meshEndpoint())
                .setRoutingIdPrefix("zoneworld-" + topology.role());

            if (topology.is("gateway")) {
                mesh.objects().client();
                options.addStreamNode(ZoneWorldNames.GATEWAY_STREAM)
                    .bind(topology.streamEndpoint())
                    .enableActorDispatch()
                    .registerSession(GameSession.class);
                return;
            }

            if (topology.is("zone")) {
                mesh.channelName(ZoneWorldNames.ZONE_CHANNEL)
                    .server()
                    .addHandlerGroup(ZoneWorldNames.ZONE_CHANNEL);
                mesh.channelName(ZoneWorldNames.REPORT_CHANNEL).client();
                mesh.objects().server()
                    .addEntrySpot(ZoneEntrySpot.class)
                    .addSpotFactory(
                        ZoneWorldNames.ZONE_SPOT_TYPE,
                        ZoneSpot.class,
                        factory -> factory.stableTypeLimit(2).disableRelocation())
                    .addActorFactory(
                        ZoneWorldNames.PLAYER_ACTOR_TYPE,
                        PlayerActor.class,
                        PlayerActorFactory.class,
                        factory -> factory.preserveStateWith(PlayerActorRelocationAdapter.class));
                options.addFanoutChannel(ZoneWorldNames.BROADCAST_CHANNEL)
                    .enableSubscriber()
                    .addHandlerGroup(ZoneWorldNames.BROADCAST_HANDLER_GROUP);
                return;
            }

            mesh.channelName(ZoneWorldNames.REPORT_CHANNEL)
                .server()
                .addHandlerGroup(ZoneWorldNames.OPS_HANDLER_GROUP);
            mesh.objects().client();
            options.addFanoutChannel(ZoneWorldNames.BROADCAST_CHANNEL)
                .setRoutingIdPrefix("zoneworld-ops-broadcast")
                .enablePublisher();
            options.addStreamNode(ZoneWorldNames.OPS_STREAM)
                .bind(topology.streamEndpoint())
                .enableActorDispatch()
                .registerSession(OpsSession.class);
        };
    }

    @Bean
    ApplicationRunner runtimeEvidence(
        ObjectProvider<ZLinkRouteMeshRuntime> runtimeProvider,
        ObjectProvider<ZLinkFrameworkRuntime> frameworkProvider,
        SampleTopology topology) {
        return ignored -> {
            ZLinkFrameworkRuntime framework = frameworkProvider.getIfAvailable();
            if (framework != null) {
                System.out.println("framework lifecycle role=" + topology.role()
                    + " state=" + framework.status().state());
            }
            ZLinkRouteMeshRuntime runtime = runtimeProvider.getIfAvailable();
            if (runtime == null) return;
            runtime.observe(ZoneWorldNames.MESH, 32).subscribe(
                new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                    @Override
                    public void onSubscribe(Flow.Subscription subscription) {
                        subscription.request(Long.MAX_VALUE);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                        ZLinkMeshNodeSnapshot status = observed.status();
                        System.out.println("runtime event mesh=" + status.meshName()
                            + " state=" + status.state()
                            + " readyPeers=" + status.readyPeerCount());
                    }

                    @Override
                    public void onError(Throwable error) {
                        System.out.println("runtime event error mesh=" + ZoneWorldNames.MESH
                            + " detail=" + error.getMessage());
                    }

                    @Override
                    public void onComplete() {
                    }
                });
        };
    }

    @Bean
    @ConditionalOnProperty(prefix = "sample", name = "role", havingValue = "zone")
    ZoneBootstrap zoneBootstrap(
        SampleTopology topology,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        NodeMaintenanceState maintenance,
        MaintenanceStore store) {
        return new ZoneBootstrap(topology, spots, actors, maintenance, store);
    }

    @Bean
    @ConditionalOnProperty(prefix = "sample", name = "role", havingValue = "zone")
    ZoneStatusReporter zoneStatusReporter(
        SampleTopology topology,
        ZLinkRouteClient routes,
        NodeCensus census,
        NodeMaintenanceState maintenance) {
        return new ZoneStatusReporter(topology, routes, census, maintenance);
    }
}
