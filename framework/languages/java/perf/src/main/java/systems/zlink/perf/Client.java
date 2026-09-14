package systems.zlink.perf;

import static systems.zlink.perf.Contracts.*;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.*;
import java.net.URI;
import java.net.http.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.stream.connector.*;

public final class Client implements AutoCloseable {
    private final JsonNode manifest;
    private final Config config;
    private final Measurement metrics;
    private final ArrayList<ZLinkStreamConnector> connectors=new ArrayList<>();
    private final ArrayList<AtomicLong> sequences=new ArrayList<>();
    private final ExecutorService drivers=Executors.newVirtualThreadPerTaskExecutor();
    private final HttpClient http=HttpClient.newHttpClient();
    private final int first,count;
    private Client(JsonNode manifest,int index){
        this.manifest=manifest;
        int total=manifest.path("workload").path("connections").asInt(),clients=manifest.path("workload").path("clientCount").asInt();
        if(clients<1||index<0||index>=clients)throw new IllegalArgumentException("Invalid client index");
        count=total/clients+(index<total%clients?1:0);first=index*(total/clients)+Math.min(index,total%clients);
        ObjectNode node=manifest.deepCopy();node.put("role","client");node.put("roleInstance",index);node.put("source",true);node.put("objectRole","None");
        if(!node.has("scenario"))node.put("scenario","session-echo-only");
        if(!node.has("mode"))node.put("mode","request");
        config=new Config(node);metrics=new Measurement(config);
    }
    public static void run(String path,int index)throws Exception{
        try(Client client=new Client(Config.read(path).root(),index)){
            try{client.prepare();}catch(Exception error){client.metrics.diagnostic(error);}
            write(Measurement.map("type","prepared","ok",!client.metrics.hasErrors(),"snapshot",client.metrics.snapshot()));
            try(var input=new BufferedReader(new InputStreamReader(System.in))){
                String line;while((line=input.readLine())!=null){
                    try{
                        JsonNode command=Config.JSON.readTree(line),request=command.path("request");Object response;
                        switch(command.path("command").asText()){
                            case "start"->response=client.metrics.start(request,client::workload);
                            case "reset"->response=client.metrics.reset(request);
                            case "stats"->response=client.metrics.snapshot();
                            case "wait"->{client.metrics.awaitPhase();response=Measurement.map("ok",!client.metrics.hasErrors(),"phase",client.metrics.phase());}
                            case "triggerRoles"->response=client.roles("/app/perf/start",request);
                            case "resetRoles"->response=client.roles("/perf/reset",request);
                            case "stop"->{return;}
                            default->throw new IllegalArgumentException("Unknown client control command");
                        }
                        write(Measurement.map("ok",true,"response",response));
                    }catch(Exception error){client.metrics.diagnostic(error);write(Measurement.map("ok",false,"errorType",error.getClass().getName(),"message",String.valueOf(error.getMessage())));}
                }
            }
        }
    }
    private static void write(Object value)throws IOException{System.out.println(Config.JSON.writeValueAsString(value));System.out.flush();}
    private void prepare()throws Exception{
        JsonNode session=null;for(JsonNode role:manifest.path("roles"))if(role.path("streamEndpoint").isTextual()){session=role;break;}
        if(session==null)throw new IllegalArgumentException("STREAM role missing");
        URI endpoint=URI.create(session.path("streamEndpoint").asText());
        var timeout=Duration.ofMillis(config.number("requestTimeoutMs",5000));
        for(int i=0;i<count;i++){connectors.add(null);sequences.add(new AtomicLong());}
        var setupJobs=new ArrayList<CompletableFuture<Void>>();
        var permits=new Semaphore(config.number("connectConcurrency",256));
        final JsonNode sessionRole=session;
        for(int index=0;index<count;index++){
            final int i=index;
            setupJobs.add(CompletableFuture.runAsync(()->{
            boolean acquired=false;
            try{permits.acquire();acquired=true;
            var defaults=ZLinkStreamConnectorOptions.createDefault(endpoint);
            var options=new ZLinkStreamConnectorOptions(endpoint,ZLinkStreamDispatchMode.IMMEDIATE,timeout,timeout,0,
                Duration.ofMillis(config.number("setupTimeoutMs",30000)),defaults.maxSendPayloadSize(),defaults.maxReceivePayloadSize(),
                defaults.maxReceivedMessages(),defaults.maxInboundObserverNotifications(),defaults.maxInboundObserverPayloadPreviewBytes(),
                false,defaults.heartbeatInterval(),defaults.heartbeatTimeout(),false,defaults.reconnectInitialDelay(),
                defaults.reconnectMaxDelay(),defaults.reconnectBackoffFactor(),defaults.skipServerCertificateValidation(),
                ZLinkStreamCompression.NONE,null,defaults.nameResolver(),defaults.typedCodec(),ZLinkStreamDiagnosticsLevel.OFF);
            var connector=ZLinkStreamConnectorFactory.create(options);connectors.set(i,connector);
            connector.connect().submit().toCompletableFuture().get(config.number("setupTimeoutMs",30000),TimeUnit.MILLISECONDS);
            if(!config.scenario().equals("session-echo-only")){
                JsonNode ids=sessionRole.path("actorIds");if(ids.size()!=config.number("connections",1))throw new Validation("SetupIncomplete","One actor ID per connector required");
                String actor=ids.get(first+i).asText();
                var reply=connector.request(new PerfBindRequest(actor)).submit(PerfBindReply.class).toCompletableFuture().get(config.number("setupTimeoutMs",30000),TimeUnit.MILLISECONDS);
                if(!reply.actorId().equals(actor))throw new Validation("SetupIncomplete","Bound Actor ID differs");
            }
            var request=metrics.request(first+i,sequences.get(i).getAndIncrement(),true);
            var reply=connector.request(request).submit(PerfEchoReply.class).toCompletableFuture().get(config.number("setupTimeoutMs",30000),TimeUnit.MILLISECONDS);
            metrics.validateReply(request,reply);metrics.connected();
            }catch(Exception failure){metrics.connectionFailed();throw new CompletionException(failure);}
            finally{if(acquired)permits.release();}
            },drivers));
        }
        CompletableFuture.allOf(setupJobs.toArray(CompletableFuture[]::new)).get(config.number("setupTimeoutMs",30000),TimeUnit.MILLISECONDS);

        metrics.infrastructureReady=metrics.objectsReady=metrics.consumersReady=true;metrics.evidence(Measurement.map("kind","typedConnectorProbe","count",count,"firstConnectorId",first));
    }
    private void workload(){
        var jobs=new ArrayList<CompletableFuture<Void>>();
        for(int index=0;index<connectors.size();index++)for(int slot=0;slot<config.number("inflight",1);slot++){
            final int i=index;jobs.add(CompletableFuture.runAsync(()->{
                while(metrics.canIssue()){
                    var request=metrics.request(first+i,sequences.get(i).getAndIncrement(),false);
                    long started=metrics.begin();if(started==0)break;
                    Throwable failure=null;
                    try{
                        var reply=connectors.get(i).request(request).submit(PerfEchoReply.class).toCompletableFuture().get();
                        metrics.validateReply(request,reply);
                    }catch(Throwable e){failure=e;}
                    metrics.complete(started,failure,true);
                }
            },drivers));
        }
        CompletableFuture.allOf(jobs.toArray(CompletableFuture[]::new)).join();
    }
    private Object roles(String path,JsonNode request)throws Exception{
        var replies=new ArrayList<Object>();
        for(JsonNode role:manifest.path("roles")){
            String base=path.startsWith("/perf/")?role.path("metrics").path("baseUrl").asText():URI.create(role.path("applicationTriggerUrl").asText()).resolve("/").toString();
            var response=http.send(HttpRequest.newBuilder(URI.create(base).resolve(path)).header("Content-Type","application/json")
                .timeout(Duration.ofMillis(config.number("adminTimeoutMs",5000))).POST(HttpRequest.BodyPublishers.ofString(Config.JSON.writeValueAsString(request))).build(),HttpResponse.BodyHandlers.ofString());
            if(response.statusCode()!=200)throw new IOException("Role control rejected: "+response.body());replies.add(Config.JSON.readTree(response.body()));
        }
        return replies;
    }
    @Override public void close(){
        for(var connector:connectors){if(connector==null)continue;try{connector.close().submit().toCompletableFuture().join();}catch(CompletionException error){metrics.diagnostic(error);}}
        drivers.shutdownNow();metrics.close();
    }
}
