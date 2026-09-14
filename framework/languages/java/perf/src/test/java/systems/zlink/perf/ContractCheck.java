package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import java.util.Arrays;

/** Pure application contract checks; these do not replace real Framework scenario runs. */
public final class ContractCheck {
    public static void main(String[] args)throws Exception{
        checkPreparationRoles();
        new Config(Config.JSON.readTree("{\"workload\":{\"connections\":1000,\"logicalStreams\":1000,\"inflight\":2}}"));
        new Config(Config.JSON.readTree("{\"workload\":{\"logicalStreams\":8}}"));
        for(String key:new String[]{"connections","logicalStreams"}) {
            try { new Config(Config.JSON.readTree("{\"workload\":{\""+key+"\":1001}}"));throw new AssertionError("CCU above 1000 accepted"); }
            catch(IllegalArgumentException expected){}
        }
        for(int size:new int[]{64,4096})validatePayload(payload(size),size);
        try{validatePayload(payload(64),4096);throw new AssertionError("Wrong length accepted");}catch(Validation expected){}
        var histogram=new Histogram();histogram.record(1000);histogram.record(60_000_000_001L);
        if(Histogram.BOUNDS.length!=1797)throw new AssertionError("Histogram bucket count");
        try(var fixture=ContractCheck.class.getResourceAsStream("/histogram-bounds-ns.json")){
            if(fixture==null)throw new AssertionError("Shared histogram fixture missing");
            {var root=Config.JSON.readTree(fixture);var bounds=root.isArray()?root:root.path("boundsNs");
                if(bounds.size()!=Histogram.BOUNDS.length)throw new AssertionError("Shared fixture size");
                for(int i=0;i<bounds.size();i++)if(bounds.get(i).asLong()!=Histogram.BOUNDS[i])throw new AssertionError("Shared bound differs at "+i);
            }
        }
        if(!histogram.snapshot().get("overflow").equals("1"))throw new AssertionError("Overflow omitted");
        if(histogram.percentile(999)!=null)throw new AssertionError("Overflow percentile must be null");
        var dto=new PerfEchoRequest("run","cell","0","warmup",0,"1","cell/warmup/0/1",null,"1","clock",null,null,payload(64));
        if(!Config.JSON.readValue(Config.JSON.writeValueAsString(dto),PerfEchoRequest.class).equals(dto))throw new AssertionError("DTO roundtrip");
        if(!Arrays.equals(bytes(64),java.util.Base64.getDecoder().decode(dto.payload())))throw new AssertionError("Pattern differs");
        var evidence=new SequenceEvidence();evidence.attempt(0,"1");evidence.attempt(0,"3");evidence.admission(0,"1",true);
        if(!evidence.receipt(0,"1",false)||evidence.receipt(0,"1",true))throw new AssertionError("Duplicate receipt moved windows");
        if(evidence.unique()!=1)throw new AssertionError("Duplicate counted twice");
        var config=new Config(Config.JSON.readTree("""
            {"runId":"r","cellId":"c","configHash":"h","role":"channel","roleInstance":0,"source":true,
             "scenario":"channel-echo-only","provenance":{"comparisonKey":"comparison"},
             "workload":{"warmupSeconds":0.05,"durationSeconds":0.25,"settleTimeoutMs":1000}}
            """));
        try(var measurement=new Measurement(config)){
            measurement.start(Config.JSON.readTree("{\"runId\":\"r\",\"cellId\":\"c\",\"resetSeq\":\"0\",\"phase\":\"warmup\"}"),()->{long started=measurement.begin();measurement.complete(started,new java.util.concurrent.TimeoutException("public workload terminal"),true);});
            measurement.awaitPhase();
            if(!measurement.snapshot().toString().contains("java.util.concurrent.TimeoutException"))throw new AssertionError("Warmup terminal not preserved");
            if(!measurement.reset(Config.JSON.readTree("{\"runId\":\"r\",\"cellId\":\"c\",\"resetSeq\":\"1\"}")).get("ok").equals(true))throw new AssertionError("reset failed");
            measurement.start(Config.JSON.readTree("{\"runId\":\"r\",\"cellId\":\"c\",\"resetSeq\":\"1\",\"phase\":\"measured\"}"),()->{long started=measurement.begin();measurement.complete(started,null,true);});
            measurement.awaitPhase();
            var snapshot=Config.JSON.valueToTree(measurement.snapshot());
            if(!snapshot.path("comparisonKey").asText().equals("comparison"))throw new AssertionError("comparison key lost");
            var bins=snapshot.path("timeSeries");
            if(bins.size()!=3||bins.get(2).path("durationMs").asDouble()!=50)throw new AssertionError("time series partition");
            long sent=0,completed=0;for(var bin:bins){sent+=bin.path("counts").path("messages.sent").asLong();completed+=bin.path("counts").path("messages.completed").asLong();}
            if(snapshot.path("runtimeMetrics").path("cpuSamples").path("value").isEmpty())throw new AssertionError("Actual CPU samples missing");
            if(!snapshot.path("metrics").path("errors.language").isEmpty())throw new AssertionError("Measured language errors include warmup");
            if(sent!=1||completed!=1)throw new AssertionError("time series event conservation");
            if(args.length>0)Config.JSON.writeValue(java.nio.file.Path.of(args[0]).toFile(),snapshot);
        }
        System.out.println("Java perf DTO/payload/histogram/receipt/catalog/time-series checks passed");
    }

    private static <T> org.springframework.beans.factory.ObjectProvider<T> provider(T value){
        return new org.springframework.beans.factory.ObjectProvider<>(){
            @Override public T getObject(){return value;}
        };
    }
    private static void checkPreparationRoles() throws Exception {
        var actors=new java.util.ArrayList<String>();
        var publications=new java.util.concurrent.atomic.AtomicInteger();
        var manager=(systems.zlink.framework.actors.ZLinkActorManager)java.lang.reflect.Proxy.newProxyInstance(
            ContractCheck.class.getClassLoader(),new Class<?>[]{systems.zlink.framework.actors.ZLinkActorManager.class},
            (proxy,method,args)->{
                if(!method.getName().equals("getOrCreate"))throw new AssertionError("Unexpected actor manager call: "+method);
                String id=(String)args[0];
                if(!args[1].equals(Handlers.ACTOR_TYPE))throw new AssertionError("Actor factory type changed");
                return java.lang.reflect.Proxy.newProxyInstance(ContractCheck.class.getClassLoader(),
                    new Class<?>[]{systems.zlink.framework.actors.ZLinkActorGetOrCreateCall.class},(call,operation,values)->{
                        if(operation.getName().equals("request"))return call;
                        if(!operation.getName().equals("submit"))throw new AssertionError("Unexpected creation call: "+operation);
                        actors.add(id);
                        return java.util.concurrent.CompletableFuture.completedFuture(
                            new systems.zlink.framework.actors.ZLinkActorCreateResult.Existing(
                                new systems.zlink.framework.actors.ActorRef(id,1,"mesh",systems.zlink.contracts.core.RoutingId.from("target"))));
                    });
            });
        systems.zlink.framework.channels.ZLinkFanoutClient fanout=new systems.zlink.framework.channels.ZLinkFanoutClient(){
            public systems.zlink.framework.channels.ZLinkFanoutPublishCall publish(String channel,Object message){throw new AssertionError("Missing topic");}
            public systems.zlink.framework.channels.ZLinkFanoutPublishCall publish(String channel,String topic,Object message){
                return ()->{publications.incrementAndGet();return java.util.concurrent.CompletableFuture.completedFuture(null);};
            }
        };
        for(String role:new String[]{"none","actor","source"}){
            boolean source=role.equals("source"),objectServer=role.equals("actor");
            var root=Config.JSON.createObjectNode();root.put("runId","prepare");root.put("cellId","cell");root.put("configHash","hash");
            root.put("source",source);root.put("objectRole",objectServer?"server":"None");
            root.put("scenario",source?"pubsub-fanout-echo":"cs-remote-session-actor-echo");root.put("channelName","events");
            root.putArray("actorIds").add("actor-0").add("actor-1");
            root.putObject("workload").put("logicalStreams",2);
            var config=new Config(root);
            try(var measurement=new Measurement(config);var engine=new Engine(config,measurement,null,null,provider(manager),null,provider(fanout))){
                measurement.objectsReady=!objectServer;
                if(!engine.prepare().get("ok").equals(true)||!measurement.objectsReady)throw new AssertionError("Preparation failed");
                if(measurement.consumersReady!=source)throw new AssertionError("Target consumer evidence fabricated");
                engine.prepare();
            }
            if(actors.size()!=(role.equals("none")?0:2))throw new AssertionError("Role prepared unowned actors or duplicated preparation");
            if(publications.get()!=(source?2:0))throw new AssertionError("Probe executed on a target or duplicated");
        }
    }

}
