package systems.zlink.perf;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.net.*;
import java.io.*;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;

/** Admin and application triggers use separate loopback listeners, outside measured calls. */
public final class Admin implements AutoCloseable {
    private final HttpServer admin,application;
    private final java.util.concurrent.ExecutorService executor=Executors.newVirtualThreadPerTaskExecutor();
    public Admin(Config config,Measurement metrics,Engine engine,BooleanSupplier infrastructure)throws IOException{
        URI adminUri=URI.create(config.text("metricsUrl")),appUri=URI.create(config.text("applicationTriggerUrl"));
        if(adminUri.getPort()==appUri.getPort())throw new IllegalArgumentException("Separate admin/application listener required");
        admin=HttpServer.create(new InetSocketAddress("127.0.0.1",adminUri.getPort()),0);
        application=HttpServer.create(new InetSocketAddress("127.0.0.1",appUri.getPort()),0);
        admin.setExecutor(executor);application.setExecutor(executor);
        admin.createContext("/perf/ready",exchange->handle(exchange,()->{metrics.infrastructureReady=infrastructure.getAsBoolean();return metrics.ready();}));
        admin.createContext("/perf/stats",exchange->handle(exchange,metrics::snapshot));
        admin.createContext("/perf/reset",exchange->handle(exchange,()->metrics.reset(read(exchange))));
        application.createContext("/app/perf/prepare",exchange->handle(exchange,()->{
            var request=read(exchange);
            if(!request.path("runId").asText().equals(config.text("runId"))||!request.path("cellId").asText().equals(config.text("cellId"))||!request.path("resetSeq").asText().equals("0"))throw new IllegalArgumentException("Prepare identity differs");
            return engine.prepare();
        }));
        application.createContext("/app/perf/start",exchange->handle(exchange,()->metrics.start(read(exchange),config.source()?engine::run:()->{})));
    }
    private static com.fasterxml.jackson.databind.JsonNode read(HttpExchange exchange)throws IOException{return Config.JSON.readTree(exchange.getRequestBody());}
    @FunctionalInterface private interface Handler{Object get()throws Exception;}
    private static void handle(HttpExchange exchange,Handler handler)throws IOException{
        Object value;int status=200;
        try{value=handler.get();if(value instanceof java.util.Map<?,?> map&&(Boolean.FALSE.equals(map.get("ok"))||Boolean.FALSE.equals(map.get("accepted"))))status=409;}
        catch(Exception failure){status=500;value=Measurement.map("ok",false,"errorType",failure.getClass().getName(),"reason",String.valueOf(failure.getMessage()));}
        byte[] body=Config.JSON.writeValueAsBytes(value);exchange.getResponseHeaders().set("Content-Type","application/json");exchange.sendResponseHeaders(status,body.length);
        try(var output=exchange.getResponseBody()){output.write(body);}
    }
    public void start(){admin.start();application.start();}
    @Override public void close(){admin.stop(0);application.stop(0);executor.shutdownNow();}
}
