package systems.zlink.perf;

import java.net.URI;
import java.time.Duration;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.*;
import systems.zlink.framework.configuration.*;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.monitoring.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.spring.*;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods=false)
public class Program {
    public static void main(String[] args) throws Exception {
        if(args.length==4&&args[0].equals("--endpoint-config")&&args[2].equals("--client-index")){Client.run(args[1],Integer.parseInt(args[3]));return;}
        if(args.length!=2||!args[0].equals("--config"))throw new IllegalArgumentException("--config <role.json> or --endpoint-config <manifest.json> --client-index <index>");
        Config config=Config.read(args[1]);
        var builder=new SpringApplicationBuilder(Program.class).web(WebApplicationType.NONE)
            .properties("spring.main.banner-mode=off","logging.level.root=WARN")
            .initializers(context->context.getBeanFactory().registerSingleton("perfConfig",config));
        if(config.root().path("diagnostics").isObject())builder.properties("logging.level.root=INFO","logging.file.name="+config.root().path("diagnostics").path("flowFile").asText());
        builder.application().setKeepAlive(true);
        var context=builder.run();
        var metrics=context.getBean(Measurement.class);
        var runtime=context.getBean(ZLinkFrameworkRuntime.class);
        metrics.publicStatus=()->{
            var status=runtime.status();
            return Measurement.map("state",status.state().toString(),"isReady",status.isReady(),"acceptingWork",status.acceptingWork(),
                "capacity",Measurement.map("measurementEpoch",Long.toUnsignedString(status.capacity().measurementEpoch()),
                    "coreHwm",Measurement.map("effectiveBudgetBytes",Long.toUnsignedString(status.capacity().coreHwm().effectiveBudgetBytes()),"currentAccountedBytes",Long.toUnsignedString(status.capacity().coreHwm().currentAccountedBytes()),"peakAccountedBytes",Long.toUnsignedString(status.capacity().coreHwm().peakAccountedBytes())),
                    "applicationJobQueue",Measurement.map("pressureState",status.capacity().applicationJobQueue().pressureState().toString(),"queuedApplicationJobs",Long.toUnsignedString(status.capacity().applicationJobQueue().queuedApplicationJobs()),"effectiveMaxQueuedApplicationJobs",Long.toUnsignedString(status.capacity().applicationJobQueue().effectiveMaxQueuedApplicationJobs()),"permitsInUse",Long.toUnsignedString(status.capacity().applicationJobQueue().permitsInUse()),"peakPermitsInUse",Long.toUnsignedString(status.capacity().applicationJobQueue().peakPermitsInUse()))));
        };
        metrics.capacityReset=()->{runtime.resetCapacityMetrics();return Long.toUnsignedString(runtime.status().capacity().measurementEpoch());};
        metrics.objectsReady=!config.objectServer();
        Admin admin=new Admin(config,metrics,context.getBean(Engine.class),()->{
            boolean ready=runtime.status().isReady();
            if(config.publish()&&!config.source())ready&=context.getBean(ZLinkFanoutRuntime.class).snapshot(config.text("channelName")).isReady();
            else if(config.text("topology").equals("clientserver"))ready&=context.getBean(ZLinkClientServerRuntime.class).isReady(config.text("channelName"));
            else if(!config.publish()&&!(config.cs()&&!config.objects())&&!config.listener().isEmpty()&&!config.text("meshName").isEmpty())ready&=context.getBean(ZLinkRouteMeshRuntime.class).isReady(config.text("meshName"));
            return ready;
        });
        admin.start();Runtime.getRuntime().addShutdownHook(new Thread(admin::close));
    }
    @Bean Measurement measurement(Config config){return new Measurement(config);}
    @Bean Engine engine(Config config,Measurement measurement,ObjectProvider<ZLinkRouteClient> routes,ObjectProvider<ZLinkActorClient> actors,
        ObjectProvider<ZLinkActorManager> actorManagers,ObjectProvider<ZLinkSpotManager> spots,ObjectProvider<ZLinkFanoutClient> fanouts){
        return new Engine(config,measurement,routes,actors,actorManagers,spots,fanouts);
    }
    @Bean ZLinkFrameworkConfigurer framework(Config config){return options->{
        options.setDefaultRequestTimeout(Duration.ofMillis(config.number("requestTimeoutMs",5000)));
        options.configureNetwork().setBindHost("127.0.0.1");options.configureNetwork().setAdvertiseHost("127.0.0.1");
        options.configureDispatch().messageFlow(config.root().path("diagnostics").isObject()?ZLinkMessageFlowLogMode.NORMAL:ZLinkMessageFlowLogMode.OFF);
        options.configureWorkers().minThreads(config.number("workerPoolSize",8)).maxThreads(config.number("workerPoolSize",8)).idleTimeout(Duration.ofSeconds(60));
        if(config.root().path("store").isObject()){
            var store=config.root().path("store");
            options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions().setConnectionString(store.path("endpoint").asText()).setKeyPrefix(store.path("namespace").asText())));
        }
        if(config.publish()){
            var fanout=options.addFanoutChannel(config.text("channelName"));
            if(config.source())fanout.enablePublisher(config.text("fanoutEndpoint")).setRoutingIdPrefix(config.text("channelName"));
            else {fanout.enableSubscriber();fanout.addPublishHandler(Handlers.Fanout.class,Contracts.PerfPublishEvent.class);}
            return;
        }
        if(config.cs()&&(config.text("listenerEndpoint").startsWith("ws://")||config.text("listenerEndpoint").startsWith("wss://"))){
            var stream=options.addStreamNode("perf-session").bind(config.text("listenerEndpoint")).registerSession(Handlers.Session.class);
            if(config.objects()){stream.enableActorDispatch();stream.addSessionPacketHandler(Handlers.SessionBind.class);}
            else stream.addSessionPacketHandler(Handlers.SessionEcho.class);
        }
        if(config.text("topology").equals("clientserver")){
            var channel=options.addClientServerChannel(config.text("channelName"));
            if(config.source())channel.client().connect(config.text("peerEndpoint"));
            else channel.server().listen(URI.create(config.text("listenerEndpoint")).getPort()).setBindHost("127.0.0.1").addRequestHandler(Handlers.ChannelRequest.class,Contracts.PerfEchoRequest.class,Contracts.PerfEchoReply.class);
            return;
        }
        if(!config.objects()&&config.cs())return;
        var mesh=options.addRouteMesh(config.text("meshName")).listen(config.listener());
        mesh.configureRouterSocket().setSendTimeout(Duration.ofMillis(config.number("socketSendTimeoutMs",5000)));
        if(!config.objects()){
            for(String peer:config.list("peerEndpoints"))mesh.peerConnections().connect(peer);
            if(config.list("peerEndpoints").isEmpty()&&!config.text("peerEndpoint").isEmpty())mesh.peerConnections().connect(config.text("peerEndpoint"));
        }
        boolean targetChannel=config.scenario().equals("channel-echo-only")?!config.source():config.spotDriver()&&!config.source();
        if(targetChannel){
            var server=mesh.channel(config.text("channelName")).server();
            server.addRequestHandler(Handlers.ChannelRequest.class,Contracts.PerfEchoRequest.class,Contracts.PerfEchoReply.class);
            server.addSendHandler(Handlers.ChannelSend.class,Contracts.PerfEchoRequest.class);
        }else if(config.spotDriver()||config.scenario().equals("channel-echo-only"))mesh.channel(config.text("channelName")).client();
        if(config.sendSend()&&!config.spotDriver()){
            if(config.source())mesh.channel(config.returnChannel()).server().addSendHandler(Handlers.ChannelReturn.class,Contracts.PerfEchoReply.class);
            else mesh.channel(config.returnChannel()).client();
        }
        if(config.objects()){
            if(config.objectServer()){
                var objects=mesh.objects().server();
                if(!config.list("actorIds").isEmpty()||config.cs()){
                    objects.addEntrySpot(Handlers.EntrySpot.class);
                    objects.addActorFactory(Handlers.ACTOR_TYPE,Handlers.Actor.class,Handlers.ActorFactory.class,factory->factory.disableRelocation());
                }
                if(!config.list("spotIds").isEmpty())objects.addSpotFactory(Handlers.SPOT_TYPE,Handlers.UserSpot.class,factory->{factory.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE);factory.disableRelocation();});
            }else mesh.objects().client();
        }
    };}
}
