package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import java.util.concurrent.*;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.*;

/** Independent typed echo application, registered using the public Framework contracts. */
public final class Handlers {
    private Handlers() {}
    public static final String SPOT_TYPE="framework-perf-user-spot", ACTOR_TYPE="framework-perf-actor";
    static <T> CompletionStage<T> done(T value){return CompletableFuture.completedFuture(value);}

    public static final class ChannelRequest implements ZLinkRequestHandler<PerfEchoRequest,PerfEchoReply> {
        private final Engine engine;
        public ChannelRequest(Engine engine){this.engine=engine;}
        @Override public CompletionStage<PerfEchoReply> handle(PerfEchoRequest request,ZLinkMessageContext context){
            engine.metrics.handlerEnter();try{engine.metrics.consumersReady=true;return done(engine.metrics.reply(request));}finally{engine.metrics.handlerExit();}}
    }
    public static final class ChannelSend implements ZLinkSendHandler<PerfEchoRequest> {
        private final Engine engine;
        public ChannelSend(Engine engine){this.engine=engine;}
        @Override public CompletionStage<Void> handle(PerfEchoRequest request,ZLinkMessageContext context){
            return engine.receiveSend(request,null);}
    }
    public static final class ChannelReturn implements ZLinkSendHandler<PerfEchoReply> {
        private final Engine engine;
        public ChannelReturn(Engine engine){this.engine=engine;}
        @Override public CompletionStage<Void> handle(PerfEchoReply reply,ZLinkMessageContext context){engine.metrics.returned(reply);return done(null);}
    }
    public static final class Fanout implements ZLinkFanoutHandler<PerfPublishEvent> {
        private final Measurement metrics;
        public Fanout(Measurement metrics){this.metrics=metrics;}
        @Override public CompletionStage<Void> handle(PerfPublishEvent event,ZLinkPublishMessageContext context){
            metrics.handlerEnter();try{metrics.delivered(event);return done(null);}finally{metrics.handlerExit();}}
    }
    public static final class UserSpot implements ZLinkSpot<Actor> {
        private final ZLinkSpotContext context;
        private final Measurement metrics;
        public UserSpot(ZLinkSpotContext context,Measurement metrics){this.context=context;this.metrics=metrics;}
        @Override public ZLinkSpotContext context(){return context;}
        @Override public CompletionStage<Void> onJoinedActor(Actor actor){return done(null);}
        @Override public CompletionStage<Void> onLeaveActor(Actor actor){return done(null);}
        @Override public void configure(){
            if(metrics.config.spotDriver()){
                context.handlers().addHandler(SpotDrive.class);
                if(metrics.config.sendSend())context.handlers().addHandler(SpotReturn.class);
            }else if(metrics.config.oneWay()||metrics.config.sendSend())context.handlers().addHandler(SpotSend.class);
            else context.handlers().addHandler(SpotRequest.class);
        }
        @Override public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request){metrics.objectsReady=true;return done(ZLinkSpotCreateResponse.accept());}
    }
    public static final class SpotRequest implements ZLinkSpotRequestHandler<UserSpot,PerfEchoRequest,PerfEchoReply> {
        private final Engine engine;
        public SpotRequest(Engine engine){this.engine=engine;}
        @Override public CompletionStage<PerfEchoReply> handle(UserSpot spot,PerfEchoRequest request){
            engine.metrics.handlerEnter();engine.metrics.increment("spot.applicationHandlerEntries");engine.metrics.consumersReady=true;
            try {
                engine.metrics.validate(request);
                if(!engine.config.worker())return done(engine.metrics.reply(request)).whenComplete((r,e)->engine.metrics.handlerExit());
                final int millis=engine.config.number("workerTaskMillis",5);
                long submitted=System.nanoTime();
                var call=spot.context().runCpuWorker(cancellation->{
                    long started=System.nanoTime(),iterations=0;int x=0x12345678;
                    do {for(int i=0;i<1024;i++){x^=x<<13;x^=x>>>17;x^=x<<5;iterations++;}cancellation.throwIfCancellationRequested();}
                    while(System.nanoTime()-started<millis*1_000_000L);
                    return new WorkerObservation(Long.toString(started),Measurement.nowText(),Measurement.DOMAIN,Long.toString(iterations),Integer.toUnsignedLong(x));
                }).timeout(java.time.Duration.ofMillis(engine.config.number("requestTimeoutMs",5000)));
                if(engine.config.yielding())engine.metrics.increment("spot.applicationYieldCalls");
                return (engine.config.yielding()?call.yield():call.submit()).thenApply(observation->{
                    long continued=System.nanoTime(),started=Long.parseLong(observation.startedTicks()),ended=Long.parseLong(observation.endedTicks());
                    engine.metrics.worker(observation);
                    engine.metrics.record("workerCallLatencyMs",continued-submitted);
                    engine.metrics.record("workerSubmitToStartMs",started-submitted);
                    engine.metrics.record("workerTaskLatencyMs",ended-started);
                    engine.metrics.record("workerResultToContinuationMs",continued-ended);
                    return engine.metrics.reply(request);
                }).whenComplete((r,e)->engine.metrics.handlerExit());
            }catch(Throwable failure){engine.metrics.handlerExit();return CompletableFuture.failedFuture(failure);}
        }
    }
    public static final class SpotSend implements ZLinkSpotPacketHandler<UserSpot,PerfEchoRequest> {
        private final Engine engine;
        public SpotSend(Engine engine){this.engine=engine;}
        @Override public CompletionStage<Void> handle(UserSpot spot,PerfEchoRequest request){return engine.receiveSend(request,spot.context().outbound());}
    }
    public static final class SpotReturn implements ZLinkSpotPacketHandler<UserSpot,PerfEchoReply> {
        private final Measurement metrics;
        public SpotReturn(Measurement metrics){this.metrics=metrics;}
        @Override public CompletionStage<Void> handle(UserSpot spot,PerfEchoReply reply){metrics.returned(reply);return done(null);}
    }
    public static final class SpotDrive implements ZLinkSpotRequestHandler<UserSpot,PerfDriveRequest,PerfDriveReply> {
        private final Engine engine;
        public SpotDrive(Engine engine){this.engine=engine;}
        @Override public CompletionStage<PerfDriveReply> handle(UserSpot spot,PerfDriveRequest request){return engine.drive(spot,request.echo());}
    }
    public static final class Actor implements ZLinkActor {
        private final ZLinkActorContext context;
        public Actor(ZLinkActorContext context){this.context=context;}
        @Override public ZLinkActorContext context(){return context;}
    }
    public static final class ActorFactory implements ZLinkActorFactory {
        @Override public CompletionStage<ZLinkActor> create(ZLinkActorContext context){return done(new Actor(context));}
    }
    public static final class EntrySpot implements ZLinkEntrySpot<Actor> {
        private final ZLinkEntrySpotContext context;
        private final Measurement metrics;
        public EntrySpot(ZLinkEntrySpotContext context,Measurement metrics){this.context=context;this.metrics=metrics;}
        @Override public ZLinkEntrySpotContext context(){return context;}
        @Override public CompletionStage<Void> onJoinedActor(Actor actor){return done(null);}
        @Override public CompletionStage<Void> onLeaveActor(Actor actor){return done(null);}
        @Override public void configure(){context.handlers().addHandler(ActorRequest.class);context.handlers().addHandler(ActorSend.class);}
        @Override public CompletionStage<ZLinkActorCreateResponse> onCreateActor(Actor actor,ZLinkMessage request){metrics.objectsReady=true;return done(ZLinkActorCreateResponse.accept());}
    }
    public static final class ActorRequest implements ZLinkEntrySpotActorRequestHandler<EntrySpot,Actor,PerfEchoRequest,PerfEchoReply> {
        private final Measurement metrics;
        public ActorRequest(Measurement metrics){this.metrics=metrics;}
        @Override public CompletionStage<PerfEchoReply> handle(EntrySpot spot,Actor actor,ZLinkMessageContext context,PerfEchoRequest request){
            metrics.handlerEnter();try{metrics.consumersReady=true;return done(metrics.reply(request));}finally{metrics.handlerExit();}}
    }
    public static final class ActorSend implements ZLinkEntrySpotActorSendHandler<EntrySpot,Actor,PerfEchoRequest> {
        private final Engine engine;
        public ActorSend(Engine engine){this.engine=engine;}
        @Override public CompletionStage<Void> handle(EntrySpot spot,Actor actor,ZLinkMessageContext context,PerfEchoRequest request){return engine.receiveSend(request,null);}
    }
    public static final class Session implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher;
        private final Measurement metrics;
        public Session(ZLinkSessionContext context,ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher,Measurement metrics){this.context=context;this.dispatcher=dispatcher;this.metrics=metrics;}
        @Override public ZLinkSessionContext context(){return context;}
        @Override public CompletionStage<Void> onConnected(){return done(null);}
        @Override public CompletionStage<Void> onDisconnected(){return done(null);}
        @Override public CompletionStage<Void> onError(ZLinkStreamError error){metrics.diagnostic(new IllegalStateException(error.toString()));return done(null);}
        @Override public CompletionStage<Void> onDispatch(ZLinkSessionDispatchContext dispatch,ZLinkMessage payload){
            if(!metrics.config.objects()||dispatch.packetName().equals(PerfBindRequest.class.getSimpleName()))
                return dispatcher.tryHandle(context,dispatch,payload).thenCompose(handled->handled?done(null):
                    CompletableFuture.failedFuture(new Validation("SetupIncomplete","Session needs exactly one bound Actor")));
            var bound=context.actors().bound();
            if(bound.size()!=1)return CompletableFuture.failedFuture(new Validation("SetupIncomplete","Session needs exactly one bound Actor"));
            metrics.handlerEnter();metrics.consumersReady=true;
            return bound.get(0).relay(dispatch,payload).whenComplete((r,e)->metrics.handlerExit());
        }
    }
    public static final class SessionEcho implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext,PerfEchoRequest> {
        private final Measurement metrics;
        public SessionEcho(Measurement metrics){this.metrics=metrics;}
        @Override public Class<PerfEchoRequest> messageType(){return PerfEchoRequest.class;}
        @Override public CompletionStage<Void> handle(ZLinkSessionContext session,ZLinkSessionDispatchContext dispatch,PerfEchoRequest request){
            metrics.handlerEnter();metrics.consumersReady=true;
            try {
                return session.client().reply(metrics.reply(request)).submit().whenComplete((r,e)->metrics.handlerExit());
            }catch(Throwable failure){metrics.handlerExit();return CompletableFuture.failedFuture(failure);}
        }
    }
    public static final class SessionBind implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext,PerfBindRequest> {
        private final ObjectProvider<ZLinkActorManager> actors;
        public SessionBind(ObjectProvider<ZLinkActorManager> actors){this.actors=actors;}
        @Override public Class<PerfBindRequest> messageType(){return PerfBindRequest.class;}
        @Override public CompletionStage<Void> handle(ZLinkSessionContext context,ZLinkSessionDispatchContext dispatch,PerfBindRequest request){
            return actors.getObject().getOrCreate(request.actorId(),ACTOR_TYPE).request(ZLinkMessage.of(new PerfCreateRequest("setup"))).submit()
                .thenCompose(result->context.actors().bindOrGet(actorRef(result)))
                .thenCompose(bound->context.client().reply(new PerfBindReply(bound.actorId())).submit());
        }
    }
    static ActorRef actorRef(ZLinkActorCreateResult result){
        if(result instanceof ZLinkActorCreateResult.Created created)return created.actor();
        if(result instanceof ZLinkActorCreateResult.Existing existing)return existing.actor();
        throw new Validation("SetupIncomplete","Actor creation rejected");
    }
}
