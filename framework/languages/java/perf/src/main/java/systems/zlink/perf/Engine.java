package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

public final class Engine implements AutoCloseable {
    public final Config config;
    public final Measurement metrics;
    private final ObjectProvider<ZLinkRouteClient> routes;
    private final ObjectProvider<ZLinkActorClient> actors;
    private final ObjectProvider<ZLinkActorManager> actorManagers;
    private final ObjectProvider<ZLinkSpotManager> spots;
    private final ObjectProvider<ZLinkFanoutClient> fanouts;
    private final ExecutorService drivers=Executors.newVirtualThreadPerTaskExecutor();
    private final AtomicLong[] sequences;
    private final AtomicLong publishSequence=new AtomicLong();
    private volatile boolean prepared;
    public Engine(Config config,Measurement metrics,ObjectProvider<ZLinkRouteClient> routes,ObjectProvider<ZLinkActorClient> actors,
        ObjectProvider<ZLinkActorManager> actorManagers,ObjectProvider<ZLinkSpotManager> spots,ObjectProvider<ZLinkFanoutClient> fanouts){
        this.config=config;this.metrics=metrics;this.routes=routes;this.actors=actors;this.actorManagers=actorManagers;this.spots=spots;this.fanouts=fanouts;
        sequences=new AtomicLong[config.streams()];Arrays.setAll(sequences,i->new AtomicLong());
    }
    public synchronized Map<String,Object> prepare() throws Exception {
        if(prepared)return Measurement.map("ok",true,"state","alreadyPrepared");
        if(!config.source())throw new IllegalStateException("Only the source role runs prepare");
        for(String id:config.list("spotIds")){
            var result=await(spots.getObject().getOrCreate(id,Handlers.SPOT_TYPE).request(ZLinkMessage.of(new PerfCreateRequest("setup"))).submit());
            if(result.spot()==null)throw new Validation("SetupIncomplete","Spot creation rejected");
            metrics.evidence(Measurement.map("kind","publicSpotGetOrCreate","spotId",result.spot().spotId()));
        }
        for(String id:config.list("actorIds")){
            var actor=Handlers.actorRef(await(actorManagers.getObject().getOrCreate(id,Handlers.ACTOR_TYPE).request(ZLinkMessage.of(new PerfCreateRequest("setup"))).submit()));
            metrics.evidence(Measurement.map("kind","publicActorGetOrCreate","actorId",actor.actorId()));
        }
        metrics.objectsReady=true;
        for(int stream=0;stream<config.streams();stream++)execute(metrics.request(stream,sequences[stream].getAndIncrement(),true),true);
        metrics.consumersReady=true;metrics.evidence(Measurement.map("kind","typedProbe","streams",config.streams()));prepared=true;
        return Measurement.map("ok",true,"state","prepared");
    }
    public void run(){
        var jobs=new ArrayList<CompletableFuture<Void>>();
        for(int stream=0;stream<config.streams();stream++)for(int slot=0;slot<config.number("inflight",1);slot++){
            final int id=stream;jobs.add(CompletableFuture.runAsync(()->{
                while(metrics.canIssue()){
                    var request=metrics.request(id,sequences[id].getAndIncrement(),false);
                    long started=metrics.begin();if(started==0)break;
                    boolean operationStarted=true;Throwable failure=null;
                    try{operationStarted=execute(request,false);}
                    catch(Throwable e){failure=e;}
                    metrics.complete(started,failure,operationStarted,config.spotDriver()||config.sendSend()?request.correlationId():null);
                    if(config.spotDriver())metrics.unregister(request.correlationId());
                }
            },drivers));
        }
        CompletableFuture.allOf(jobs.toArray(CompletableFuture[]::new)).join();
    }
    private boolean execute(PerfEchoRequest request,boolean probe) throws Exception {
        if(config.publish()){
            String sequence=Long.toUnsignedString(publishSequence.getAndIncrement());
            var event=new PerfPublishEvent(request.runId(),request.cellId(),request.resetSeq(),request.phase(),sequence,null,"perf.echo",request.sentTicks(),request.clockDomainId(),request.payload());
            if(!probe)metrics.attempt(0,sequence);
            long started=System.nanoTime();await(fanouts.getObject().publish(config.text("channelName"),"perf.echo",event).submit());
            if(!probe)metrics.admitted(new PerfEchoRequest(request.runId(),request.cellId(),request.resetSeq(),request.phase(),0,sequence,request.correlationId(),null,request.sentTicks(),request.clockDomainId(),null,null,request.payload()),started);
            return true;
        }
        if(config.spotDriver()){
            long started=System.nanoTime();metrics.increment("driver.issued");
            var pending=metrics.register(request);
            try{
                var reply=await(routes.getObject().requestToSpot(spotId(request),new PerfDriveRequest(request)).timeout(timeout()).submit(PerfDriveReply.class));
                if(!reply.started())return false;
                if(config.sendSend())metrics.validateReply(request,awaitEcho(request,pending));
                else if(!config.oneWay())metrics.validateReply(request,reply.echo());
                metrics.record("driverLatencyMs",System.nanoTime()-started);return true;
            }finally{if(probe)metrics.unregister(request.correlationId());}
        }
        if(config.sendSend()||config.oneWay()){
            long started=System.nanoTime();if(config.oneWay()&&!probe)metrics.attempt(request.clientId(),request.sequence());var pending=config.sendSend()?metrics.register(request):null;
            try{
                CompletionStage<Void> call=config.scenario().startsWith("actor-")?actors.getObject().sendToActor(actorId(request),request).submit():routes.getObject().sendToSpot(spotId(request),request).submit();
                await(call);if(!probe)metrics.admitted(request,started);
                if(pending!=null)metrics.validateReply(request,awaitEcho(request,pending));return true;
            }finally{if(pending!=null)metrics.unregister(request.correlationId());}
        }
        CompletionStage<PerfEchoReply> call;
        if(config.scenario().startsWith("actor-"))call=actors.getObject().requestToActor(actorId(request),request).timeout(timeout()).submit(PerfEchoReply.class);
        else if(config.scenario().equals("channel-echo-only"))call=routes.getObject().requestToChannel(config.text("channelName"),request).timeout(timeout()).submit(PerfEchoReply.class);
        else call=routes.getObject().requestToSpot(spotId(request),request).timeout(timeout()).submit(PerfEchoReply.class);
        metrics.validateReply(request,await(call));return true;
    }
    public CompletionStage<Void> receiveSend(PerfEchoRequest request,ZLinkSpotOutbound outbound){
        metrics.handlerEnter();metrics.consumersReady=true;
        try{
            if(config.oneWay()){metrics.delivered(request);metrics.handlerExit();return CompletableFuture.completedFuture(null);}
            var reply=metrics.reply(request);
            CompletionStage<Void> result;
            if(request.returnSpotId()!=null)result=routes.getObject().sendToSpot(request.returnSpotId(),reply).submit();
            else if(request.returnChannel()!=null)result=outbound==null?routes.getObject().sendToChannel(request.returnChannel(),reply).submit():outbound.sendToChannel(request.returnChannel(),reply).submit();
            else throw new Validation("IdentityMismatch","Send/send requires return address");
            return result.whenComplete((r,e)->metrics.handlerExit());
        }catch(Throwable failure){metrics.handlerExit();return CompletableFuture.failedFuture(failure);}
    }
    public CompletionStage<PerfDriveReply> drive(Handlers.UserSpot spot,PerfEchoRequest request){
        // Setup probe precedes the measured window; late measured driver work starts no outbound call.
        if(!request.resetSeq().equals("0")&&!metrics.canIssue())return CompletableFuture.completedFuture(new PerfDriveReply(false,null));
        metrics.handlerEnter();metrics.increment("spot.applicationHandlerEntries");metrics.consumersReady=true;
        long started=System.nanoTime();metrics.primaryInterval(request.correlationId(),started,started);
        try{
            metrics.validate(request);
            if(config.oneWay()||config.sendSend()){
                if(config.oneWay()&&!request.resetSeq().equals("0"))metrics.attempt(request.clientId(),request.sequence());
                return spot.context().outbound().sendToChannel(config.text("channelName"),request).submit().thenApply(ignored->{
                    metrics.admitted(request,started);return new PerfDriveReply(true,null);
                }).whenComplete((r,e)->metrics.handlerExit());
            }
            var call=spot.context().outbound().requestToChannel(config.text("channelName"),request).timeout(timeout());
            if(config.yielding())metrics.increment("spot.applicationYieldCalls");
            return(config.yielding()?call.yield(PerfEchoReply.class):call.submit(PerfEchoReply.class)).thenApply(reply->{
                metrics.validateReply(request,reply);long completed=System.nanoTime();metrics.primaryInterval(request.correlationId(),started,completed);return new PerfDriveReply(true,reply);
            }).whenComplete((r,e)->metrics.handlerExit());
        }catch(Throwable failure){metrics.handlerExit();return CompletableFuture.failedFuture(failure);}
    }
    private String spotId(PerfEchoRequest request){return config.list("spotIds").get(request.clientId()%config.list("spotIds").size());}
    private String actorId(PerfEchoRequest request){return config.list("actorIds").get(request.clientId()%config.list("actorIds").size());}
    private PerfEchoReply awaitEcho(PerfEchoRequest request,CompletableFuture<PerfEchoReply> pending)throws Exception{
        try{return pending.get(metrics.correlationRemaining(request.correlationId()),TimeUnit.NANOSECONDS);}catch(TimeoutException timeout){metrics.expired(request.correlationId());throw new Validation("CorrelationExpired","Send/send echo deadline elapsed");}
    }
    private Duration timeout(){return Duration.ofMillis(config.number("requestTimeoutMs",5000));}
    private <T>T await(CompletionStage<T> stage) throws Exception{return stage.toCompletableFuture().get();}
    @Override public void close(){drivers.shutdownNow();}
}
