package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import com.fasterxml.jackson.databind.JsonNode;
import java.lang.management.ManagementFactory;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Cohort/correlation state belongs to this application, never to transport policy. */
public final class Measurement implements AutoCloseable {
    public static final String DOMAIN="java-process-"+ProcessHandle.current().pid();
    public final Config config;
    private final Map<Integer,Map<String,Long>> intervals=new TreeMap<>();
    private final Map<String,Long> counts=new LinkedHashMap<>(),directional=new LinkedHashMap<>();
    private final Map<String,Long> frameworkErrors=new LinkedHashMap<>(),harnessErrors=new LinkedHashMap<>(),languageErrors=new LinkedHashMap<>();
    private final List<Map<String,Object>> cpuSamples;
    private long cpuLast,cpuLastAt,cpuStartAt;
    private final List<Object> errors=new ArrayList<>(),setup=new ArrayList<>();
    private java.math.BigInteger workerIterations=java.math.BigInteger.ZERO;
    private WorkerObservation lastWorker;private long workerCount;
    private final Map<String,Histogram> hist=new LinkedHashMap<>();
    private final Map<String,Map<String,Object>> acks=new HashMap<>();
    private final Map<String,Pending> pending=new ConcurrentHashMap<>();
    private final ArrayDeque<Pending> retired=new ArrayDeque<>();
    private boolean sealed;
    private final SequenceEvidence sequenceEvidence=new SequenceEvidence();
    private final ExecutorService phases=Executors.newVirtualThreadPerTaskExecutor();
    private String phase="setup",resetSeq="0",startUnix,endUnix;
    private long start,end,settled,inflight,maxInflight,active,connected,connectionFailures,cpuStart,cpuEnd,rssMax;
    private Map<String,Object> resetAck;
    private CompletableFuture<Void> phaseTask=CompletableFuture.completedFuture(null);
    public volatile boolean infrastructureReady,objectsReady,consumersReady;
    public volatile Supplier<Object> publicStatus=()->null;
    public volatile Supplier<String> capacityReset;
    public Measurement(Config config) { this.config=config; cpuSamples=new ArrayList<>((int)Math.ceil(Math.max(config.seconds("warmupSeconds",2),config.seconds("durationSeconds",5))*10)+2); for(int i=0;i<(int)Math.ceil(Math.max(config.seconds("warmupSeconds",2),config.seconds("durationSeconds",5))*10);i++)intervals.put(i,new LinkedHashMap<>()); }
    public static Map<String,Object> map(Object... values) {
        var result=new LinkedHashMap<String,Object>();for(int i=0;i<values.length;i+=2)result.put((String)values[i],values[i+1]);return result;
    }
    public static Map<String,Object> reason(String code,String message) {return map("code",code,"reason",message,"owner","perf/README.ko.md");}
    public static String nowText(){return Long.toString(System.nanoTime());}
    public static String unix(){return Long.toString(System.currentTimeMillis());}
    public synchronized String phase(){return phase;}
    public synchronized String resetSeq(){return resetSeq;}
    public synchronized boolean canIssue(){return start!=0&&System.nanoTime()<end&&(phase.equals("warmup")||phase.equals("measured"));}
    public synchronized boolean hasErrors(){return !errors.isEmpty();}
    public synchronized void evidence(Object value){setup.add(value);}
    public synchronized void connected(){connected++;}
    public synchronized void connectionFailed(){connectionFailures++;}
    public synchronized void increment(String key){counts.merge(key,1L,Long::sum);if(key.equals("sent")&&config.echo()&&!config.spotDriver())interval("slo.eligible",System.nanoTime());}
    public synchronized void handlerEnter(){active++;}
    public synchronized void handlerExit(){active--;}
    public synchronized void diagnostic(Throwable failure){diagnostic(failure,false);}
    public synchronized void diagnostic(Throwable failure,boolean phaseFailure){if(phaseFailure||phase.equals("setup"))increment("phaseDiagnosticFailures");error(failure,false);}
    private static String canonicalKind(String name){var result=new StringBuilder();for(String part:name.toLowerCase(java.util.Locale.ROOT).split("_"))result.append(Character.toUpperCase(part.charAt(0))).append(part.substring(1));return result.toString();}
    private void error(Throwable failure,boolean outcome) {
        while((failure instanceof CompletionException||failure instanceof ExecutionException)&&failure.getCause()!=null)failure=failure.getCause();
        String category="failed",errorKey;
        if(failure instanceof ZLinkFrameworkException framework){String key=canonicalKind(framework.kind().toString());frameworkErrors.merge(key,1L,Long::sum);errorKey="errors.byKind."+key;}
        else if(failure instanceof Validation validation){harnessErrors.merge(validation.kind,1L,Long::sum);errorKey="errors.harness."+validation.kind;}
        else {languageErrors.merge(failure.getClass().getName(),1L,Long::sum);errorKey="errors.language."+failure.getClass().getName();}
        interval(errorKey,System.nanoTime());
        if(failure instanceof TimeoutException || failure instanceof ZLinkFrameworkException f&&f.kind().toString().equals("DEADLINE_EXCEEDED"))category="timeout";
        if(failure instanceof CancellationException)category="cancelled";
        if(outcome){increment(category);interval("messages."+category,System.nanoTime());}
        if(errors.size()<32)errors.add(map("errorType",failure.getClass().getName(),"errorMessage",String.valueOf(failure.getMessage())));
    }
    public synchronized PerfEchoRequest request(int stream,long sequence,boolean probe) {
        String p=probe||resetSeq.equals("0")?"warmup":"measured";
        String seq=Long.toUnsignedString(sequence);
        String returnSpot=config.sendSend()&&config.spotDriver()?config.list("spotIds").get(stream%config.list("spotIds").size()):null;
        String returnChannel=config.sendSend()&&!config.spotDriver()?config.returnChannel():null;
        return new PerfEchoRequest(config.text("runId"),config.text("cellId"),probe?"0":resetSeq,p,stream,seq,
            config.text("cellId")+"/"+p+"/"+stream+"/"+seq,null,nowText(),DOMAIN,returnSpot,returnChannel,payload(config.requestBytes()));
    }
    public void validate(PerfEchoRequest request) {
        if(request.phase().equals("warmup")&&!resetSeq().equals("0"))increment("load.lateWarmupMessages");
        if(!request.runId().equals(config.text("runId"))||!request.cellId().equals(config.text("cellId"))||request.clientId()<0
            ||!request.correlationId().equals(request.cellId()+"/"+request.phase()+"/"+request.clientId()+"/"+request.sequence()))
            throw new Validation("IdentityMismatch","Request identity differs");
        if(!request.resetSeq().equals(resetSeq())||!List.of("warmup","measured").contains(request.phase())
            ||request.phase().equals("warmup")!=request.resetSeq().equals("0"))throw new Validation("PhaseMismatch","Request phase differs");
        Long.parseUnsignedLong(request.sequence()); Long.parseLong(request.sentTicks());
        validatePayload(request.payload(),config.requestBytes());
    }
    public PerfEchoReply reply(PerfEchoRequest request) {
        validate(request); direction(config.sendSend()?"send":"reply");
        return new PerfEchoReply(request.runId(),request.cellId(),request.resetSeq(),request.phase(),request.clientId(),request.sequence(),
            request.correlationId(),nowText(),DOMAIN,payload(config.replyBytes()));
    }
    public void validateReply(PerfEchoRequest request,PerfEchoReply reply) {
        if(!request.runId().equals(reply.runId())||!request.cellId().equals(reply.cellId())||!request.resetSeq().equals(reply.resetSeq())
            ||!request.phase().equals(reply.phase())||request.clientId()!=reply.clientId()||!request.sequence().equals(reply.sequence())
            ||!request.correlationId().equals(reply.correlationId()))throw new Validation("IdentityMismatch","Reply identity differs");
        validatePayload(reply.payload(),config.replyBytes());
    }
    public synchronized void direction(String direction){long at=System.nanoTime();if(start!=0&&at<end){directional.merge(direction,1L,Long::sum);interval("applicationMessages."+direction,at);}}
    public synchronized long begin(){if(!canIssue())return 0;increment("sent");if(!config.spotDriver())interval("messages.sent",System.nanoTime());inflight++;maxInflight=Math.max(maxInflight,inflight);direction(config.publish()?"event":config.oneWay()||config.sendSend()?"send":"request");return System.nanoTime();}
    public synchronized void complete(long started,Throwable failure,boolean startedOperation){complete(started,failure,startedOperation,null);}
    public synchronized void complete(long started,Throwable failure,boolean startedOperation,String correlation){
        if(sealed)return;
        if(inflight>0)inflight--;
        if(!startedOperation){increment("driver.notStarted");counts.computeIfPresent("sent",(k,v)->v-1);return;}
        if(failure!=null){error(failure,true);return;}
        long now=System.nanoTime();long totalElapsed=now-started;
        Pending observed=correlation==null?null:pending.get(correlation);
        if((config.spotDriver()||config.sendSend())&&observed!=null&&observed.started>0&&observed.ended>0){started=observed.started;now=observed.ended;}
        boolean window=now<end;
        if(config.echo()){increment(window?"completed":"settleCompleted");interval(window?"messages.completed":"messages.settleCompleted",now);record(window?"latencyMs":"settleLatencyMs",now-started);
            if((config.spotDriver()?totalElapsed:now-started)<=config.number("applicationDeadlineMs",50)*1_000_000L){increment("slo.met");if(window)increment("slo.windowMet");}}
    }
    public synchronized void worker(WorkerObservation value){workerCount++;workerIterations=workerIterations.add(new java.math.BigInteger(value.iterations()));lastWorker=value;}
    public synchronized void record(String key,long duration){if(!key.equals("latencyMs")&&!key.equals("settleLatencyMs")&&(start==0||System.nanoTime()>=end))return;hist.computeIfAbsent(key,k->new Histogram()).record(duration);}
    public synchronized void admitted(PerfEchoRequest request,long started){
        increment(config.publish()?"published":"admitted");interval(config.publish()?"messages.publishedInWindow":"messages.admittedInWindow",System.nanoTime());
        if(!config.publish())increment(System.nanoTime()<end?"admittedInWindow":"settleAdmitted");
        if(config.publish())increment(System.nanoTime()<end?"publishedInWindow":"settlePublished");
        if(config.oneWay()||config.publish())sequenceEvidence.admission(request.clientId(),request.sequence(),System.nanoTime()<end);
        if(config.scenario().startsWith("actor-"))record("sourceAdmissionMs",System.nanoTime()-started);
        if(config.oneWay())record("sendAdmissionLatencyMs",System.nanoTime()-started);
    }
    public synchronized void attempt(int client,String sequence){sequenceEvidence.attempt(client,sequence);}
    public synchronized void delivered(PerfEchoRequest request){
        if(request.phase().equals("warmup")&&!resetSeq.equals("0")){increment("load.lateWarmupMessages");return;}
        validate(request);
        if(!request.resetSeq().equals(resetSeq))return;
        consumersReady=true;long at=System.nanoTime();if(request.phase().equals("warmup")||start==0||at<start||at>=end+config.number("settleTimeoutMs",5000)*1_000_000L)return;
        if(!sequenceEvidence.receipt(request.clientId(),request.sequence(),System.nanoTime()<end))increment("delivery.duplicates");
        else {increment(System.nanoTime()<end?"delivery.inWindow":"delivery.settle");interval("send.deliveredInWindow",System.nanoTime());}
        consumersReady=true;
    }
    public synchronized void delivered(PerfPublishEvent event){
        if(event.phase().equals("warmup")&&!resetSeq.equals("0")){increment("load.lateWarmupMessages");return;}
        if(!event.runId().equals(config.text("runId"))||!event.cellId().equals(config.text("cellId"))||!event.resetSeq().equals(resetSeq))throw new Validation("IdentityMismatch","Event cohort differs");
        validatePayload(event.payload(),config.number("sendPayloadBytes",4096));
        consumersReady=true;long at=System.nanoTime();if(event.phase().equals("warmup")||start==0||at<start||at>=end+config.number("settleTimeoutMs",5000)*1_000_000L)return;
        if(!sequenceEvidence.receipt(0,event.sequence(),System.nanoTime()<end))increment("fanout.duplicateEvents");
        else {increment(System.nanoTime()<end?"fanout.deliveredInWindow":"fanout.settleDelivered");interval("fanout.deliveredInWindow",System.nanoTime());}
        consumersReady=true;
    }
    public synchronized CompletableFuture<PerfEchoReply> register(PerfEchoRequest request){
        long now=System.nanoTime();while(!retired.isEmpty()&&retired.peek().expiresAt<=now){Pending old=retired.remove();pending.remove(old.request.correlationId(),old);}
        Pending value=new Pending(request,new CompletableFuture<>());
        value.deadline=now+config.number("correlationExpiryMs",1000)*1_000_000L;
        if(pending.putIfAbsent(request.correlationId(),value)!=null)throw new Validation("IdentityMismatch","Duplicate correlation registration");
        return value.future;
    }
    public synchronized void returned(PerfEchoReply reply){
        if(reply.phase().equals("warmup")&&!resetSeq.equals("0")){increment("load.lateWarmupMessages");return;}
        if(sealed)return;
        Pending p=pending.get(reply.correlationId());
        if(p==null){increment("unknownCorrelation");return;}
        if(p.expired){increment("lateReply");return;}
        if(config.sendSend()&&!p.future.isDone()&&System.nanoTime()>=p.deadline){expired(reply.correlationId());increment("lateReply");return;}
        try{validateReply(p.request,reply);if(!p.future.isDone())p.ended=System.nanoTime();if(!p.future.complete(reply))increment("duplicateReply");}
        catch(Throwable failure){p.future.completeExceptionally(failure);}
    }
    public synchronized void unregister(String correlation){
        Pending p=pending.get(correlation);if(p==null||p.retired)return;
        if(!config.sendSend()){pending.remove(correlation);return;}
        p.retired=true;p.expiresAt=System.nanoTime()+config.number("correlationExpiryMs",1000)*1_000_000L;retired.add(p);
    }
    public long correlationRemaining(String correlation){Pending p=pending.get(correlation);return p==null?1:Math.max(1,p.deadline-System.nanoTime());}
    public synchronized void expired(String correlation){Pending p=pending.get(correlation);if(p!=null&&!p.future.isDone()){p.expired=true;p.future.completeExceptionally(new Validation("CorrelationExpired","Send/send echo deadline elapsed"));increment("expired");}}
    private static final class Pending {
        final PerfEchoRequest request;final CompletableFuture<PerfEchoReply> future;volatile long started=System.nanoTime(),ended,expiresAt,deadline;boolean expired,retired,primaryObserved;
        Pending(PerfEchoRequest request,CompletableFuture<PerfEchoReply> future){this.request=request;this.future=future;}
    }
    public synchronized void primaryInterval(String correlation,long started,long ended){Pending p=pending.get(correlation);if(p!=null){if(!p.primaryObserved){interval("messages.sent",started);if(config.echo()&&config.spotDriver())interval("slo.eligible",started);p.primaryObserved=true;}p.started=started;p.ended=ended;}}
    public Map<String,Object> ready(){Object observed=publicStatus.get();return ready(observed);}
    private synchronized Map<String,Object> ready(Object observed){
        boolean ready=infrastructureReady&&objectsReady&&consumersReady&&!hasErrors();
        var evidence=new ArrayList<>(setup);evidence.add(map("kind","publicStatus","observedValue",observed));
        return map("runId",config.text("runId"),"cellId",config.text("cellId"),"role",config.text("role"),"roleInstance",config.instance(),
            "infrastructureReady",infrastructureReady,"objectsReady",objectsReady,"consumersReady",consumersReady,"ready",ready,
            "observedAtUnixMs",unix(),"evidence",evidence,"reasons",ready?List.of():List.of("Public infrastructure, objects or typed probe is pending"));
    }
    public synchronized Map<String,Object> start(JsonNode request,Runnable workload){
        String requestedPhase=request.path("phase").asText();String key=requestedPhase+"/"+request.path("resetSeq").asText();
        if(acks.containsKey(key))return acks.get(key);
        boolean identity=request.path("runId").asText().equals(config.text("runId"))&&request.path("cellId").asText().equals(config.text("cellId"))&&request.path("resetSeq").asText().equals(resetSeq);
        boolean allowed=requestedPhase.equals("warmup")?phase.equals("setup")&&resetSeq.equals("0"):requestedPhase.equals("measured")&&phase.equals("reset")&&!resetSeq.equals("0");
        boolean accepted=identity&&allowed&&phaseTask.isDone()&&inflight==0&&active==0;
        var reply=map("runId",config.text("runId"),"cellId",config.text("cellId"),"resetSeq",resetSeq,"phase",requestedPhase,"accepted",accepted,
            "state",accepted?"started":"rejected","configHash",config.text("configHash"),"reason",accepted?null:"Identity, phase or drain differs");
        if(!accepted)return reply;
        sealed=false;phase=requestedPhase;start=System.nanoTime();end=start+(long)(config.seconds(phase.equals("warmup")?"warmupSeconds":"durationSeconds",phase.equals("warmup")?2:5)*1e9);
        startUnix=unix();cpuStart=cpuLast=cpu();cpuStartAt=cpuLastAt=System.nanoTime();cpuSamples.clear();rssMax=rss();acks.put(key,reply);
        phaseTask=CompletableFuture.runAsync(()->runPhase(workload),phases);
        return reply;
    }
    private void runPhase(Runnable workload){
        CompletableFuture<Void> operations=CompletableFuture.runAsync(workload,phases);
        try{
            while(System.nanoTime()<end){TimeUnit.NANOSECONDS.sleep(Math.min(100_000_000,Math.max(1,end-System.nanoTime())));sample();}
            sample();
            synchronized(this){endUnix=unix();cpuEnd=cpuLast;phase="settle";}
            operations.get(Math.max(1,end+config.number("settleTimeoutMs",5000)*1_000_000L-System.nanoTime()),TimeUnit.NANOSECONDS);
            if(!config.source()&&(config.oneWay()||config.publish())){long remaining=end+config.number("settleTimeoutMs",5000)*1_000_000L-System.nanoTime();if(remaining>0)TimeUnit.NANOSECONDS.sleep(remaining);}
        }catch(Throwable failure){diagnostic(failure,true);}
        synchronized(this){settled=System.nanoTime();counts.put("unresolved",inflight);sealed=config.source();phase="complete";}
    }
    public void awaitPhase(){phaseTask.join();}
    public synchronized Map<String,Object> reset(JsonNode request){
        String seq=request.path("resetSeq").asText();
        if(resetAck!=null&&seq.equals(resetSeq)&&inflight==0&&active==0)return resetAck;
        boolean valid=request.path("runId").asText().equals(config.text("runId"))&&request.path("cellId").asText().equals(config.text("cellId"))
            &&Long.parseUnsignedLong(seq)>Long.parseUnsignedLong(resetSeq)&&phase.equals("complete")&&phaseTask.isDone()&&inflight==0&&active==0&&harnessErrors.isEmpty()&&counts.getOrDefault("phaseDiagnosticFailures",0L)==0&&counts.getOrDefault("load.lateWarmupMessages",0L)==0;
        String epoch=null;if(valid&&capacityReset!=null)epoch=capacityReset.get();
        var result=map("ok",valid,"runId",config.text("runId"),"cellId",config.text("cellId"),"role",config.text("role"),"roleInstance",config.instance(),
            "resetSeq",seq,"applicationResetAtUnixMs",unix(),"capacityEpoch",epoch,"reason",valid?null:"Previous phase not drained, identity mismatch or errors",
            "nullReasons",epoch==null?map("/capacityEpoch",reason("NOT_APPLICABLE","Client owns no Framework host")):Map.of());
        if(valid){cpuSamples.clear();frameworkErrors.clear();harnessErrors.clear();languageErrors.clear();errors.clear();counts.clear();intervals.values().forEach(Map::clear);directional.clear();hist.clear();workerCount=0;workerIterations=java.math.BigInteger.ZERO;lastWorker=null;sequenceEvidence.clear();pending.clear();retired.clear();sealed=false;start=end=settled=0;startUnix=endUnix=null;maxInflight=0;resetSeq=seq;phase="reset";resetAck=result;}
        return result;
    }
    private synchronized void sample(){
        long current=cpu(),at=System.nanoTime(),span=at-cpuLastAt,delta=current-cpuLast;
        if(span<=0||delta<0)throw new Validation("CpuSampleInvalid","CPU sampling requires positive elapsed time and nonnegative cumulative delta");
        if(cpuLastAt>=start&&cpuLastAt<end)cpuSamples.add(map("binIndex",(int)((cpuLastAt-start)/100_000_000L),"startOffsetMs",(cpuLastAt-start)/1e6,"endOffsetMs",(at-start)/1e6,"observedDurationNs",Long.toString(span),"cpuDeltaNs",Long.toString(delta)));
        cpuLast=current;cpuLastAt=at;rssMax=Math.max(rssMax,rss());
    }
    private Double binCpu(int index){long span=0,delta=0;for(var s:cpuSamples)if(((Number)s.get("binIndex")).intValue()==index){span=Math.addExact(span,Long.parseLong((String)s.get("observedDurationNs")));delta=Math.addExact(delta,Long.parseLong((String)s.get("cpuDeltaNs")));}return span==0?null:100.0*delta/span;}
    private static long cpu(){return ManagementFactory.getPlatformMXBean(com.sun.management.OperatingSystemMXBean.class).getProcessCpuTime();}
    private static long rss(){
        try{String stat=java.nio.file.Files.readString(java.nio.file.Path.of("/proc/self/status"));for(String line:stat.split("\n"))if(line.startsWith("VmRSS:"))return Long.parseLong(line.trim().split("\\s+")[1])*1024;}
        catch(java.io.IOException e){return 0;}return 0;
    }
    public Map<String,Object> snapshot(){Object status=publicStatus.get();return snapshot(status);}
    private synchronized Map<String,Object> snapshot(Object status){
        var m=new LinkedHashMap<String,Object>();var h=new LinkedHashMap<String,Object>();var n=new LinkedHashMap<String,Object>();var runtime=new LinkedHashMap<String,Object>();
        boolean primary=config.source();double seconds=start==0?0:(end-start)/1e9;
        for(String k:List.of("sent","completed","settleCompleted","failed","timeout","cancelled","unresolved","admitted","expired","duplicateReply","lateReply","unknownCorrelation","published","publishedInWindow","settlePublished")){
            boolean applicable=primary&&switch(k){case "completed","settleCompleted"->config.echo();case "admitted"->config.oneWay()||config.sendSend();case "expired","duplicateReply","lateReply","unknownCorrelation"->config.sendSend();case "published","publishedInWindow","settlePublished"->config.publish();default->true;};
            if(applicable)m.put("messages."+k,Long.toString(counts.getOrDefault(k,0L)));else nil(m,n,"metrics","messages."+k,"NOT_APPLICABLE","Outcome belongs to the applicable source");
        }
        String[] pairs={"latencyMs:latency","settleLatencyMs:settle.latency","sourceAdmissionMs:actor.sourceAdmission.latency","driverLatencyMs:driver.latency","workerCallLatencyMs:worker.callLatency","workerSubmitToStartMs:worker.submitToStart","workerTaskLatencyMs:worker.taskLatency","workerResultToContinuationMs:worker.resultToContinuation"};
        for(String pair:pairs){String[] p=pair.split(":");if(hist.containsKey(p[0])||primary&&config.echo()&&(p[0].equals("latencyMs")||p[0].equals("settleLatencyMs")))hist.computeIfAbsent(p[0],k->new Histogram()).export(p[1],p[0],m,h,n);else{
            nil(h,n,"histograms",p[0],"NOT_APPLICABLE","No applicable interval on this role");for(String s:List.of("meanMs","p50Ms","p95Ms","p99Ms","p999Ms","maxMs"))nil(m,n,"metrics",p[1]+"."+s,"NOT_APPLICABLE","No applicable interval on this role");}}
        for(String k:List.of("spot.mailboxDepth.max","spot.mailboxDepth.mean","spot.suspendedTurns","spot.resumedTurns","spot.resumeLatency.p95Ms","spot.resumeLatency.p99Ms","worker.pool.queueDepth.max","worker.pool.queueDepth.mean","host.queueWaitLatency.p50Ms","host.queueWaitLatency.p95Ms","host.queueWaitLatency.p99Ms"))nil(m,n,"metrics",k,"PUBLIC_OBSERVATION_UNSUPPORTED","No public timestamp or counter hook");
        for(String k:List.of("spot.applicationYieldCalls","spot.applicationHandlerEntries","driver.issued","driver.notStarted","driver.failed"))if((k.startsWith("driver.")&&config.spotDriver())||(k.startsWith("spot.")&&(config.spotDriver()||config.worker()||config.scenario().equals("spot-no-await-echo"))))m.put(k,Long.toString(counts.getOrDefault(k,0L)));else nil(m,n,"metrics",k,"NOT_APPLICABLE","No corresponding driver or Spot handler");
        m.put("load.lateWarmupMessages",Long.toString(counts.getOrDefault("load.lateWarmupMessages",0L)));
        if(primary&&(config.oneWay()||config.sendSend())){m.put("messages.admittedInWindow",Long.toString(counts.getOrDefault("admittedInWindow",0L)));m.put("messages.settleAdmitted",Long.toString(counts.getOrDefault("settleAdmitted",0L)));}
        for(String d:List.of("request","send","reply","event")){long count=directional.getOrDefault(d,0L);int bytes=d.equals("reply")?config.replyBytes():d.equals("request")?config.number("requestPayloadBytes",64):config.number("sendPayloadBytes",4096);m.put("applicationMessages."+d,Long.toString(count));m.put("applicationPayloadBytes."+d,Long.toString(count*bytes));}
        for(String k:List.of("connections.requested","connections.connected","connections.failed")){
            if(config.cs()&&primary)m.put(k,Long.toString(k.endsWith("failed")?connectionFailures:k.endsWith("requested")?config.number("connections",1)/config.number("clientCount",1)+(config.instance()<config.number("connections",1)%config.number("clientCount",1)?1:0):connected));else nil(m,n,"metrics",k,"NOT_APPLICABLE","Role owns no connectors");}
        m.put("load.logicalStreams",primary&&!config.cs()?Integer.toString(config.streams()):null);m.put("load.inflightPerStream",primary?Integer.toString(config.number("inflight",1)):null);m.put("load.inflight.max",primary?Long.toString(maxInflight):null);
        m.put("throughput.kops",primary&&config.echo()&&seconds>0?counts.getOrDefault("completed",0L)/seconds/1000:null);
        long messages=directional.values().stream().mapToLong(Long::longValue).sum();
        long bytes=0;for(String d:List.of("request","send","reply","event"))bytes+=Long.parseLong((String)m.get("applicationPayloadBytes."+d));
        m.put("throughput.messagesPerSec",seconds>0?messages/seconds:null);m.put("throughput.megabytesPerSec",seconds>0?bytes/seconds/1048576:null);
        long eligible=counts.getOrDefault("sent",0L),met=counts.getOrDefault("slo.met",0L);
        if(primary&&config.echo()){m.put("slo.eligible",Long.toString(eligible));m.put("slo.met",Long.toString(met));m.put("slo.missed",Long.toString(eligible-met));m.put("slo.missRatio",eligible==0?null:(double)(eligible-met)/eligible);m.put("slo.goodputOpsPerSec",seconds>0?counts.getOrDefault("slo.windowMet",0L)/seconds:null);if(eligible==0)n.put("/metrics/slo.missRatio",reason("ZERO_DENOMINATOR","No issued operations"));}
        m.put("errors.byKind",strings(frameworkErrors));m.put("errors.harness",strings(harnessErrors));m.put("errors.language",strings(languageErrors));
        m.put("process.cpuPercent",phase.equals("complete")&&seconds>0?(cpuEnd-cpuStart)/(double)(cpuLastAt-cpuStartAt)*100:null);m.put("process.rssMb",rssMax>0?rssMax/1048576.0:null);
        nil(m,n,"metrics","process.allocatedMb","PUBLIC_OBSERVATION_UNSUPPORTED","No process-wide JVM allocated-byte counter enabled");
        for(String k:List.of("gc.gen0","gc.gen1","gc.gen2"))nil(m,n,"metrics",k,"NOT_APPLICABLE","JVM collectors do not share .NET generation semantics");
        if(config.publish()){m.put("fanout.uniqueDelivered",Long.toString(sequenceEvidence.unique()));for(String k:List.of("fanout.deliveredInWindow","fanout.settleDelivered","fanout.duplicateEvents","fanout.outOfCohortEvents"))m.put(k,Long.toString(counts.getOrDefault(k,0L)));}
        for(String k:List.of("fanout.deliveryLatency","fanout.settleDeliveryLatency","delivery.latency"))for(String s:List.of("meanMs","p50Ms","p95Ms","p99Ms","p999Ms","maxMs"))nil(m,n,"metrics",k+"."+s,"CLOCK_DOMAIN_UNVERIFIED","System.nanoTime is process scoped");
        runtime.put("cpuObservationSeconds",map("name","ProcessCpuTime actual observation span","unit","s","type","number","value",start==0?0:(cpuLastAt-cpuStartAt)/1e9));runtime.put("cpuSamples",observation("actual CPU spans, assigned by span start","array",new ArrayList<>(cpuSamples)));runtime.put("phaseDiagnosticFailures",observation("phaseDiagnosticFailures","integer",Long.toString(counts.getOrDefault("phaseDiagnosticFailures",0L))));
        runtime.put("errors",observation("firstErrors","array",new ArrayList<>(errors)));runtime.put("setupEvidence",observation("setupEvidence","array",new ArrayList<>(setup)));runtime.put("activeHandlers",observation("activeHandlers","integer",Long.toString(active)));
        if(config.oneWay()){runtime.put("sourceEvidence",observation("sourceEvidence","array",sequenceEvidence.sources()));runtime.put("deliveryEvidence",observation("deliveryEvidence","array",sequenceEvidence.receivers()));}
        if(config.publish()){var evidence=config.source()?sequenceEvidence.publisher():sequenceEvidence.subscriber(config.instance());evidence.putAll(map("runId",config.text("runId"),"cellId",config.text("cellId"),"resetSeq",resetSeq,"phase",resetSeq.equals("0")?"warmup":"measured"));String key=config.source()?"publisherSequences":"subscriberSequences";runtime.put(key,observation(key,"object",evidence));}
        if(config.worker())runtime.put("workerObservations",observation("workerObservations","object",map("count",Long.toString(workerCount),"iterations",workerIterations.toString(),"last",lastWorker)));
        runtime.put("deliveryCounters",observation("deliveryCounters","object",strings(counts)));
        var window=map("startedAtUnixMs",startUnix,"endedAtUnixMs",endUnix,"startTicks",start==0?null:Long.toString(start),"endTicks",end==0?null:Long.toString(end),"measuredSeconds",seconds==0?null:seconds,"settleSeconds",settled==0?null:Math.max(0,settled-end)/1e9);
        var clock=map("source","System.nanoTime","nativeFrequencyHz","1000000000","ticksUnit","ns","clockDomainId",DOMAIN,"scope","process","alignmentMethod",null,"maxErrorNs",null,"validFromTicks",null,"validThroughTicks",null,"evidence",List.of("JVM System.nanoTime process-local monotonic clock"));
        var serialized=new ArrayList<Object>();serialized.add(map("direction",config.publish()?"event":config.echo()&&!config.sendSend()?"request":"send","packetName",config.publish()?"PerfPublishEvent":"PerfEchoRequest","logicalPayloadBytes",Integer.toString(config.requestBytes()),"observedSerializedBytes",null));
        if(config.echo())serialized.add(map("direction","reply","packetName","PerfEchoReply","logicalPayloadBytes",Integer.toString(config.replyBytes()),"observedSerializedBytes",null));
        var provenance=Config.JSON.convertValue(config.root().path("provenance"),new com.fasterxml.jackson.core.type.TypeReference<LinkedHashMap<String,Object>>() {});provenance.put("pid",ProcessHandle.current().pid());provenance.put("primaryEchoOwner",primary&&config.echo());provenance.put("runtimeVersion",System.getProperty("java.version"));provenance.put("executor",map("name","Java virtual threads","effectiveProcessorCount",Runtime.getRuntime().availableProcessors()));provenance.put("resetAcknowledgement",resetAck);provenance.put("workerOptions",map("minThreads",config.number("workerPoolSize",8),"maxThreads",config.number("workerPoolSize",8),"idleTimeoutMs",60000,"maxQueueLength",null));
        if(config.oneWay()){m.put("send.admissionOpsPerSec",primary&&seconds>0?counts.getOrDefault("admittedInWindow",0L)/seconds:null);m.put("send.uniqueDelivered",Long.toString(sequenceEvidence.unique()));m.put("send.deliveredInWindow",Long.toString(counts.getOrDefault("delivery.inWindow",0L)));m.put("send.settleDelivered",Long.toString(counts.getOrDefault("delivery.settle",0L)));m.put("send.duplicateMessages",Long.toString(counts.getOrDefault("delivery.duplicates",0L)));}
        if(config.publish()){m.put("fanout.subscriberCount",Integer.toString(config.number("subscriberCount",8)));m.put("fanout.publishOpsPerSec",primary&&seconds>0?counts.getOrDefault("publishedInWindow",0L)/seconds:null);}
        for(String prefix:List.of("send.admissionLatency","send.deliveryLatency","load.scheduleLag","load.scheduledLatency"))for(String suffix:List.of("meanMs","p50Ms","p95Ms","p99Ms","p999Ms","maxMs"))if(!m.containsKey(prefix+"."+suffix))nil(m,n,"metrics",prefix+"."+suffix,prefix.equals("send.deliveryLatency")&&config.oneWay()?"CLOCK_DOMAIN_UNVERIFIED":"NOT_APPLICABLE","No applicable timing sample");
        for(String key:List.of("fanout.subscriberCount","fanout.uniqueDelivered","fanout.deliveredInWindow","fanout.settleDelivered","fanout.duplicateEvents","fanout.outOfCohortEvents","fanout.deliveryRatio","fanout.publishOpsPerSec","fanout.deliveryOpsPerSec","slo.eligible","slo.met","slo.missed","slo.missRatio","slo.goodputOpsPerSec","send.admissionOpsPerSec","send.deliveryOpsPerSec","send.deliveryRatio"))if(!m.containsKey(key))nil(m,n,"metrics",key,"NOT_APPLICABLE","No role-owned aggregate; joined delivery evidence is computed by the runner");
        if(config.oneWay()&&primary&&hist.containsKey("sendAdmissionLatencyMs"))hist.get("sendAdmissionLatencyMs").export("send.admissionLatency","sendAdmissionLatencyMs",m,h,n);
        for(String suffix:List.of("meanMs","p50Ms","p95Ms","p99Ms","p999Ms","maxMs")){if(config.scenario().equals("s2s-spot-to-channel-request-echo")&&primary){m.put("spot.remoteCallLatency."+suffix,m.get("latency."+suffix));if(m.get("latency."+suffix)==null)n.put("/metrics/spot.remoteCallLatency."+suffix,n.get("/metrics/latency."+suffix));}else nil(m,n,"metrics","spot.remoteCallLatency."+suffix,"NOT_APPLICABLE","No remote Spot request");}
        for(String key:List.of("fanoutDeliveryLatencyMs","fanoutSettleDeliveryLatencyMs","sendAdmissionLatencyMs","sendDeliveryLatencyMs","scheduleLagMs","scheduledLatencyMs"))if(!h.containsKey(key))nil(h,n,"histograms",key,"NOT_APPLICABLE","No applicable public interval");
        var result=map("schemaVersion",3,"runId",config.text("runId"),"cellId",config.text("cellId"),"resetSeq",resetSeq,"language","java","role",config.text("role"),"roleInstance",config.instance(),"configHash",config.text("configHash"),"comparisonKey",config.root().path("provenance").path("comparisonKey").asText(),"phase",phase,"window",window,"timeSeries",timeSeries(),"clock",clock,"serializedMessageBytes",serialized,"metrics",m,"histograms",h,"nullReasons",n,"publicStatus",status,"publicMetrics",List.of(),"runtimeMetrics",runtime,"provenance",provenance);
        MetricCatalog.complete(m,h,n);nullPaths(result,"",n);return result;
    }
    private synchronized void interval(String key,long at){
        if(start==0||at<start||at>=end)return;
        int index=(int)((at-start)/100_000_000L);
        intervals.computeIfAbsent(index,k->new LinkedHashMap<>()).merge(key,1L,Long::sum);
    }
    private List<Object> timeSeries(){
        var result=new ArrayList<Object>();if(start==0)return result;
        long duration=end-start;
        for(int index=0;index*100_000_000L<duration;index++)result.add(map("offsetMs",index*100.0,"durationMs",Math.min(100_000_000L,duration-index*100_000_000L)/1e6,
            "counts",strings(intervals.getOrDefault(index,Map.of())),"cpuPercent",binCpu(index),"nullReasons",binCpu(index)==null?map("/cpuPercent",reason("NO_SAMPLES","No actual CPU sample span starts in this bin")):Map.of()));
        return result;
    }
    private static Map<String,String> strings(Map<String,Long> source){var r=new LinkedHashMap<String,String>();source.forEach((k,v)->r.put(k,Long.toString(v)));return r;}
    private static Object observation(String name,String type,Object value){return map("name",name,"unit","observation","type",type,"value",value);}
    private static void nil(Map<String,Object> values,Map<String,Object> reasons,String container,String key,String code,String message){values.put(key,null);reasons.put("/"+container+"/"+key,reason(code,message));}
    private static void nullPaths(Object value,String path,Map<String,Object> reasons){
        if(value==null){reasons.putIfAbsent(path,reason("NOT_APPLICABLE","No applicable public observation"));return;}
        if(value instanceof Map<?,?> map){for(var e:map.entrySet())if(!e.getKey().equals("nullReasons"))nullPaths(e.getValue(),path+"/"+e.getKey(),reasons);}
        else if(value instanceof List<?> list)for(int i=0;i<list.size();i++)nullPaths(list.get(i),path+"/"+i,reasons);
    }
    @Override public void close(){phases.shutdownNow();}
}
