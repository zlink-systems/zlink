package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import java.net.ServerSocket;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.*;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.spring.*;
import systems.zlink.stream.connector.*;

/** Real public STREAM/Spring regression; no benchmark windows or private runtime adapters. */
public final class SessionPatternCheck {
    private static Path entries;

    public static void main(String[] args) throws Exception {
        if(args.length==2&&args[0].equals("--actor")) {
            var config=Config.read(args[1]);entries=Path.of(config.text("entryFile"));
            try(var context=start(config)) {
                awaitReady(context,false);
                System.out.println("ACTOR_READY");System.out.flush();
                System.in.read();
            }
            return;
        }
        String redis=System.getenv("ZLINK_SESSION_PATTERN_REDIS");
        if(redis==null)throw new IllegalArgumentException("Owned test Redis endpoint is required");
        Path run=Path.of(System.getenv("ZLINK_SESSION_PATTERN_ARTIFACTS"));Files.createDirectories(run);
        Files.writeString(run.resolve("test.pid"),Long.toString(ProcessHandle.current().pid()));
        entries=run.resolve("actor-entries.txt");Files.deleteIfExists(entries);
        baseline(run);
        local(redis,run);
        remote(redis,run);
        System.out.println("SESSION_PATTERN_PASS baseline/local/remote; Actor entries and original typed replies verified");
    }

    private static Config config(String scenario,String role,String endpoint,String mesh,String redis,String namespace,Path entryFile)throws Exception {
        var root=Config.JSON.createObjectNode();
        root.put("runId",namespace).put("cellId",scenario).put("scenario",scenario).put("mode","request")
            .put("role",role).put("objectRole",role.equals("baseline")?"None":role.equals("session")?"ObjectClient":"ObjectServer")
            .put("listenerEndpoint",endpoint).put("meshEndpoint",mesh).put("meshName","session-pattern")
            .put("entryFile",entryFile.toString());
        root.putObject("workload").put("workerPoolSize",8).put("requestTimeoutMs",1000).put("setupTimeoutMs",30000);
        if(redis!=null)root.putObject("store").put("endpoint",redis).put("namespace",namespace);
        return new Config(root);
    }

    private static void baseline(Path run)throws Exception {
        Config config=config("session-echo-only","baseline",endpoint("ws"),"",null,"baseline-"+UUID.randomUUID(),entries);
        try(var context=start(config)) {
            var connector=connector(config.text("listenerEndpoint"));
            try {
            if(!context.getBeansOfType(ZLinkActorManager.class).isEmpty())throw new AssertionError("Baseline unexpectedly has ActorManager");
            awaitReady(context,false);connector.connect().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);
            echo(connector,config,1L);echo(connector,config,2L);
            if(Files.exists(entries))throw new AssertionError("Baseline entered Actor handler");
            }finally{connector.close().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);}
        }
        System.out.println("BASELINE_PASS no ActorManager; two typed echoes");
    }

    private static void local(String redis,Path run)throws Exception {
        Config config=config("cs-local-session-actor-echo","local",endpoint("ws"),endpoint("tcp"),redis,"local-"+UUID.randomUUID(),entries);
        try(var context=start(config)) {
            var connector=connector(config.text("listenerEndpoint"));
            try {
            awaitReady(context,false);connector.connect().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);
            bind(connector,"local-actor");
            echo(connector,config,1L);echo(connector,config,2L);
            assertEntries(List.of("local-actor/1","local-actor/2"));
            }finally{connector.close().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);}
        }
        Files.delete(entries);
        System.out.println("LOCAL_ACTOR_PASS requested Actor handler entered twice; two original typed replies");
    }

    private static void remote(String redis,Path run)throws Exception {
        String namespace="remote-"+UUID.randomUUID();
        Config actor=config("cs-remote-session-actor-echo","actor","",endpoint("tcp"),redis,namespace,entries);
        Config session=config("cs-remote-session-actor-echo","session",endpoint("ws"),endpoint("tcp"),redis,namespace,entries);
        Path actorConfig=run.resolve("actor-config.json");Files.writeString(actorConfig,Config.JSON.writeValueAsString(actor.root()));
        Path actorLog=run.resolve("actor.log");
        Process child=new ProcessBuilder(Path.of(System.getProperty("java.home"),"bin","java").toString(),
            "--enable-native-access=ALL-UNNAMED","-cp",System.getProperty("java.class.path"),SessionPatternCheck.class.getName(),"--actor",actorConfig.toString())
            .redirectErrorStream(true).redirectOutput(actorLog.toFile()).start();
        Files.writeString(run.resolve("actor.pid"),Long.toString(child.pid()));
        Throwable originalFailure=null;
        try(var context=start(session)) {
            var connector=connector(session.text("listenerEndpoint"));
            try {
            awaitReady(context,true);connector.connect().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);
            bind(connector,"remote-actor");
            echo(connector,session,1L);echo(connector,session,2L);
            assertEntries(List.of("remote-actor/1","remote-actor/2"));
            }finally{connector.close().submit().toCompletableFuture().get(5,TimeUnit.SECONDS);}
        } catch(Exception|AssertionError failure) {
            originalFailure=failure;throw failure;
        } finally {
            try {
                try {
                    child.getOutputStream().write('\n');child.getOutputStream().flush();child.getOutputStream().close();
                } finally {
                    boolean exited=child.waitFor(10,TimeUnit.SECONDS);
                    if(!exited){child.destroyForcibly();child.waitFor();}
                    Files.writeString(run.resolve("actor-exit.txt"),Integer.toString(child.exitValue()));
                    if(!exited)throw new AssertionError("Owned Actor JVM failed to stop");
                    if(child.exitValue()!=0)throw new AssertionError("Owned Actor JVM failed: "+child.exitValue());
                }
            }catch(Exception|AssertionError cleanupFailure) {
                if(originalFailure!=null)originalFailure.addSuppressed(cleanupFailure);
                else throw cleanupFailure;
            }
        }
        System.out.println("REMOTE_ACTOR_PASS separate Actor JVM; requested Actor handler entered twice; two original typed replies");
    }

    private static void bind(ZLinkStreamConnector connector,String actorId)throws Exception {
        var reply=connector.request(new PerfBindRequest(actorId)).submit(PerfBindReply.class).toCompletableFuture().get(1,TimeUnit.SECONDS);
        if(!actorId.equals(reply.actorId()))throw new AssertionError("Bound Actor identity differs");
    }
    private static void echo(ZLinkStreamConnector connector,Config config,long sequence)throws Exception {
        PerfEchoRequest request;
        try(var measurement=new Measurement(config)){request=measurement.request(0,sequence,true);}
        var reply=connector.request(request).submit(PerfEchoReply.class).toCompletableFuture().get(1,TimeUnit.SECONDS);
        if(!request.runId().equals(reply.runId())||!request.cellId().equals(reply.cellId())||!request.resetSeq().equals(reply.resetSeq())
            ||!request.phase().equals(reply.phase())||request.clientId()!=reply.clientId()||!request.sequence().equals(reply.sequence())
            ||!request.correlationId().equals(reply.correlationId()))throw new AssertionError("Original typed reply identity differs");
        validatePayload(reply.payload(),4096);
    }
    private static void assertEntries(List<String> expected)throws Exception {
        if(!Files.exists(entries)||!Files.readAllLines(entries).equals(expected))throw new AssertionError("Actual requested Actor business entries differ: "+(Files.exists(entries)?Files.readAllLines(entries):List.of()));
    }
    private static ZLinkStreamConnector connector(String endpoint) {
        var uri=URI.create(endpoint);var defaults=ZLinkStreamConnectorOptions.createDefault(uri);
        var options=new ZLinkStreamConnectorOptions(uri,ZLinkStreamDispatchMode.IMMEDIATE,Duration.ofSeconds(1),Duration.ofSeconds(1),0,
            Duration.ofSeconds(5),defaults.maxSendPayloadSize(),defaults.maxReceivePayloadSize(),defaults.maxReceivedMessages(),
            defaults.maxInboundObserverNotifications(),defaults.maxInboundObserverPayloadPreviewBytes(),false,defaults.heartbeatInterval(),
            defaults.heartbeatTimeout(),false,defaults.reconnectInitialDelay(),defaults.reconnectMaxDelay(),defaults.reconnectBackoffFactor(),
            defaults.skipServerCertificateValidation(),ZLinkStreamCompression.NONE,null,defaults.nameResolver(),defaults.typedCodec(),ZLinkStreamDiagnosticsLevel.OFF);
        return ZLinkStreamConnectorFactory.create(options);
    }
    private static String endpoint(String scheme)throws Exception {
        try(var socket=new ServerSocket(0)){return scheme+"://127.0.0.1:"+socket.getLocalPort();}
    }
    private static AnnotationConfigApplicationContext start(Config config) {
        var context=new AnnotationConfigApplicationContext();
        context.registerBean(Config.class,()->config);context.register(Fixture.class);context.refresh();return context;
    }
    private static void awaitReady(AnnotationConfigApplicationContext context,boolean remote)throws Exception {
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(10);
        while(System.nanoTime()<deadline) {
            var runtime=context.getBean(ZLinkFrameworkRuntime.class);
            boolean ready=runtime.status().isReady();
            if(remote){var mesh=context.getBean(ZLinkRouteMeshRuntime.class).snapshot("session-pattern");ready&=mesh.isReady()&&mesh.readyPeerCount()>0;}
            if(ready)return;
            Thread.sleep(10);
        }
        throw new AssertionError("Required public topology did not become ready within original 10s budget");
    }

    @Configuration(proxyBeanMethods=false)
    @EnableZLinkFramework
    public static class Fixture {
        @Bean Measurement measurement(Config config){return new Measurement(config);}
        @Bean ZLinkFrameworkConfigurer framework(Config config){return options->{
            options.configureNetwork().setBindHost("127.0.0.1");options.configureNetwork().setAdvertiseHost("127.0.0.1");
            options.configureWorkers().minThreads(8).maxThreads(8);
            options.setDefaultRequestTimeout(Duration.ofSeconds(1));
            if(config.objects()) {
                options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
                    .setConnectionString(config.root().path("store").path("endpoint").asText()).setKeyPrefix(config.root().path("store").path("namespace").asText())));
                var mesh=options.addRouteMesh("session-pattern").listen(config.text("meshEndpoint"));
                if(config.objectServer()) {
                    var objects=mesh.objects().server();objects.addEntrySpot(TestEntry.class);
                    objects.addActorFactory(Handlers.ACTOR_TYPE,Handlers.Actor.class,Handlers.ActorFactory.class,factory->factory.disableRelocation());
                }else mesh.objects().client();
            }
            if(!config.text("listenerEndpoint").isEmpty()) {
                var stream=options.addStreamNode("perf-session").bind(config.text("listenerEndpoint")).registerSession(Handlers.Session.class);
                if(config.objects()){stream.enableActorDispatch();stream.addSessionPacketHandler(Handlers.SessionBind.class);}
                else stream.addSessionPacketHandler(Handlers.SessionEcho.class);
            }
        };}
    }
    public static final class TestEntry implements ZLinkEntrySpot<Handlers.Actor> {
        private final ZLinkEntrySpotContext context;
        public TestEntry(ZLinkEntrySpotContext context){this.context=context;}
        @Override public ZLinkEntrySpotContext context(){return context;}
        @Override public CompletionStage<Void> onJoinedActor(Handlers.Actor actor){return Handlers.done(null);}
        @Override public CompletionStage<Void> onLeaveActor(Handlers.Actor actor){return Handlers.done(null);}
        @Override public void configure(){context.handlers().addHandler(CounterRequest.class);}
        @Override public CompletionStage<ZLinkActorCreateResponse> onCreateActor(Handlers.Actor actor,ZLinkMessage request){return Handlers.done(ZLinkActorCreateResponse.accept());}
    }
    public static final class CounterRequest implements ZLinkEntrySpotActorRequestHandler<TestEntry,Handlers.Actor,PerfEchoRequest,PerfEchoReply> {
        private final Measurement metrics;
        public CounterRequest(Measurement metrics){this.metrics=metrics;}
        @Override public CompletionStage<PerfEchoReply> handle(TestEntry spot,Handlers.Actor actor,ZLinkMessageContext context,PerfEchoRequest request) {
            try {
                validatePayload(request.payload(),64);
                Files.writeString(entries,actor.context().actorId()+"/"+request.sequence()+"\n",java.nio.file.StandardOpenOption.CREATE,java.nio.file.StandardOpenOption.APPEND);
                return Handlers.done(metrics.reply(request));
            }catch(java.io.IOException failure){return java.util.concurrent.CompletableFuture.failedFuture(failure);}
        }
    }
}
